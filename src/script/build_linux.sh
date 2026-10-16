#!/bin/sh
# Usage: build_linux.sh [Release|Debug|RelWithDebInfo|MinSizeRel] [build-directory] [CMake options...]
set -eu

configuration="${1:-Release}"
build_directory="${2:-build/linux}"
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
if [ "$(uname -s)" != Linux ]; then
    echo "Run this script on Linux." >&2
    exit 1
fi

cd "$(dirname "$0")/../.."
if [ ! -f "$build_directory/CMakeCache.txt" ]; then
    set -- -G Ninja "$@"
fi
cmake -S . -B "$build_directory" -DCMAKE_BUILD_TYPE="$configuration" "$@"
cmake --build "$build_directory" --config "$configuration" --target nxemu --parallel "$jobs"
