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
