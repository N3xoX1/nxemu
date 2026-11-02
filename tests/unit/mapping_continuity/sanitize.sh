#!/bin/sh
set -eu
cd "$(dirname "$0")/../../.."
mkdir -p build
c++ -std=c++20 -O1 -g -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -DFMT_HEADER_ONLY -Isrc -Isrc/yuzu_video_core -Iexternal/boost -Iexternal/fmt/include \
    tests/unit/mapping_continuity/mapping.cpp \
    src/yuzu_video_core/host1x/gpu_device_memory_manager.cpp \
    src/yuzu_common/virtual_buffer.cpp tests/common/support_stubs.cpp \
    -o build/mapping-continuity-sanitized
./build/mapping-continuity-sanitized
