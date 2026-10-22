#!/bin/sh
# Usage: build_macos.sh [Release|Debug|RelWithDebInfo|MinSizeRel] [build-directory] [CMake options...]
set -eu

configuration="${1:-Release}"
build_directory="${2:-build}"
jobs="${NXEMU_BUILD_JOBS:-2}"
if [ "$#" -gt 0 ]; then shift; fi
if [ "$#" -gt 0 ]; then shift; fi

case "$configuration" in
    Release|Debug|RelWithDebInfo|MinSizeRel) ;;
    *) echo "Unsupported build configuration: $configuration" >&2; exit 1 ;;
esac
case "$jobs" in
    ''|0|*[!0-9]*) echo "NXEMU_BUILD_JOBS must be a positive integer" >&2; exit 1 ;;
esac
if [ "$(uname -s)" != Darwin ]; then
    echo "Run this script on macOS with Xcode installed." >&2
    exit 1
fi

cd "$(dirname "$0")/../.."

# Preserve the generator of an existing build tree (including Unix Makefiles).
if [ ! -f "$build_directory/CMakeCache.txt" ]; then
    set -- -G Ninja "$@"
fi
cmake -S . -B "$build_directory" \
    -DCMAKE_BUILD_TYPE="$configuration" \
    -DCMAKE_OSX_ARCHITECTURES="${NXEMU_MACOS_ARCHITECTURE:-arm64}" \
    -DNXEMU_MACOS_BUNDLE=ON "$@"
cmake --build "$build_directory" --config "$configuration" --target nxemu_bundle --parallel "$jobs"
