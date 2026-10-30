// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>
#include <bit>

#include "yuzu_common/common_types.h"
#include "yuzu_shader_recompiler/backend/bindings.h"
#include "yuzu_shader_recompiler/frontend/ir/program.h"
#include "yuzu_shader_recompiler/profile.h"
#include "yuzu_shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {

constexpr u32 NUM_TEXTURE_SCALING_WORDS = 4;
constexpr u32 NUM_IMAGE_SCALING_WORDS = 2;
constexpr u32 NUM_TEXTURE_AND_IMAGE_SCALING_WORDS =
    NUM_TEXTURE_SCALING_WORDS + NUM_IMAGE_SCALING_WORDS;

struct RescalingLayout {
    alignas(16) std::array<u32, NUM_TEXTURE_SCALING_WORDS> rescaling_textures;
    alignas(16) std::array<u32, NUM_IMAGE_SCALING_WORDS> rescaling_images;
    u32 down_factor;
    /// Four bits per pixel local texture read: the color attachment it reads, or 0xF for none
    u32 attachment_reads;
};
struct RenderAreaLayout {
    std::array<f32, 4> render_area;
};
constexpr u32 RESCALING_LAYOUT_WORDS_OFFSET = offsetof(RescalingLayout, rescaling_textures);
constexpr u32 RESCALING_LAYOUT_DOWN_FACTOR_OFFSET = offsetof(RescalingLayout, down_factor);
constexpr u32 RESCALING_LAYOUT_ATTACHMENT_READS_OFFSET =
    offsetof(RescalingLayout, attachment_reads);
constexpr u32 NO_ATTACHMENT_READS = ~0U;
// Render-area dimensions must not alias texture/image scaling masks.
struct GraphicsPushConstants {
    RescalingLayout scaling{};
    RenderAreaLayout area{};
};
constexpr u32 RENDERAREA_LAYOUT_OFFSET = offsetof(GraphicsPushConstants, area);
static_assert(RENDERAREA_LAYOUT_OFFSET >= sizeof(RescalingLayout));
static_assert(sizeof(GraphicsPushConstants) <= 128);
inline GraphicsPushConstants MakeGraphicsPushConstants(
    const std::array<u32, NUM_TEXTURE_AND_IMAGE_SCALING_WORDS>& masks, f32 down_factor,
    u32 attachment_reads, const std::array<f32, 4>& area) {
    GraphicsPushConstants data{};
    for (size_t i = 0; i < NUM_TEXTURE_SCALING_WORDS; ++i)
        data.scaling.rescaling_textures[i] = masks[i];
    for (size_t i = 0; i < NUM_IMAGE_SCALING_WORDS; ++i)
        data.scaling.rescaling_images[i] = masks[NUM_TEXTURE_SCALING_WORDS + i];
    data.scaling.down_factor = std::bit_cast<u32>(down_factor);
    data.scaling.attachment_reads = attachment_reads;
    data.area.render_area = area;
    return data;
}

[[nodiscard]] std::vector<u32> EmitSPIRV(const Profile& profile, const RuntimeInfo& runtime_info,
                                         IR::Program& program, Bindings& bindings);

[[nodiscard]] inline std::vector<u32> EmitSPIRV(const Profile& profile, IR::Program& program) {
    Bindings binding;
    return EmitSPIRV(profile, {}, program, binding);
}

} // namespace Shader::Backend::SPIRV
