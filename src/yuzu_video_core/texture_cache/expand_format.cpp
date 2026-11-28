// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>

#include "yuzu_video_core/texture_cache/expand_format.h"

namespace VideoCommon {

namespace {

using VideoCore::Surface::PixelFormat;

[[nodiscard]] u64 TexelCount(const BufferImageCopy& copy) {
    return static_cast<u64>(copy.image_extent.width) * copy.image_extent.height *
           copy.image_extent.depth * copy.image_subresource.num_layers;
}

[[nodiscard]] constexpr u8 Widen4To8(u8 nibble) {
    return static_cast<u8>((nibble << 4) | nibble);
}

/// G4R4_UNORM _> R8G8_UNORM.
void ExpandG4R4(std::span<const u8> input, std::span<u8> output, u64 texels) {
    for (u64 i = 0; i < texels; ++i) {
        const u8 packed = input[i];
        output[i * 2 + 0] = Widen4To8(static_cast<u8>(packed & 0xF));
        output[i * 2 + 1] = Widen4To8(static_cast<u8>(packed >> 4));
    }
}

/// R32G32B32_FLOAT -> R32G32B32A32_FLOAT.
void ExpandR32G32B32(std::span<const u8> input, std::span<u8> output, u64 texels) {
    static constexpr float ONE = 1.0f;
    for (u64 i = 0; i < texels; ++i) {
        std::memcpy(output.data() + i * 16, input.data() + i * 12, 12);
        std::memcpy(output.data() + i * 16 + 12, &ONE, sizeof(ONE));
    }
}

} // Anonymous namespace

bool IsPixelFormatExpanded(PixelFormat format) {
    switch (format) {
    case PixelFormat::G4R4_UNORM:
    case PixelFormat::R32G32B32_FLOAT:
        return true;
    default:
        return false;
    }
}

u32 ExpandedBytesPerTexel(PixelFormat format) {
    switch (format) {
    case PixelFormat::G4R4_UNORM:
        return 2;
    case PixelFormat::R32G32B32_FLOAT:
        return 16;
    default:
        return 0;
    }
}

void ExpandPixelFormat(std::span<const u8> input, std::span<u8> output,
                       const BufferImageCopy& copy, PixelFormat format) {
    const u64 texels = TexelCount(copy);
    if (texels == 0) {
        return;
    }
    const u64 in_bytes = texels * (format == PixelFormat::G4R4_UNORM ? 1 : 12);
    const u64 out_bytes = texels * ExpandedBytesPerTexel(format);
    if (input.size_bytes() < in_bytes || output.size_bytes() < out_bytes) {
        return;
    }

    switch (format) {
    case PixelFormat::G4R4_UNORM:
        ExpandG4R4(input, output, texels);
        break;
    case PixelFormat::R32G32B32_FLOAT:
        ExpandR32G32B32(input, output, texels);
        break;
    default:
        break;
    }
}

} // namespace VideoCommon
