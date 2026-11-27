// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>

#include "yuzu_common/cityhash.h"
#include "yuzu_video_core/renderer_metal/fixed_pipeline_state.h"

namespace Metal {

size_t FixedPipelineState::Hash() const noexcept {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(this), sizeof(*this));
    return static_cast<size_t>(hash);
}

size_t DepthStencilState::Hash() const noexcept {
    u64 hash = static_cast<u64>(depth_compare);
    hash = hash * 31 + static_cast<u64>(depth_write);
    hash = hash * 31 + static_cast<u64>(stencil_enable);

    const auto mix_face = [&hash](const StencilFace& face) {
        hash = hash * 31 + static_cast<u64>(face.compare);
        hash = hash * 31 + static_cast<u64>(face.fail);
        hash = hash * 31 + static_cast<u64>(face.depth_fail);
        hash = hash * 31 + static_cast<u64>(face.pass);
        hash = hash * 31 + face.read_mask;
        hash = hash * 31 + face.write_mask;
    };
    mix_face(front);
    mix_face(back);

    return static_cast<size_t>(hash);
}

} // namespace Metal
