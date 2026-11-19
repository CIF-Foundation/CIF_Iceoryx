#!/usr/bin/env bash
# Fetch the iceoryx2 revision pinned in third_party/iceoryx2.rev.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REV_FILE="$ROOT/third_party/iceoryx2.rev"
IOX2_DIR="$ROOT/third_party/iceoryx2"
URL="https://github.com/eclipse-iceoryx/iceoryx2.git"
REV="$(sed -e 's/#.*//' -e 's/[[:space:]]//g' "$REV_FILE" | sed -n '/./p' | head -1)"

if [ -z "$REV" ]; then
    echo "No revision in $REV_FILE" >&2
    exit 1
fi

if [ -f "$IOX2_DIR/CMakeLists.txt" ]; then
    head="$(git -C "$IOX2_DIR" rev-parse HEAD 2>/dev/null || true)"
    if [ "$head" != "$REV" ]; then
        echo "third_party/iceoryx2 is ${head:-not a git checkout}, pinned revision is $REV." >&2
        echo "Remove third_party/iceoryx2 and rebuild." >&2
        exit 1
    fi
    echo "iceoryx2 $REV"
    exit 0
fi

echo "Fetching iceoryx2 $REV"
rm -rf "$IOX2_DIR"
mkdir -p "$IOX2_DIR"
git -C "$IOX2_DIR" init
git -C "$IOX2_DIR" remote add origin "$URL"
git -C "$IOX2_DIR" fetch --depth 1 origin "$REV"
git -C "$IOX2_DIR" checkout --detach FETCH_HEAD
