// SPDX-FileCopyrightText: Copyright 2026 nxemu contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <span>

#include "yuzu_video_core/texture_cache/image_info.h"

namespace VideoCommon {

// Conservative coverage for native, uncompressed 2D copies. Fragmented copies
// deliberately fall back to uploading the guest contents.
class ImageCopyCoverage {
public:
    explicit ImageCopyCoverage(const ImageInfo& destination_) : destination{destination_} {}

    void Add(const ImageInfo& source, std::span<const ImageCopy> copies) {
        if (destination.type != ImageType::e2D || source.type != ImageType::e2D ||
            destination.num_samples != 1 || source.num_samples != 1 ||
            destination.format != source.format || destination.resources.layers != 1 ||
            destination.size.depth != 1 || source.size.depth != 1) {
            return;
        }
        for (const auto& copy : copies) {
            const auto level = copy.dst_subresource.base_level;
            const auto src_level = copy.src_subresource.base_level;
            if (level < 0 || level >= destination.resources.levels || level >= MAX_MIP_LEVELS ||
                src_level < 0 || src_level >= source.resources.levels ||
                src_level >= MAX_MIP_LEVELS || copy.dst_subresource.base_layer != 0 ||
                copy.dst_subresource.num_layers != 1 || copy.src_subresource.base_layer < 0 ||
                copy.src_subresource.num_layers != 1 ||
                copy.src_subresource.base_layer >= source.resources.layers ||
                copy.dst_offset != Offset3D{0, 0, 0} || copy.src_offset.x < 0 ||
                copy.src_offset.y < 0 || copy.src_offset.z != 0) {
                continue;
            }
            const u32 width = std::max(destination.size.width >> level, 1U);
            const u32 height = std::max(destination.size.height >> level, 1U);
            const u32 src_width = std::max(source.size.width >> src_level, 1U);
            const u32 src_height = std::max(source.size.height >> src_level, 1U);
            const u32 src_x = static_cast<u32>(copy.src_offset.x);
            const u32 src_y = static_cast<u32>(copy.src_offset.y);
            if (copy.extent.width != width || copy.extent.height != height ||
                copy.extent.depth != 1 || src_x > src_width || src_y > src_height ||
                copy.extent.width > src_width - src_x || copy.extent.height > src_height - src_y) {
                continue;
            }
            covered_levels |= 1U << level;
        }
    }

    [[nodiscard]] bool Complete() const {
        const auto levels = destination.resources.levels;
        return levels > 0 && levels <= MAX_MIP_LEVELS && covered_levels == (1U << levels) - 1;
    }

private:
    const ImageInfo& destination;
    u32 covered_levels{};
};

} // namespace VideoCommon
