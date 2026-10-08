// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <bit>

#include "yuzu_video_core/textures/texture.h"

namespace VideoCommon {

// Array layers do not contribute to mip dimensions. Linear, buffer and
// multisampled images cannot have a mip chain in the host API.
[[nodiscard]] constexpr u32 MipLevelCount(const Tegra::Texture::TICEntry& config) {
    using Tegra::Texture::TextureType;
    if (config.IsPitchLinear() || config.IsBuffer() ||
        config.texture_type == TextureType::Texture1DBuffer ||
        config.msaa_mode != Tegra::Texture::MsaaMode::Msaa1x1) {
        return 1;
    }
    u32 max_dimension = config.Width();
    if (config.texture_type != TextureType::Texture1D &&
        config.texture_type != TextureType::Texture1DArray) {
        max_dimension = std::max(max_dimension, config.Height());
    }
    if (config.texture_type == TextureType::Texture3D) {
        max_dimension = std::max(max_dimension, config.Depth());
    }
    return std::min(config.max_mip_level.Value() + 1,
                    static_cast<u32>(std::bit_width(max_dimension)));
}

} // namespace VideoCommon
