// SPDX-FileCopyrightText: Copyright 2026 nxemu contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "yuzu_video_core/texture_cache/image_info.h"

namespace VideoCommon {

struct FullClearCoverage {
    bool all_aspects{};
    bool viewport_clip{};
    u32 first_layer{};
    u32 layer_count{};
    u32 area_width{};
    u32 area_height{};
    bool scissor_enabled{};
    s32 min_x{};
    s32 min_y{};
    s32 max_x{};
    s32 max_y{};
};

[[nodiscard]] inline bool IsWholeImageClear(const ImageInfo& image,
                                            const FullClearCoverage& clear) {
    return image.type == ImageType::e2D && image.num_samples == 1 && image.resources.levels == 1 &&
           image.resources.layers == 1 && image.size.depth == 1 && image.size.width != 0 &&
           image.size.height != 0 && clear.all_aspects && !clear.viewport_clip &&
           clear.first_layer == 0 && clear.layer_count == 1 &&
           clear.area_width >= image.size.width && clear.area_height >= image.size.height &&
           (!clear.scissor_enabled ||
            (clear.min_x == 0 && clear.min_y == 0 && clear.max_x >= s64(image.size.width) &&
             clear.max_y >= s64(image.size.height)));
}

// Joining can expand an image around the requested render target. Discard only
// when the clear still covers every subresource of that resulting allocation.
[[nodiscard]] inline bool ClearCoversJoinedImage(const ImageInfo& requested,
                                                 const ImageInfo& joined,
                                                 GPUVAddr requested_address,
                                                 GPUVAddr joined_address) {
    return requested_address == joined_address && requested.type == ImageType::e2D &&
           joined.type == ImageType::e2D && requested.size == joined.size &&
           requested.format == joined.format && requested.num_samples == 1 &&
           joined.num_samples == 1 && requested.resources.levels == 1 &&
           joined.resources.levels == 1 && requested.resources.layers == 1 &&
           joined.resources.layers == 1;
}

} // namespace VideoCommon
