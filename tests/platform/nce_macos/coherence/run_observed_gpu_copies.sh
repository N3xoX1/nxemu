#!/bin/bash
# Build the real-GPU validation against the same MoltenVK shipped with the app.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"
OUT="${1:-$REPO/build/observed_gpu_copies_validation}"
LIBDIR="$REPO/bin/arm64/Release/NxEmu.app/Contents/Frameworks"
mkdir -p "$OUT"
clang++ -std=c++20 -O2 -I "$REPO/src" -I "$REPO/external/vulkan-headers/include" \
    "$HERE/test_observed_gpu_copies.cpp" "$LIBDIR/libMoltenVK.dylib" \
    -Wl,-rpath,"$LIBDIR" -o "$OUT/test_observed_gpu_copies"
"$OUT/test_observed_gpu_copies"
