#!/bin/sh
# anime-denoise-cuda-setup -- upgrade a CPU-only install to use NVIDIA CUDA.
#
# Fetches the ONNX Runtime linux-x64-gpu build (the same 1.24.4 the base
# package ships), verifies its pinned sha256, and stages libonnxruntime.so
# plus libonnxruntime_providers_{shared,cuda}.so into <exe>/runtime/.
# Afterwards `anime-denoise --device gpu|auto` picks the CUDA EP.
#
# Needs write access to the runtime dir: with the .deb / default install.sh
# layout that means `sudo anime-denoise-cuda-setup`.
set -eu
VER=1.24.4
SHA256="c5f804ff5d239b436fa59e9f2fb288a39f7eb9552f6a636c8b71e792e91a8808"
TGZ="onnxruntime-linux-x64-gpu-$VER.tgz"
URL="https://github.com/microsoft/onnxruntime/releases/download/v$VER/$TGZ"
DIR="onnxruntime-linux-x64-gpu-$VER"

# Resolve through the symlink so /usr/bin/anime-denoise-cuda-setup still
# finds /usr/lib/anime-denoise/runtime.
SELF="$(readlink -f "$0" 2>/dev/null || echo "$0")"
RT_DIR="$(CDPATH= cd -- "$(dirname -- "$SELF")" && pwd)/runtime"

if [ ! -d "$RT_DIR" ]; then
    echo "cuda-setup: runtime dir not found at $RT_DIR" >&2
    exit 1
fi
if [ ! -w "$RT_DIR" ]; then
    echo "cuda-setup: $RT_DIR is not writable -- run with sudo" >&2
    exit 1
fi
for tool in curl tar sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "cuda-setup: missing required tool: $tool" >&2; exit 1; }
done

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "cuda-setup: downloading ONNX Runtime $VER (linux-x64-gpu) ..."
curl -fL --retry 3 -o "$TMP/$TGZ" "$URL"
echo "$SHA256  $TMP/$TGZ" | sha256sum -c - || {
    echo "cuda-setup: tarball failed sha256 verification" >&2; exit 1; }
tar -xzf "$TMP/$TGZ" -C "$TMP" --wildcards "$DIR/lib/libonnxruntime*.so*"

# Atomic staging: a truncated .so must never satisfy a later -f check.
for so in "$TMP/$DIR/lib/libonnxruntime.so.$VER" \
          "$TMP/$DIR/lib/"libonnxruntime_providers_*.so; do
    [ -e "$so" ] || continue
    cp "$so" "$RT_DIR/$(basename "$so").tmp"
    mv "$RT_DIR/$(basename "$so").tmp" "$RT_DIR/$(basename "$so")"
done
# The engine dlopens "libonnxruntime.so" -- keep the unversioned name too.
cp "$RT_DIR/libonnxruntime.so.$VER" "$RT_DIR/libonnxruntime.so.tmp"
mv "$RT_DIR/libonnxruntime.so.tmp" "$RT_DIR/libonnxruntime.so"

echo "cuda-setup: done -- 'anime-denoise --device gpu' now uses the CUDA EP"
