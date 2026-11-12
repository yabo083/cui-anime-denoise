"""Export the sidecar replacement's super-resolution models to ONNX.

Exports each .pth checkpoint (loaded via spandrel, same as the reference
gpt-anime-denoise denoise.py) to a plain opset-17 ONNX graph in models/:

  input  "input"  NCHW float32 RGB in [0, 1], dynamic N/H/W
  output "output" NCHW float32, no clamp inside the graph

Weights stay fp32 and no external optimizer/simplifier is baked in, so the C
engine's ONNX Runtime sees the same maths as the torch reference.

Usage (run with a python that has torch+spandrel+onnx, e.g. the tmp export env):

  python tools/export_onnx.py                # export all three models
  python tools/export_onnx.py esrgan cugan   # only some

Downloads are cached in _dl/models-src/ and SHA-256 verified against the same
table as denoise.py; the CUGAN weight is extracted from its zip release.
Outputs land in models/ next to this repo's assets (gitignored, *.onnx).
"""
import hashlib
import io
import sys
import urllib.request
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CACHE = REPO / "_dl" / "models-src"
OUT = REPO / "models"

MODELS = {
    "esrgan": ("RealESRGAN_x4plus_anime_6B.pth",
               "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.2.4/RealESRGAN_x4plus_anime_6B.pth",
               "esrgan_anime6b_x4.onnx"),
    "apisr": ("2x_APISR_RRDB_GAN_generator.pth",
              "https://huggingface.co/camenduru/APISR/resolve/main/2x_APISR_RRDB_GAN_generator.pth",
              "apisr_rrdb_x2.onnx"),
    "cugan": ("up2x-latest-denoise3x.pth",
              "https://github.com/bilibili/ailab/releases/download/Real-CUGAN/updated_weights.zip",
              "cugan_up2x_denoise3x.onnx"),
}
SHA256 = {
    "RealESRGAN_x4plus_anime_6B.pth": "f872d837d3c90ed2e05227bed711af5671a6fd1c9f7d7e91c911a61f155e99da",
    "2x_APISR_RRDB_GAN_generator.pth": "3b0d2b3a3c0461ac17d00f4f32240666fb832b738ea5a48449b1acf07fbb07e5",
    "up2x-latest-denoise3x.pth": "0a14739f3f5fcbd74ec3ce2806d13a47916c916b20afe4a39d95f6df4ca6abd8",
}


def fetch(key):
    """Return the cached checkpoint path for key, downloading + hash-checking if missing."""
    name, url, _ = MODELS[key]
    path = CACHE / name
    if path.exists():
        return path
    CACHE.mkdir(parents=True, exist_ok=True)
    print(f"downloading {name} ...", flush=True)
    data = urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "cui-anime-denoise"})).read()
    if url.endswith(".zip"):
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            data = z.read(next(n for n in z.namelist() if n.endswith(name)))
    if hashlib.sha256(data).hexdigest() != SHA256[name]:
        raise SystemExit(f"checksum mismatch for {name} from {url}; not saved")
    path.write_bytes(data)
    return path


def export_one(key):
    import onnx
    import spandrel
    import torch

    name, _, onnx_name = MODELS[key]
    model = spandrel.ModelLoader().load_from_file(str(fetch(key))).model.eval()
    if key == "cugan":
        # UpCunet2x.forward(x, alpha=1) would export a second scalar "alpha"
        # input; bake the default the reference uses instead.
        inner = model

        class _AlphaFixed(torch.nn.Module):
            def __init__(self, m):
                super().__init__()
                self.m = m

            def forward(self, x):
                return self.m(x, 1.0)

        model = _AlphaFixed(inner)
    OUT.mkdir(exist_ok=True)
    dest = OUT / onnx_name

    # dynamo=False: the legacy exporter embeds fp32 weights in a single file and
    # honours opset 17 exactly (the dynamo path emits external .onnx.data and can
    # only downconvert to 17 when a version adapter exists — CUGAN's Pad has none).
    x = torch.zeros(1, 3, 64, 64)
    with torch.no_grad():
        torch.onnx.export(
            model, x, str(dest), dynamo=False,
            opset_version=17,
            input_names=["input"], output_names=["output"],
            dynamic_axes={"input": {0: "N", 2: "H", 3: "W"},
                          "output": {0: "N", 2: "H", 3: "W"}},
        )
    m = onnx.load(str(dest))
    onnx.checker.check_model(m)
    mb = dest.stat().st_size / (1 << 20)
    print(f"{name} -> {dest} ({mb:.1f} MiB, opset {m.opset_import[0].version})", flush=True)


def main():
    keys = sys.argv[1:] or list(MODELS)
    for k in keys:
        if k not in MODELS:
            raise SystemExit(f"unknown model {k!r}; choose from {', '.join(MODELS)}")
        export_one(k)


if __name__ == "__main__":
    main()
