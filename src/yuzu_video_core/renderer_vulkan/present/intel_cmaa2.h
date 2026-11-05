// SPDX-FileCopyrightText: Copyright 2026 NXEmu contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <vector>
#include "yuzu_video_core/renderer_vulkan/present/anti_alias_pass.h"
#include "yuzu_video_core/vulkan_common/vulkan_memory_allocator.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
class Device;
class Scheduler;

// Port of Intel CMAA2 compute detection and candidates with a fullscreen fragment resolve.
// Intel CMAA2.hlsl remains Apache-2.0, upstream copyright (c) Intel 2018.
class IntelCMAA2 final : public AntiAliasPass {
public:
    IntelCMAA2(const Device& device, MemoryAllocator& allocator, size_t image_count, VkExtent2D extent);
    ~IntelCMAA2() override;
    void Draw(Scheduler& scheduler, size_t image_index, VkImage* image, VkImageView* view) override;

private:
    struct Frame {
        vk::Image output;
        vk::ImageView output_view;
        vk::Framebuffer output_framebuffer;
        vk::Image edges;
        vk::ImageView edges_view;
        vk::Image heads;
        vk::ImageView heads_view;
        std::array<vk::Buffer, 5> buffers; // candidates, locations, blended items, control, indirect
        vk::DescriptorSets descriptors;
        bool initialized{};
    };
    enum Buffer : size_t {Candidates, Locations, Items, Control, Indirect};
    enum Pass : size_t {Edges, Arguments, CandidatesPass, PassCount};
    void CreateLayout();
    void CreatePipelines();
    void CreateFrames();
    void UpdateInputDescriptor(Frame& frame, VkImageView image);

    const Device& device;
    MemoryAllocator& allocator;
    VkExtent2D extent;
    vk::DescriptorPool descriptor_pool;
    vk::DescriptorSetLayout descriptor_layout;
    vk::PipelineLayout pipeline_layout;
    vk::Sampler sampler;
    std::array<vk::ShaderModule, PassCount> shaders;
    std::array<vk::Pipeline, PassCount> pipelines;
    vk::RenderPass render_pass;
    vk::ShaderModule apply_vertex;
    vk::ShaderModule apply_fragment;
    vk::Pipeline apply_pipeline;
    // Frame descriptor sets must be destroyed before descriptor_pool.
    std::vector<Frame> frames;
};
} // namespace Vulkan
