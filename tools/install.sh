#!/bin/sh
# install.sh -- install the portable anime-denoise tree (non-dpkg path).
#
#   ./install.sh                # -> /usr/local (needs sudo)
#   ./install.sh --prefix DIR   # -> DIR (e.g. $HOME/.local, no sudo)
#   PREFIX=$HOME/.local ./install.sh
#
# Layout: $PREFIX/lib/anime-denoise/{anime-denoise, runtime/, models/,
# assets/, cuda-setup.sh} and $PREFIX/bin/{anime-denoise,
# anime-denoise-cuda-setup} symlinks. The engine resolves resources from
# /proc/self/exe so any prefix works.
set -eu
PREFIX="${PREFIX:-/usr/local}"
case "${1:-}" in
    --prefix) PREFIX="${2:?--prefix needs a directory}" ;;
    --prefix=*) PREFIX="${1#--prefix=}" ;;
    "") ;;
    *) echo "usage: install.sh [--prefix DIR]" >&2; exit 2 ;;
esac

SRC="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
DEST="$PREFIX/lib/anime-denoise"

if ! mkdir -p "$DEST" "$PREFIX/bin" 2>/dev/null; then
    echo "install: cannot create $DEST -- rerun with sudo or --prefix" >&2
    exit 1
fi
if [ ! -w "$DEST" ] || [ ! -w "$PREFIX/bin" ]; then
    echo "install: $DEST or $PREFIX/bin not writable -- sudo or --prefix" >&2
    exit 1
fi

cp "$SRC/anime-denoise" "$SRC/cuda-setup.sh" "$DEST/"
cp -R "$SRC/runtime" "$SRC/models" "$SRC/assets" "$DEST/"
ln -sfn "$DEST/anime-denoise" "$PREFIX/bin/anime-denoise"
ln -sfn "$DEST/cuda-setup.sh" "$PREFIX/bin/anime-denoise-cuda-setup"

echo "installed to $DEST"
echo "  $PREFIX/bin/anime-denoise --help"
echo "  NVIDIA GPU? sudo anime-denoise-cuda-setup"
