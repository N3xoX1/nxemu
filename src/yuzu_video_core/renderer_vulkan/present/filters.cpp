// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/common_types.h"
#include "nxemu-module-spec/video.h"
#include "yuzu_video_core/host_shaders/present_catmull_rom_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_cas_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_cas_quad_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_cas_catmull_quad_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_cas_catmull_frag_spv.h"

#include "yuzu_video_core/host_shaders/present_area_frag_spv.h"
#include "yuzu_video_core/host_shaders/present_bicubic_frag_spv.h"
#include "yuzu_video_core/host_shaders/present_gaussian_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_easu_fp16_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_easu_fp32_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_rcas_fp16_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_rcas_fp32_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_scaleforce_fp16_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_scaleforce_fast_fp16_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_scaleforce_fast_fp32_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_scaleforce_fp32_frag_spv.h"
#include "yuzu_video_core/renderer_vulkan/present/filters.h"
#include "yuzu_video_core/renderer_vulkan/present/util.h"
#include "yuzu_video_core/renderer_vulkan/vk_shader_util.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

std::unique_ptr<WindowAdaptPass> MakeCatmullRom(const Device& device, VkFormat format) {
    return std::make_unique<WindowAdaptPass>(device, format, CreateWrappedSampler(device),
        BuildShader(device, PRESENT_CATMULL_ROM_FRAG_SPV), false, true);
}

std::unique_ptr<WindowAdaptPass> MakeCas(const Device& device, VkFormat format, ScalingFilter filter) {
    const bool catmull = filter == ScalingFilter::CasCatmullRom || filter == ScalingFilter::CasCatmullRomFast;
    const bool quad = device.IsSubgroupStageSupported(VK_SHADER_STAGE_FRAGMENT_BIT) &&
        device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_BASIC_BIT) &&
        device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_QUAD_BIT);
    auto shader = catmull
        ? (quad ? BuildShader(device, VULKAN_PRESENT_CAS_CATMULL_QUAD_FRAG_SPV)
                : BuildShader(device, VULKAN_PRESENT_CAS_CATMULL_FRAG_SPV))
        : (quad ? BuildShader(device, VULKAN_PRESENT_CAS_QUAD_FRAG_SPV)
                : BuildShader(device, VULKAN_PRESENT_CAS_FRAG_SPV));
    return std::make_unique<WindowAdaptPass>(device, format,
        catmull ? CreateWrappedSampler(device)
                : filter == ScalingFilter::CasNearest ? CreateNearestNeighborSampler(device)
                                                     : CreateBilinearSampler(device),
        std::move(shader), false, catmull, true);
}

namespace {

vk::ShaderModule SelectScaleForceShader(const Device& device) {
    if (device.IsFloat16Supported()) {
        return BuildShader(device, VULKAN_PRESENT_SCALEFORCE_FP16_FRAG_SPV);
    } else {
        return BuildShader(device, VULKAN_PRESENT_SCALEFORCE_FP32_FRAG_SPV);
    }
}

} // Anonymous namespace

std::unique_ptr<WindowAdaptPass> MakeNearestNeighbor(const Device& device, VkFormat frame_format) {
    return std::make_unique<WindowAdaptPass>(device, frame_format,
                                             CreateNearestNeighborSampler(device),
                                             BuildShader(device, VULKAN_PRESENT_FRAG_SPV));
}

std::unique_ptr<WindowAdaptPass> MakeArea(const Device& device, VkFormat frame_format) {
    return std::make_unique<WindowAdaptPass>(device, frame_format,
        CreateBilinearSampler(device, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
        BuildShader(device, PRESENT_AREA_FRAG_SPV));
}

std::unique_ptr<WindowAdaptPass> MakeBilinear(const Device& device, VkFormat frame_format) {
    return std::make_unique<WindowAdaptPass>(device, frame_format, CreateBilinearSampler(device, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
                                             BuildShader(device, VULKAN_PRESENT_FRAG_SPV));
}

std::unique_ptr<WindowAdaptPass> MakeFsrEasu(const Device& device, VkFormat frame_format) {
    auto shader = device.IsFloat16Supported() && device.IsShaderInt16Supported()
        ? BuildShader(device, VULKAN_PRESENT_EASU_FP16_FRAG_SPV)
        : BuildShader(device, VULKAN_PRESENT_EASU_FP32_FRAG_SPV);
    return std::make_unique<WindowAdaptPass>(device, frame_format, CreateBilinearSampler(device),
                                             std::move(shader));
}

std::unique_ptr<WindowAdaptPass> MakeFsrRcas(const Device& device, VkFormat frame_format) {
    auto shader = device.IsFloat16Supported() && device.IsShaderInt16Supported()
        ? BuildShader(device, VULKAN_PRESENT_RCAS_FP16_FRAG_SPV)
        : BuildShader(device, VULKAN_PRESENT_RCAS_FP32_FRAG_SPV);
    return std::make_unique<WindowAdaptPass>(device, frame_format, CreateBilinearSampler(device),
                                             std::move(shader), true);
}

std::unique_ptr<WindowAdaptPass> MakeBicubic(const Device& device, VkFormat frame_format) {
    return std::make_unique<WindowAdaptPass>(device, frame_format, CreateBilinearSampler(device),
                                             BuildShader(device, PRESENT_BICUBIC_FRAG_SPV));
}

std::unique_ptr<WindowAdaptPass> MakeGaussian(const Device& device, VkFormat frame_format) {
    return std::make_unique<WindowAdaptPass>(device, frame_format, CreateBilinearSampler(device),
                                             BuildShader(device, PRESENT_GAUSSIAN_FRAG_SPV));
}

std::unique_ptr<WindowAdaptPass> MakeScaleForce(const Device& device, VkFormat frame_format) {
    return std::make_unique<WindowAdaptPass>(device, frame_format, CreateBilinearSampler(device),
                                             SelectScaleForceShader(device));
}

std::unique_ptr<WindowAdaptPass> MakeScaleForceFast(const Device& device, VkFormat frame_format) {
    auto shader = device.IsFloat16Supported()
        ? BuildShader(device, VULKAN_PRESENT_SCALEFORCE_FAST_FP16_FRAG_SPV)
        : BuildShader(device, VULKAN_PRESENT_SCALEFORCE_FAST_FP32_FRAG_SPV);
    return std::make_unique<WindowAdaptPass>(device, frame_format, CreateBilinearSampler(device),
                                           std::move(shader));
}

} // namespace Vulkan
