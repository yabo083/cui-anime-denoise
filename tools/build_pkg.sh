#!/bin/sh
# build_pkg.sh -- package the POSIX anime-denoise CLI for end users.
#
# Outputs (under dist/):
#   anime-denoise_<VER>_amd64.deb          dpkg/apt-managed install
#   anime-denoise_<VER>_linux-x64.tar.gz   portable tree + install.sh
#
# The .deb is the Linux analogue of installer/*.iss: install -> binary on
# PATH, runtime+models+assets under /usr/lib/anime-denoise (resolved from
# /proc/self/exe), docs under /usr/share/doc. Run on a Debian-family host
# (dpkg-deb >= 1.19 for --root-owner-group; no fakeroot needed).
#
# GPU note: whichever providers the staged build-posix/runtime/ holds get
# shipped -- a CPU-flavour runtime yields a CPU-only package; users with
# NVIDIA hardware then upgrade in place via `anime-denoise-cuda-setup`.
#
# Usage:  tools/build_pkg.sh            # build + package
#         VERSION=1.1.7 tools/build_pkg.sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT="$ROOT/build-posix"
DIST="$ROOT/dist"
VER="${VERSION:-$(sed -n 's/^VERSION  ?= //p' "$ROOT/tools/Makefile.posix" | tr -d '[:space:]')}"

for tool in tar gzip sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "build_pkg: missing required tool: $tool" >&2; exit 1; }
done

# -- build ---------------------------------------------------------------
make -C "$ROOT" -f tools/Makefile.posix -j"$(nproc 2>/dev/null || echo 4)"

[ -f "$OUT/runtime/libonnxruntime.so" ] || {
    echo "build_pkg: $OUT/runtime/libonnxruntime.so missing --" >&2
    echo "  run setup.sh first (it fetches the ORT runtime)" >&2
    exit 1
}

# -- shared payload staging ----------------------------------------------
# One payload tree feeds both packages: the exe-relative layout the engine
# resolves is preserved 1:1 (runtime/, models/, assets/ beside the binary).
PAY="$OUT/payload"
rm -rf "$PAY"
mkdir -p "$PAY/anime-denoise/runtime" "$PAY/anime-denoise/models" \
         "$PAY/anime-denoise/assets/fonts"
cp "$OUT/anime-denoise" "$PAY/anime-denoise/anime-denoise"
cp "$OUT/runtime/"*.so "$PAY/anime-denoise/runtime/"
cp "$ROOT/models/esrgan_anime6b_x4.onnx" \
   "$ROOT/models/apisr_rrdb_x2.skel.onnx" \
   "$ROOT/models/cugan_up2x_denoise3x.skel.onnx" \
   "$PAY/anime-denoise/models/"
cp "$ROOT/assets/fonts/NotoSansSC-Medium.ttf" \
   "$PAY/anime-denoise/assets/fonts/"
cp "$ROOT/tools/cuda-setup.sh" "$PAY/anime-denoise/cuda-setup.sh"
chmod +x "$PAY/anime-denoise/anime-denoise" "$PAY/anime-denoise/cuda-setup.sh"

mkdir -p "$DIST"

# -- .deb ------------------------------------------------------------------
if command -v dpkg-deb >/dev/null 2>&1; then
    PKG="$OUT/deb/anime-denoise_${VER}_amd64"
    rm -rf "$PKG"
    L="$PKG/usr/lib/anime-denoise"
    mkdir -p "$L" "$PKG/usr/bin" "$PKG/DEBIAN" \
             "$PKG/usr/share/doc/anime-denoise" "$PKG/usr/share/man/man1"
    cp -R "$PAY/anime-denoise/"* "$L/"
    ln -sfn ../lib/anime-denoise/anime-denoise "$PKG/usr/bin/anime-denoise"
    ln -sfn ../lib/anime-denoise/cuda-setup.sh \
        "$PKG/usr/bin/anime-denoise-cuda-setup"
    cat "$ROOT/LICENSE" "$ROOT/THIRD-PARTY-NOTICES.txt" \
        "$ROOT/assets/LICENSE-gpt-anime-denoise.txt" \
        > "$PKG/usr/share/doc/anime-denoise/copyright"
    gzip -9 -c "$ROOT/tools/anime-denoise.1" \
        > "$PKG/usr/share/man/man1/anime-denoise.1.gz"
    cat > "$PKG/DEBIAN/control" <<EOF
Package: anime-denoise
Version: $VER
Section: graphics
Priority: optional
Architecture: amd64
Maintainer: yabo083 <https://github.com/yabo083>
Depends: libc6, libstdc++6, curl
Homepage: https://github.com/yabo083/cui-anime-denoise
Description: Native anime-image denoise and upscale CLI
 Fast ONNX Runtime CLI port of gpt-anime-denoise: ESRGAN/CUGAN/APISR
 super-resolution plus L0 smoothing, tiled inference, ICC/EXIF
 preservation, PNG/JPEG in and out.  NVIDIA GPUs are used through the
 CUDA execution provider when present (run anime-denoise-cuda-setup once
 to install the provider libraries); CPU otherwise.
EOF
    dpkg-deb --root-owner-group -b "$PKG" \
        "$DIST/anime-denoise_${VER}_amd64.deb"
else
    echo "build_pkg: dpkg-deb not found, skipping .deb" >&2
fi

# -- portable tarball ------------------------------------------------------
cp "$ROOT/tools/install.sh" "$PAY/anime-denoise/install.sh"
chmod +x "$PAY/anime-denoise/install.sh"
cp "$ROOT/LICENSE" "$ROOT/THIRD-PARTY-NOTICES.txt" "$PAY/anime-denoise/"
tar -C "$PAY" -czf "$DIST/anime-denoise_${VER}_linux-x64.tar.gz" anime-denoise

rm -rf "$PAY" "$OUT/deb"

echo
echo "packages:"
ls -l "$DIST/" | tail -n +2
