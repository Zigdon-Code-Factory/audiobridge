"""Fetch pinned voice models (standard-library only; no Python runtime in the app)."""
import hashlib
import io
from pathlib import Path
import sys
import tarfile
import urllib.request

MODELS = [
    ("deepfilternet3_streaming.onnx",
     "https://github.com/grazder/DeepFilterNet/raw/408414f2bdb9ed772580558d1c3ec26c5dc002d7/models/DeepFilterNet3_torchDF_onnx.tar",
     "b758c49d6708a5b7979e3de185705a8a4915076c862fb17b1b304d9a72b75cdc",
     "DeepFilterNet3_torchDF_onnx/denoiser_model.onnx"),
    ("fastenhancer_l.onnx",
     "https://github.com/aask1357/fastenhancer/releases/download/onnx-48khz-v1/fastenhancer_l.onnx",
     "d7138309ec98266c668b2f86658fcc8f4e82ded67cd7fc8d6598534104b4bf89", None),
    ("dpdfnet8_48khz_hr.onnx",
     "https://huggingface.co/Ceva-IP/DPDFNet/resolve/dd6818d00f50c836fed43a6243ebe49116de5964/onnx/dpdfnet8_48khz_hr.onnx",
     "7b3afbb260a08fe9af3d16e3bda992971be1e7e951d1dee7c2d235f5c43f5631", None),
    ("silero_vad.onnx",
     "https://raw.githubusercontent.com/snakers4/silero-vad/v6.2.3/src/silero_vad/data/silero_vad.onnx",
     "1a153a22f4509e292a94e67d6f9b85e8deb25b4988682b7e174c65279d8788e3", None),
]

LICENSES = [
    ("licenses/open-broadcast.txt",
     "https://raw.githubusercontent.com/Zigdon-Code-Factory/open-broadcast/72af5bf60c8c66bbe0b6bc68b27660e01625a37c/LICENSE",
     "933125553551b7448028c9d46cf9c8ca4d0fd87c4b0d7d5e4b88b92ae96c1c37", None),
    ("licenses/deepfilternet-MIT.txt",
     "https://raw.githubusercontent.com/grazder/DeepFilterNet/408414f2bdb9ed772580558d1c3ec26c5dc002d7/LICENSE-MIT",
     "24e6bb09c928af8d8e56268082f87413247ce36b39dd5d33add2f9893968065e", None),
    ("licenses/fastenhancer.txt",
     "https://raw.githubusercontent.com/aask1357/fastenhancer/85d98a689a85b956dec384f258b075859ca50263/LICENSE",
     "38164c22720cdccceb5f0dbb8f7218dcf17bac97ac536cad2e77c33dfbc45a18", None),
    ("licenses/dpdfnet.txt",
     "https://raw.githubusercontent.com/ceva-ip/DPDFNet/9bd9844a227bb6aa57e55588d8d0e961fcff1c46/LICENSE",
     "c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4", None),
    ("licenses/silero-vad.txt",
     "https://raw.githubusercontent.com/snakers4/silero-vad/v6.2.3/LICENSE",
     "2e63e9a38b6e8fc0c7bc37ce174caca1862870856c6daf5697cfb785e925520b", None),
]


def fetch(destination):
    destination.mkdir(parents=True, exist_ok=True)
    for name, url, digest, member in MODELS + LICENSES:
        target = destination / name
        if target.exists():
            if hashlib.sha256(target.read_bytes()).hexdigest() == digest:
                continue
        print(f"Fetching {name}", flush=True)
        with urllib.request.urlopen(url, timeout=120) as response:
            data = response.read()
        if member:
            with tarfile.open(fileobj=io.BytesIO(data)) as archive:
                data = archive.extractfile(member).read()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f"Checksum mismatch for {name}")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)


if __name__ == "__main__":
    fetch(Path(sys.argv[1]))
