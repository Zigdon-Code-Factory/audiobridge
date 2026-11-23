"""Export Inworld Voice Isolator Realtime (voice-isolator-v1-realtime) to a fixed-shape FP16
streaming ONNX graph for DirectML GPU inference in AudioBridge.

Requires (build/export time only; no Python is used at runtime):
    pip install torch safetensors onnx

Usage:
    python windows/export_inworld_model.py windows/build/voice-models/inworld_realtime_fp16.onnx
"""
import hashlib
import json
import math
import pathlib
import struct
import sys
import urllib.request

import torch
import torch.nn as nn
import torch.nn.functional as F
from safetensors.torch import load_file

WEIGHTS_URL = (
    "https://huggingface.co/inworld/voice-isolator/resolve/"
    "9b0285b3fe8403aa8e2f1dbb2165a80a4294c970/denoiser_realtime.safetensors"
)
WEIGHTS_SHA256 = "76598f05523c0669e76562a8b2f31c9d395fca316e80c907ee11775e7497ae0f"
LICENSE_URL = "https://raw.githubusercontent.com/inworld-ai/voice-isolator/main/LICENSE"


def read_config(path: pathlib.Path) -> dict:
    with open(path, "rb") as f:
        hdr_len = struct.unpack("<Q", f.read(8))[0]
        hdr = json.loads(f.read(hdr_len).decode("utf-8"))
    return json.loads(hdr["__metadata__"]["config"])


def band_edges(cfg: dict) -> list[tuple[int, int]]:
    n_bins = cfg["n_fft"] // 2 + 1
    hz_per_bin = cfg["sample_rate"] / cfg["n_fft"]
    edges = []
    start = 0
    for width_hz, upto_hz in cfg["band_plan"]:
        w = max(1, round(width_hz / hz_per_bin))
        upto = min(n_bins, round(upto_hz / hz_per_bin))
        while start < upto:
            end = min(start + w, upto)
            edges.append((start, end))
            start = end
    if start < n_bins:
        edges.append((start, n_bins))
    return edges


def rope_tables(frames: int, head_dim: int):
    half = head_dim // 2
    inv = 10000.0 ** (-torch.arange(0, half, dtype=torch.float32) / half)
    pos = torch.arange(frames, dtype=torch.float32)
    angles = torch.outer(pos, inv)
    return angles.cos(), angles.sin()


def apply_rope(x: torch.Tensor, cos: torch.Tensor, sin: torch.Tensor) -> torch.Tensor:
    half = x.shape[-1] // 2
    x1, x2 = x[..., :half], x[..., half:]
    return torch.cat((x1 * cos - x2 * sin, x1 * sin + x2 * cos), dim=-1)


def parallel_prefix_scan(a: torch.Tensor, b: torch.Tensor) -> torch.Tensor:
    """Exact Hillis-Steele associative scan over 46 bands in 6 parallel GPU passes."""
    length = int(a.shape[1])
    step = 1
    while step < length:
        a_prev, b_prev = a[:, :-step], b[:, :-step]
        a_cur, b_cur = a[:, step:], b[:, step:]
        a = torch.cat((a[:, :step], a_cur * a_prev), dim=1)
        b = torch.cat((b[:, :step], a_cur * b_prev + b_cur), dim=1)
        step <<= 1
    return b


def conv1d_causal_4tap(x: torch.Tensor, weight: torch.Tensor, bias: torch.Tensor) -> torch.Tensor:
    B, _, C = x.shape
    z1 = torch.zeros((B, 1, C), dtype=x.dtype, device=x.device)
    z2 = torch.zeros((B, 2, C), dtype=x.dtype, device=x.device)
    z3 = torch.zeros((B, 3, C), dtype=x.dtype, device=x.device)
    x0 = torch.cat((z3, x[:, :-3, :]), dim=1)
    x1 = torch.cat((z2, x[:, :-2, :]), dim=1)
    x2 = torch.cat((z1, x[:, :-1, :]), dim=1)
    return (
        x0 * weight[:, 0].view(1, 1, C)
        + x1 * weight[:, 1].view(1, 1, C)
        + x2 * weight[:, 2].view(1, 1, C)
        + x * weight[:, 3].view(1, 1, C)
        + bias.view(1, 1, C)
    )


class StreamingInworldStep(nn.Module):
    def __init__(self, state_dict: dict, cfg: dict, attn_window: int = 256, lookahead_frames: int = 2, step_frames: int = 2):
        super().__init__()
        self.W = attn_window
        self.T = step_frames
        self.width = cfg["width"]
        self.heads = cfg["num_heads"]
        self.head_dim = self.width // self.heads
        self.inner = cfg["mamba_expand"] * self.width
        self.d_state = cfg["mamba_state"]
        self.dt_rank = math.ceil(self.width / 16)
        self.edges = band_edges(cfg)
        self.bands = len(self.edges)
        self.n_bins = cfg["n_fft"] // 2 + 1
        self.block_types = cfg["block_types"]
        self.dt = torch.float16

        cos_w, sin_w = rope_tables(self.W, self.head_dim)
        self.register_buffer("rope_cos_k", cos_w.view(1, 1, self.W, self.head_dim // 2).to(self.dt))
        self.register_buffer("rope_sin_k", sin_w.view(1, 1, self.W, self.head_dim // 2).to(self.dt))
        self.register_buffer("rope_cos_q", cos_w[-self.T:].view(1, 1, self.T, self.head_dim // 2).to(self.dt))
        self.register_buffer("rope_sin_q", sin_w[-self.T:].view(1, 1, self.T, self.head_dim // 2).to(self.dt))

        widths = [e - s for s, e in self.edges]
        max_in, max_out = 2 * max(widths), 4 * max(widths)
        gather = torch.full((self.bands, max_in), 2 * self.n_bins, dtype=torch.int64)
        valid = torch.zeros(self.bands, max_in, dtype=torch.float32)
        keep = []
        for b, (s, e) in enumerate(self.edges):
            gather[b, : 2 * (e - s)] = torch.arange(2 * s, 2 * e)
            valid[b, : 2 * (e - s)] = 1.0
            keep.append(b * max_out + torch.arange(4 * (e - s)))
        self.register_buffer("enc_gather", gather)
        self.register_buffer("enc_valid", valid)
        self.register_buffer("enc_counts", torch.tensor([2.0 * w for w in widths], dtype=torch.float32).view(self.bands, 1))
        self.register_buffer("dec_keep", torch.cat(keep))

        enc_nw = torch.zeros(self.bands, max_in, dtype=torch.float32)
        enc_nb = torch.zeros(self.bands, max_in, dtype=torch.float32)
        enc_w = torch.zeros(self.bands, max_in, self.width, dtype=torch.float32)
        enc_b = torch.zeros(self.bands, self.width, dtype=torch.float32)
        for b in range(self.bands):
            nw, nb = state_dict[f"encoders.{b}.0.weight"], state_dict[f"encoders.{b}.0.bias"]
            w, bi = state_dict[f"encoders.{b}.1.weight"].t(), state_dict[f"encoders.{b}.1.bias"]
            n = nw.shape[0]
            enc_nw[b, :n], enc_nb[b, :n], enc_w[b, :n, :], enc_b[b] = nw, nb, w, bi
        la_idx = cfg["lookahead_frames_choices"].index(lookahead_frames)
        enc_b = enc_b + state_dict["band_embedding"] + state_dict["lookahead_embedding.weight"][la_idx].unsqueeze(0)
        self.register_buffer("enc_nw", enc_nw)
        self.register_buffer("enc_nb", enc_nb)
        self.register_buffer("enc_w", enc_w.to(self.dt))
        self.register_buffer("enc_b", enc_b.unsqueeze(1).to(self.dt))

        cond = F.linear(
            torch.tensor([1.0, 0.0]),
            state_dict["denoise_decoder.condition.weight"],
            state_dict["denoise_decoder.condition.bias"],
        )
        scale, shift = cond.chunk(2, dim=-1)
        self.register_buffer("dec_scale", (1.0 + scale).view(1, 1, self.width).to(self.dt))
        self.register_buffer("dec_shift", shift.view(1, 1, self.width).to(self.dt))
        self.register_buffer("dec_nw", torch.stack([state_dict[f"denoise_decoder.heads.{b}.0.weight"] for b in range(self.bands)]).unsqueeze(1))
        self.register_buffer("dec_nb", torch.stack([state_dict[f"denoise_decoder.heads.{b}.0.bias"] for b in range(self.bands)]).unsqueeze(1))
        self.register_buffer("dec_w1", torch.stack([state_dict[f"denoise_decoder.heads.{b}.1.weight"].t() for b in range(self.bands)]).to(self.dt))
        self.register_buffer("dec_b1", torch.stack([state_dict[f"denoise_decoder.heads.{b}.1.bias"] for b in range(self.bands)]).unsqueeze(1).to(self.dt))
        self.register_buffer("dec_w2", torch.stack([
            F.pad(state_dict[f"denoise_decoder.heads.{b}.3.weight"].t(), (0, max_out - state_dict[f"denoise_decoder.heads.{b}.3.weight"].shape[0]))
            for b in range(self.bands)
        ]).to(self.dt))
        self.register_buffer("dec_b2", torch.stack([
            F.pad(state_dict[f"denoise_decoder.heads.{b}.3.bias"], (0, max_out - state_dict[f"denoise_decoder.heads.{b}.3.bias"].shape[0]))
            for b in range(self.bands)
        ]).unsqueeze(1).to(self.dt))

        for i, kind in enumerate(self.block_types):
            p = f"blocks.{i}"
            if kind == "transformer":
                self._reg_f32(f"b{i}_tn1_w", state_dict[f"{p}.time_transformer.norm1.weight"])
                self._reg_f32(f"b{i}_tn1_b", state_dict[f"{p}.time_transformer.norm1.bias"])
                self._reg_dt(f"b{i}_tqkv_w", state_dict[f"{p}.time_transformer.attn.qkv.weight"].t())
                self._reg_dt(f"b{i}_tqkv_b", state_dict[f"{p}.time_transformer.attn.qkv.bias"])
                self._reg_dt(f"b{i}_tproj_w", state_dict[f"{p}.time_transformer.attn.proj.weight"].t())
                self._reg_dt(f"b{i}_tproj_b", state_dict[f"{p}.time_transformer.attn.proj.bias"])
                self._reg_f32(f"b{i}_tn2_w", state_dict[f"{p}.time_transformer.norm2.weight"])
                self._reg_f32(f"b{i}_tn2_b", state_dict[f"{p}.time_transformer.norm2.bias"])
                self._reg_dt(f"b{i}_tff1_w", state_dict[f"{p}.time_transformer.ffn.0.weight"].t())
                self._reg_dt(f"b{i}_tff1_b", state_dict[f"{p}.time_transformer.ffn.0.bias"])
                self._reg_dt(f"b{i}_tff2_w", state_dict[f"{p}.time_transformer.ffn.3.weight"].t())
                self._reg_dt(f"b{i}_tff2_b", state_dict[f"{p}.time_transformer.ffn.3.bias"])
                self._reg_f32(f"b{i}_bn1_w", state_dict[f"{p}.band_transformer.norm1.weight"])
                self._reg_f32(f"b{i}_bn1_b", state_dict[f"{p}.band_transformer.norm1.bias"])
                self._reg_dt(f"b{i}_bqkv_w", state_dict[f"{p}.band_transformer.self_attn.in_proj_weight"].t())
                self._reg_dt(f"b{i}_bqkv_b", state_dict[f"{p}.band_transformer.self_attn.in_proj_bias"])
                self._reg_dt(f"b{i}_bproj_w", state_dict[f"{p}.band_transformer.self_attn.out_proj.weight"].t())
                self._reg_dt(f"b{i}_bproj_b", state_dict[f"{p}.band_transformer.self_attn.out_proj.bias"])
                self._reg_f32(f"b{i}_bn2_w", state_dict[f"{p}.band_transformer.norm2.weight"])
                self._reg_f32(f"b{i}_bn2_b", state_dict[f"{p}.band_transformer.norm2.bias"])
                self._reg_dt(f"b{i}_bff1_w", state_dict[f"{p}.band_transformer.linear1.weight"].t())
                self._reg_dt(f"b{i}_bff1_b", state_dict[f"{p}.band_transformer.linear1.bias"])
                self._reg_dt(f"b{i}_bff2_w", state_dict[f"{p}.band_transformer.linear2.weight"].t())
                self._reg_dt(f"b{i}_bff2_b", state_dict[f"{p}.band_transformer.linear2.bias"])
            else:
                tp, fp = f"{p}.time_mamba", f"{p}.freq_mamba"
                self._reg_mamba(f"b{i}_tm", state_dict, f"{tp}.forward_blocks")
                self._reg_dt(f"b{i}_tm_proj_w", state_dict[f"{tp}.output_proj.weight"].t())
                self._reg_dt(f"b{i}_tm_proj_b", state_dict[f"{tp}.output_proj.bias"])
                self._reg_f32(f"b{i}_tm_n_w", state_dict[f"{tp}.norm.weight"])
                self._reg_f32(f"b{i}_tm_n_b", state_dict[f"{tp}.norm.bias"])
                self._reg_mamba(f"b{i}_fmf", state_dict, f"{fp}.forward_blocks")
                self._reg_mamba(f"b{i}_fmb", state_dict, f"{fp}.backward_blocks")
                self._reg_dt(f"b{i}_fm_proj_w", state_dict[f"{fp}.output_proj.weight"].t())
                self._reg_dt(f"b{i}_fm_proj_b", state_dict[f"{fp}.output_proj.bias"])
                self._reg_f32(f"b{i}_fm_n_w", state_dict[f"{fp}.norm.weight"])
                self._reg_f32(f"b{i}_fm_n_b", state_dict[f"{fp}.norm.bias"])

    def _reg_dt(self, name, tensor):
        self.register_buffer(name, tensor.detach().contiguous().to(self.dt))

    def _reg_f32(self, name, tensor):
        self.register_buffer(name, tensor.detach().contiguous().float())

    def _reg_mamba(self, prefix, sd, sd_prefix):
        self._reg_dt(f"{prefix}_in_w", sd[f"{sd_prefix}.in_proj.weight"].t())
        self._reg_dt(f"{prefix}_conv_w", sd[f"{sd_prefix}.conv1d.weight"].squeeze(1))
        self._reg_dt(f"{prefix}_conv_b", sd[f"{sd_prefix}.conv1d.bias"])
        self._reg_dt(f"{prefix}_x_w", sd[f"{sd_prefix}.x_proj.weight"].t())
        self._reg_f32(f"{prefix}_dt_w", sd[f"{sd_prefix}.dt_proj.weight"].t())
        self._reg_f32(f"{prefix}_dt_b", sd[f"{sd_prefix}.dt_proj.bias"])
        self._reg_f32(f"{prefix}_A", -torch.exp(sd[f"{sd_prefix}.A_log"].float()))
        self._reg_f32(f"{prefix}_D", sd[f"{sd_prefix}.D"].float())
        self._reg_dt(f"{prefix}_out_w", sd[f"{sd_prefix}.out_proj.weight"].t())

    def _ln(self, x: torch.Tensor, w: torch.Tensor, b: torch.Tensor) -> torch.Tensor:
        x32 = x.float()
        mean = x32.mean(-1, keepdim=True)
        var = ((x32 - mean) * (x32 - mean)).mean(-1, keepdim=True)
        return ((x32 - mean) * torch.rsqrt(var + 1e-5) * w + b).to(self.dt)

    def _gelu(self, x: torch.Tensor) -> torch.Tensor:
        return F.gelu(x.float()).to(self.dt)

    def _freq_mamba_half(self, x: torch.Tensor, prefix: str):
        xz = torch.matmul(x, getattr(self, f"{prefix}_in_w"))
        u, z = xz.chunk(2, dim=-1)
        conv = conv1d_causal_4tap(u, getattr(self, f"{prefix}_conv_w"), getattr(self, f"{prefix}_conv_b"))
        u_act = F.silu(conv.float()).to(self.dt)
        proj = torch.matmul(u_act, getattr(self, f"{prefix}_x_w"))
        dt_r, b_s, c_s = torch.split(proj, [self.dt_rank, self.d_state, self.d_state], dim=-1)
        delta = F.softplus(torch.matmul(dt_r.float(), getattr(self, f"{prefix}_dt_w")) + getattr(self, f"{prefix}_dt_b"))
        A = getattr(self, f"{prefix}_A")
        a_fac = torch.exp(delta.unsqueeze(-1) * A.view(1, 1, self.inner, self.d_state))
        b_fac = (delta * u_act.float()).unsqueeze(-1) * b_s.float().unsqueeze(2)
        return a_fac, b_fac, u_act.float(), c_s.float(), z.float()

    def forward(self, spec, attn_mask, k0, v0, k1, v1, k2, v2, k3, v3, c0, s0, c1, s1, c2, s2):
        T = self.T
        flat = torch.cat((spec.reshape(T, 2 * self.n_bins), torch.zeros((T, 1), dtype=torch.float32, device=spec.device)), dim=1)
        x_enc = flat[:, self.enc_gather].transpose(0, 1)  # (bands, T, max_in)
        valid = self.enc_valid.unsqueeze(1)
        counts = self.enc_counts.unsqueeze(1)
        mean = x_enc.sum(-1, keepdim=True) / counts
        centered = (x_enc - mean) * valid
        var = (centered * centered).sum(-1, keepdim=True) / counts
        x_enc = centered * torch.rsqrt(var + 1e-5) * self.enc_nw.unsqueeze(1) + self.enc_nb.unsqueeze(1)
        feat = torch.bmm(x_enc.to(self.dt), self.enc_w) + self.enc_b  # (bands, T, width)

        ks_in, vs_in = [k0, k1, k2, k3], [v0, v1, v2, v3]
        cs_in, ss_in = [c0, c1, c2], [s0, s1, s2]
        ks_out, vs_out, cs_out, ss_out = [], [], [], []
        attn_idx = mamba_idx = 0
        inv_sqrt_d = 1.0 / math.sqrt(self.head_dim)

        for i, kind in enumerate(self.block_types):
            if kind == "transformer":
                h = self._ln(feat, getattr(self, f"b{i}_tn1_w"), getattr(self, f"b{i}_tn1_b"))
                qkv = (torch.matmul(h, getattr(self, f"b{i}_tqkv_w")) + getattr(self, f"b{i}_tqkv_b")).view(self.bands, T, 3, self.heads, self.head_dim)
                q, k_new, v_new = qkv[:, :, 0].permute(0, 2, 1, 3), qkv[:, :, 1].permute(0, 2, 1, 3), qkv[:, :, 2].permute(0, 2, 1, 3)
                k_cache = torch.cat((ks_in[attn_idx][:, :, T:, :], k_new), dim=2)
                v_cache = torch.cat((vs_in[attn_idx][:, :, T:, :], v_new), dim=2)
                ks_out.append(k_cache); vs_out.append(v_cache); attn_idx += 1

                q_rot = apply_rope(q, self.rope_cos_q, self.rope_sin_q)
                k_rot = apply_rope(k_cache, self.rope_cos_k, self.rope_sin_k)
                scores = torch.matmul(q_rot, k_rot.transpose(-2, -1)) * inv_sqrt_d
                probs = F.softmax(scores.float() + attn_mask.float(), dim=-1).to(self.dt)
                out = torch.matmul(probs, v_cache)
                o32, v32 = out.float(), v_new.float()
                coeff = (o32 * v32).sum(-1, keepdim=True) / (v32 * v32).sum(-1, keepdim=True).clamp(min=1e-6)
                out = (o32 - coeff * v32).to(self.dt).permute(0, 2, 1, 3).reshape(self.bands, T, self.width)
                feat = feat + (torch.matmul(out, getattr(self, f"b{i}_tproj_w")) + getattr(self, f"b{i}_tproj_b"))
                h2 = self._ln(feat, getattr(self, f"b{i}_tn2_w"), getattr(self, f"b{i}_tn2_b"))
                ff = self._gelu(torch.matmul(h2, getattr(self, f"b{i}_tff1_w")) + getattr(self, f"b{i}_tff1_b"))
                feat = feat + (torch.matmul(ff, getattr(self, f"b{i}_tff2_w")) + getattr(self, f"b{i}_tff2_b"))

                bf = feat.transpose(0, 1)
                bh = self._ln(bf, getattr(self, f"b{i}_bn1_w"), getattr(self, f"b{i}_bn1_b"))
                bqkv = (torch.matmul(bh, getattr(self, f"b{i}_bqkv_w")) + getattr(self, f"b{i}_bqkv_b")).view(T, self.bands, 3, self.heads, self.head_dim)
                bq, bk, bv = bqkv[:, :, 0].permute(0, 2, 1, 3), bqkv[:, :, 1].permute(0, 2, 1, 3), bqkv[:, :, 2].permute(0, 2, 1, 3)
                bprobs = F.softmax(torch.matmul(bq, bk.transpose(-2, -1)).float() * inv_sqrt_d, dim=-1).to(self.dt)
                bout = torch.matmul(bprobs, bv).permute(0, 2, 1, 3).reshape(T, self.bands, self.width)
                bf = bf + (torch.matmul(bout, getattr(self, f"b{i}_bproj_w")) + getattr(self, f"b{i}_bproj_b"))
                bh2 = self._ln(bf, getattr(self, f"b{i}_bn2_w"), getattr(self, f"b{i}_bn2_b"))
                bff = self._gelu(torch.matmul(bh2, getattr(self, f"b{i}_bff1_w")) + getattr(self, f"b{i}_bff1_b"))
                feat = (bf + (torch.matmul(bff, getattr(self, f"b{i}_bff2_w")) + getattr(self, f"b{i}_bff2_b"))).transpose(0, 1)
            else:
                pfx = f"b{i}_tm"
                u, z = torch.matmul(feat, getattr(self, f"{pfx}_in_w")).chunk(2, dim=-1)  # (bands, T, inner)
                hist = torch.cat((cs_in[mamba_idx], u.transpose(1, 2)), dim=-1)  # (bands, inner, 3 + T)
                cs_out.append(hist[:, :, T:])
                conv_w = getattr(self, f"{pfx}_conv_w").unsqueeze(0)
                conv_b = getattr(self, f"{pfx}_conv_b").view(1, 1, self.inner)
                conv_steps = [(hist[:, :, t : t + 4] * conv_w).sum(-1) for t in range(T)]
                conv_out = torch.stack(conv_steps, dim=1) + conv_b
                u_act = F.silu(conv_out.float()).to(self.dt)
                dt_r, b_s, c_s = torch.split(torch.matmul(u_act, getattr(self, f"{pfx}_x_w")), [self.dt_rank, self.d_state, self.d_state], dim=-1)
                delta = F.softplus(torch.matmul(dt_r.float(), getattr(self, f"{pfx}_dt_w")) + getattr(self, f"{pfx}_dt_b"))
                u32 = u_act.float()
                A = getattr(self, f"{pfx}_A").unsqueeze(0)
                D = getattr(self, f"{pfx}_D").unsqueeze(0)
                ssm = ss_in[mamba_idx]
                ys = []
                for t in range(T):
                    dt_t = delta[:, t, :]
                    u_t = u32[:, t, :]
                    b_t = b_s[:, t, :].float()
                    c_t = c_s[:, t, :].float()
                    ssm = torch.exp(dt_t.unsqueeze(-1) * A) * ssm + (dt_t * u_t).unsqueeze(-1) * b_t.unsqueeze(1)
                    ys.append((ssm * c_t.unsqueeze(1)).sum(-1) + D * u_t)
                ss_out.append(ssm); mamba_idx += 1
                y = torch.stack(ys, dim=1) * F.silu(z.float())
                fwd = torch.matmul(y.to(self.dt), getattr(self, f"{pfx}_out_w"))
                tm_out = self._ln(torch.matmul(fwd + feat, getattr(self, f"b{i}_tm_proj_w")) + getattr(self, f"b{i}_tm_proj_b"), getattr(self, f"b{i}_tm_n_w"), getattr(self, f"b{i}_tm_n_b"))
                feat = feat + tm_out

                bf = feat.transpose(0, 1)
                af, b_f, uf, cf, zf = self._freq_mamba_half(bf, f"b{i}_fmf")
                ab, b_b, ub, cb, zb = self._freq_mamba_half(torch.flip(bf, dims=[1]), f"b{i}_fmb")
                h_both = parallel_prefix_scan(torch.cat((af, ab), dim=0), torch.cat((b_f, b_b), dim=0))
                yf = ((h_both[0:T] * cf.unsqueeze(2)).sum(-1) + getattr(self, f"b{i}_fmf_D").view(1, 1, self.inner) * uf) * F.silu(zf)
                yb = ((h_both[T : 2 * T] * cb.unsqueeze(2)).sum(-1) + getattr(self, f"b{i}_fmb_D").view(1, 1, self.inner) * ub) * F.silu(zb)
                out_f = torch.matmul(yf.to(self.dt), getattr(self, f"b{i}_fmf_out_w"))
                out_b = torch.flip(torch.matmul(yb.to(self.dt), getattr(self, f"b{i}_fmb_out_w")), dims=[1])
                cat_fb = torch.cat((out_f + bf, out_b + bf), dim=-1)
                fm_out = self._ln(torch.matmul(cat_fb, getattr(self, f"b{i}_fm_proj_w")) + getattr(self, f"b{i}_fm_proj_b"), getattr(self, f"b{i}_fm_n_w"), getattr(self, f"b{i}_fm_n_b"))
                feat = (bf + fm_out).transpose(0, 1)

        x32 = (feat * self.dec_scale + self.dec_shift).float()
        mean = x32.mean(-1, keepdim=True)
        var = ((x32 - mean) * (x32 - mean)).mean(-1, keepdim=True)
        x = ((x32 - mean) * torch.rsqrt(var + 1e-5) * self.dec_nw + self.dec_nb).to(self.dt)
        x = self._gelu(torch.bmm(x, self.dec_w1) + self.dec_b1)
        x = torch.bmm(x, self.dec_w2) + self.dec_b2
        raw = x.transpose(0, 1).reshape(T, -1)[:, self.dec_keep].float().view(T, self.n_bins, 4)
        return (raw, ks_out[0], vs_out[0], ks_out[1], vs_out[1], ks_out[2], vs_out[2], ks_out[3], vs_out[3],
                cs_out[0], ss_out[0], cs_out[1], ss_out[1], cs_out[2], ss_out[2])


def download_weights(cache_path: pathlib.Path) -> pathlib.Path:
    if cache_path.exists() and hashlib.sha256(cache_path.read_bytes()).hexdigest() == WEIGHTS_SHA256:
        return cache_path
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    print(f"Downloading Inworld realtime weights -> {cache_path}", flush=True)
    req = urllib.request.Request(WEIGHTS_URL, headers={"User-Agent": "AudioBridge"})
    h = hashlib.sha256()
    with urllib.request.urlopen(req, timeout=300) as r, open(cache_path, "wb") as f:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
            h.update(chunk)
    if h.hexdigest() != WEIGHTS_SHA256:
        cache_path.unlink(missing_ok=True)
        raise ValueError("Checksum mismatch for denoiser_realtime.safetensors")
    return cache_path


def main():
    output = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path("windows/build/voice-models/inworld_realtime_fp16.onnx")
    weights = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else output.parent / ".cache" / "denoiser_realtime.safetensors"
    download_weights(weights)
    cfg = read_config(weights)
    sd = load_file(str(weights))
    W = 256
    T = 2
    model = StreamingInworldStep(sd, cfg, attn_window=W, lookahead_frames=2, step_frames=T).eval()
    in_names = ["spec", "attn_mask", "k_in_0", "v_in_0", "k_in_1", "v_in_1", "k_in_2", "v_in_2", "k_in_3", "v_in_3",
                "c_in_0", "s_in_0", "c_in_1", "s_in_1", "c_in_2", "s_in_2"]
    out_names = ["raw", "k_out_0", "v_out_0", "k_out_1", "v_out_1", "k_out_2", "v_out_2", "k_out_3", "v_out_3",
                 "c_out_0", "s_out_0", "c_out_1", "s_out_1", "c_out_2", "s_out_2"]
    dummy = (
        torch.zeros(T, 1025, 2, dtype=torch.float32),
        torch.zeros(1, 1, T, W, dtype=torch.float16),
        *([torch.zeros(46, 8, W, 64, dtype=torch.float16)] * 8),
        *([torch.zeros(46, 1024, 3, dtype=torch.float16), torch.zeros(46, 1024, 16, dtype=torch.float32)] * 3),
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    torch.onnx.export(model, dummy, str(output), input_names=in_names, output_names=out_names,
                      opset_version=17, do_constant_folding=True, dynamo=False)
    lic = output.parent / "licenses" / "inworld-voice-isolator.txt"
    lic.parent.mkdir(parents=True, exist_ok=True)
    lic.write_bytes(urllib.request.urlopen(LICENSE_URL, timeout=30).read())
    print(f"Exported {output} ({output.stat().st_size / 1e6:.2f} MB)")


if __name__ == "__main__":
    main()
