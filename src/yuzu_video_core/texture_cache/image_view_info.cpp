// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>

#include "yuzu_common/yuzu_assert.h"
#include "yuzu_video_core/texture_cache/image_view_info.h"
#include "yuzu_video_core/texture_cache/mip_levels.h"
#include "yuzu_video_core/texture_cache/texture_cache_base.h"
#include "yuzu_video_core/texture_cache/types.h"
#include "yuzu_video_core/texture_cache/util.h"
#include "yuzu_video_core/textures/texture.h"

namespace VideoCommon {

namespace {

using Tegra::Texture::TextureType;

constexpr u8 RENDER_TARGET_SWIZZLE = std::numeric_limits<u8>::max();

[[nodiscard]] u8 CastSwizzle(SwizzleSource source) {
    const u8 casted = static_cast<u8>(source);
    ASSERT(static_cast<SwizzleSource>(casted) == source);
    return casted;
}

} // Anonymous namespace

ImageViewInfo::ImageViewInfo(const TICEntry& config, s32 base_layer) noexcept
    : format{PixelFormatFromTIC(config)}, x_source{CastSwizzle(config.x_source)},
      y_source{CastSwizzle(config.y_source)}, z_source{CastSwizzle(config.z_source)},
      w_source{CastSwizzle(config.w_source)} {
    const s32 last_level = static_cast<s32>(MipLevelCount(config)) - 1;
    range.base = SubresourceBase{
        .level = std::min(static_cast<s32>(config.res_min_mip_level), last_level),
        .layer = base_layer,
    };
    const s32 max_level = std::clamp(static_cast<s32>(config.res_max_mip_level),
                                   range.base.level, last_level);
    range.extent.levels = max_level - range.base.level + 1;

    switch (config.texture_type) {
    case TextureType::Texture1D:
        ASSERT(config.Height() == 1);
        ASSERT(config.Depth() == 1);
        type = ImageViewType::e1D;
        break;
    case TextureType::Texture2D:
    case TextureType::Texture2DNoMipmap:
        ASSERT(config.Depth() == 1);
        type = config.normalized_coords ? ImageViewType::e2D : ImageViewType::Rect;
        break;
    case TextureType::Texture3D:
        type = ImageViewType::e3D;
        break;
    case TextureType::TextureCubemap:
        ASSERT(config.Depth() == 1);
        type = ImageViewType::Cube;
        range.extent.layers = 6;
        break;
    case TextureType::Texture1DArray:
        type = ImageViewType::e1DArray;
        range.extent.layers = config.Depth();
        break;
    case TextureType::Texture2DArray:
        type = ImageViewType::e2DArray;
        range.extent.layers = config.Depth();
        break;
    case TextureType::Texture1DBuffer:
        type = ImageViewType::Buffer;
        break;
    case TextureType::TextureCubeArray:
        type = ImageViewType::CubeArray;
        range.extent.layers = config.Depth() * 6;
        break;
    default:
        ASSERT_MSG(false, "Invalid texture_type={}", static_cast<int>(config.texture_type.Value()));
        break;
    }
}

ImageViewInfo::ImageViewInfo(ImageViewType type_, PixelFormat format_,
                             SubresourceRange range_) noexcept
    : type{type_}, format{format_}, range{range_}, x_source{RENDER_TARGET_SWIZZLE},
      y_source{RENDER_TARGET_SWIZZLE}, z_source{RENDER_TARGET_SWIZZLE},
      w_source{RENDER_TARGET_SWIZZLE} {}

bool ImageViewInfo::IsRenderTarget() const noexcept {
    return x_source == RENDER_TARGET_SWIZZLE && y_source == RENDER_TARGET_SWIZZLE &&
           z_source == RENDER_TARGET_SWIZZLE && w_source == RENDER_TARGET_SWIZZLE;
}

} // namespace VideoCommon
