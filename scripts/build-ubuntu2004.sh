#!/usr/bin/env bash
# Native build of libiox2_cif.so on Ubuntu 20.04 (glibc 2.31).
# Intended to run inside the scripts/Dockerfile.ubuntu2004 image.
set -euo pipefail

export CARGO_HOME="${CARGO_HOME:-$HOME/.cargo}"
export RUSTUP_HOME="${RUSTUP_HOME:-$HOME/.rustup}"
export PATH="/usr/local/bin:$CARGO_HOME/bin:/usr/bin:/bin:$PATH"
export CFLAGS="${CFLAGS:--O2 -fPIC -march=x86-64}"
export CXXFLAGS="${CXXFLAGS:--O2 -fPIC -march=x86-64}"
export RUSTFLAGS="${RUSTFLAGS:--C relocation-model=pic -C target-cpu=x86-64}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(tr -d ' \t\r\n' < "$ROOT/VERSION")"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-ubuntu2004}"
DIST_DIR="${DIST_DIR:-$ROOT/dist/ubuntu}"
SO_NAME="libiox2_cif-ubuntu.so.${VERSION}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

echo "Root: $ROOT"
echo "cmake: $(command -v cmake) ($(cmake --version | head -1))"
echo "rustc: $(command -v rustc) ($(rustc --version))"
echo "cargo: $(command -v cargo) ($(cargo --version))"
echo "glibc: $(ldd --version | head -1)"
echo "CFLAGS=$CFLAGS"
echo "RUSTFLAGS=$RUSTFLAGS"
echo "VERSION=$VERSION"
echo "SO_NAME=$SO_NAME"

bash "$(dirname "$0")/fetch-iceoryx2.sh"

mkdir -p "$BUILD_DIR" "$DIST_DIR"

cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$CFLAGS" \
    -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
    -DBUILD_CXX=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_TESTING=OFF \
    -DIOX2_FEATURE_DEV_PERMISSIONS=ON

cmake --build "$BUILD_DIR" --target iox2_cif --target iox2_cif_smoke -j "$JOBS"

SO="$(find "$BUILD_DIR" -name 'libiox2_cif.so' | head -1)"
SMOKE="$(find "$BUILD_DIR" -name 'iox2_cif_smoke' -type f | head -1)"
if [ -z "$SO" ]; then
    echo "libiox2_cif.so not found under $BUILD_DIR" >&2
    exit 1
fi

cp -f "$SO" "$DIST_DIR/$SO_NAME"
cp -f "$ROOT/native/include/iox2_cif.h" "$DIST_DIR/iox2_cif.h"
ln -sfn "$SO_NAME" "$DIST_DIR/libiox2_cif.so"
echo "Copied $SO -> $DIST_DIR/$SO_NAME"

if [ -n "$SMOKE" ]; then
    echo "Running smoke test..."
    (cd "$(dirname "$SMOKE")" && ./"$(basename "$SMOKE")")
    echo "Smoke test passed."
fi

echo "Done: $DIST_DIR/$SO_NAME"
file "$DIST_DIR/$SO_NAME" || true
ldd "$DIST_DIR/$SO_NAME" || true
echo "Highest GLIBC symbol:"
objdump -T "$DIST_DIR/$SO_NAME" | sed -n 's/.*GLIBC_\([0-9.]*\).*/\1/p' | sort -V | tail -5
