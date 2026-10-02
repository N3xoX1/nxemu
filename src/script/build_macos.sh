#!/bin/sh
set -eu

configuration="${1:-Release}"
build_directory="${2:-build}"
jobs="${NXEMU_BUILD_JOBS:-2}"

cd "$(dirname "$0")/../.."

cmake -S . -B "$build_directory" -G Ninja \
    -DCMAKE_BUILD_TYPE="$configuration" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DNXEMU_MACOS_BUNDLE=ON
cmake --build "$build_directory" --target nxemu_bundle --parallel "$jobs"
