#!/usr/bin/env bash
# Native build of libiox2_cif.so on NI Linux Real-Time (x86_64).
set -euo pipefail

export HOME="${HOME:-/home/admin}"
export USER="${USER:-admin}"
export CARGO_HOME="${CARGO_HOME:-$HOME/.cargo}"
export RUSTUP_HOME="${RUSTUP_HOME:-$HOME/.rustup}"
export PATH="$HOME/opt/cmake/bin:$CARGO_HOME/bin:/usr/bin:/bin:$PATH"
export RUSTFLAGS="${RUSTFLAGS:--C relocation-model=pic}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-linuxrt}"
DIST_DIR="${DIST_DIR:-$ROOT/dist}"
IOX2_DIR="$ROOT/third_party/iceoryx2"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

echo "Root: $ROOT"
echo "cmake: $(command -v cmake) ($(cmake --version | head -1))"
echo "rustc: $(command -v rustc) ($(rustc --version))"
echo "cargo: $(command -v cargo) ($(cargo --version))"

if [ ! -f "$IOX2_DIR/CMakeLists.txt" ]; then
    mkdir -p "$(dirname "$IOX2_DIR")"
    git clone --depth 1 https://github.com/eclipse-iceoryx/iceoryx2.git "$IOX2_DIR"
fi

mkdir -p "$BUILD_DIR" "$DIST_DIR"

cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
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

cp -f "$SO" "$DIST_DIR/libiox2_cif.so"
cp -f "$ROOT/native/include/iox2_cif.h" "$DIST_DIR/iox2_cif.h"
echo "Copied $SO -> $DIST_DIR/libiox2_cif.so"

if [ "$(id -u)" = 0 ]; then
    install -m 0755 "$DIST_DIR/libiox2_cif.so" /usr/local/lib/libiox2_cif.so
    ldconfig >/dev/null 2>&1 || true
    echo "Installed /usr/local/lib/libiox2_cif.so"
    if [ -d /home/lvuser ]; then
        install -o lvuser -g ni -m 0755 "$DIST_DIR/libiox2_cif.so" /home/lvuser/libiox2_cif.so || \
            install -m 0755 "$DIST_DIR/libiox2_cif.so" /home/lvuser/libiox2_cif.so
        echo "Installed /home/lvuser/libiox2_cif.so"
    fi
fi

if [ -n "$SMOKE" ]; then
    echo "Running smoke test..."
    (cd "$(dirname "$SMOKE")" && ./"$(basename "$SMOKE")")
    echo "Smoke test passed."
fi

echo "Done: $DIST_DIR/libiox2_cif.so"
file "$DIST_DIR/libiox2_cif.so" || true
ldd "$DIST_DIR/libiox2_cif.so" || true
