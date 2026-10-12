#!/usr/bin/env bash
# Native build of libiox2_cif.so on NI Linux Real-Time (x86_64).
set -euo pipefail

export HOME="${HOME:-/home/admin}"
export USER="${USER:-admin}"
export CARGO_HOME="${CARGO_HOME:-$HOME/.cargo}"
export RUSTUP_HOME="${RUSTUP_HOME:-$HOME/.rustup}"
export PATH="$HOME/opt/cmake/bin:$CARGO_HOME/bin:/usr/bin:/bin:$PATH"
# NI Linux RT "core2-64" includes Atom (e.g. cRIO-9057 E3805) with no AVX.
# Building on a NUC without this produces SIGILL in LabVIEW (vmovdqu).
export CFLAGS="${CFLAGS:--O2 -fPIC -march=core2}"
export CXXFLAGS="${CXXFLAGS:--O2 -fPIC -march=core2}"
export RUSTFLAGS="${RUSTFLAGS:--C relocation-model=pic -C target-cpu=core2}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(tr -d ' \t\r\n' < "$ROOT/VERSION")"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-linuxrt}"
DIST_DIR="${DIST_DIR:-$ROOT/dist/linuxrt}"
SO_NAME="libiox2_cif-linuxrt.so.${VERSION}"
IOX2_DIR="$ROOT/third_party/iceoryx2"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

echo "Root: $ROOT"
echo "cmake: $(command -v cmake) ($(cmake --version | head -1))"
echo "rustc: $(command -v rustc) ($(rustc --version))"
echo "cargo: $(command -v cargo) ($(cargo --version))"
echo "CFLAGS=$CFLAGS"
echo "RUSTFLAGS=$RUSTFLAGS"
echo "VERSION=$VERSION"
echo "SO_NAME=$SO_NAME"

if [ ! -f "$IOX2_DIR/CMakeLists.txt" ]; then
    mkdir -p "$(dirname "$IOX2_DIR")"
    git clone --depth 1 https://github.com/eclipse-iceoryx/iceoryx2.git "$IOX2_DIR"
fi

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

if [ "$(id -u)" = 0 ]; then
    install -m 0755 "$DIST_DIR/$SO_NAME" "/usr/local/lib/$SO_NAME"
    ln -sfn "/usr/local/lib/$SO_NAME" /usr/local/lib/libiox2_cif.so
    ldconfig >/dev/null 2>&1 || true
    echo "Installed /usr/local/lib/$SO_NAME -> libiox2_cif.so"
    if [ -d /home/lvuser ]; then
        install -o lvuser -g ni -m 0755 "$DIST_DIR/$SO_NAME" "/home/lvuser/$SO_NAME" || \
            install -m 0755 "$DIST_DIR/$SO_NAME" "/home/lvuser/$SO_NAME"
        ln -sfn "/home/lvuser/$SO_NAME" /home/lvuser/libiox2_cif.so
        echo "Installed /home/lvuser/$SO_NAME -> libiox2_cif.so"
    fi
fi

if [ -n "$SMOKE" ]; then
    echo "Running smoke test..."
    (cd "$(dirname "$SMOKE")" && ./"$(basename "$SMOKE")")
    echo "Smoke test passed."
fi

echo "Done: $DIST_DIR/$SO_NAME"
file "$DIST_DIR/$SO_NAME" || true
ldd "$DIST_DIR/$SO_NAME" || true
