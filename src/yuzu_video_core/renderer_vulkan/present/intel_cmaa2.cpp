// SPDX-FileCopyrightText: Copyright 2026 NXEmu contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Original Intel CMAA2 HLSL: Copyright (c) Intel 2018; Apache-2.0.
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <tuple>
#include "yuzu_video_core/host_shaders/intel_cmaa2_apply_vert_spv.h"
#include "yuzu_video_core/host_shaders/intel_cmaa2_apply_frag_spv.h"
#include "yuzu_video_core/host_shaders/intel_cmaa2_edges_spv.h"
#include "yuzu_video_core/host_shaders/intel_cmaa2_args_spv.h"
#include "yuzu_video_core/host_shaders/intel_cmaa2_candidates_spv.h"
#include "yuzu_video_core/renderer_vulkan/present/intel_cmaa2.h"
#include "yuzu_video_core/renderer_vulkan/present/util.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {
constexpr VkFormat ColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat IntegerFormat = VK_FORMAT_R32_UINT;
constexpr VkFormat EdgeFormat = VK_FORMAT_R8_UINT;

u32 CeilDiv(u32 a, u32 b) {
    return (a / b) + static_cast<u32>(a % b != 0);
}

vk::Image MakeComputeImage(MemoryAllocator& allocator, VkExtent2D extent, VkFormat format,
                           VkImageUsageFlags usage) {
    // Generic presentation images request COLOR_ATTACHMENT and TRANSFER_DST as well.
    // Intel's scratch integer images only need storage access, which is supported
    // on more devices (and avoids unnecessary required format features).
    const VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {.width = extent.width, .height = extent.height, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    return allocator.CreateImage(info);
}

vk::Buffer MakeStorageBuffer(MemoryAllocator& allocator, VkDeviceSize size, VkBufferUsageFlags extra = 0) {
    const VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = std::max<VkDeviceSize>(size, 16),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | extra,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    return allocator.CreateBuffer(info, MemoryUsage::DeviceLocal);
}

void ComputeBarrier(vk::CommandBuffer& cmd, bool indirect = false) {
    const VkMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                         (indirect ? VK_ACCESS_INDIRECT_COMMAND_READ_BIT : 0U),
    };
    cmd.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                            (indirect ? VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT : 0),
                        0, barrier);
}
} // namespace

IntelCMAA2::IntelCMAA2(const Device& device_, MemoryAllocator& allocator_, size_t count,
                       VkExtent2D size)
    : device(device_), allocator(allocator_), extent(size) {
    if (size.width == 0 || size.height == 0) {
        throw std::invalid_argument("Intel CMAA2 requires nonzero source dimensions");
    }
    // Intel packs candidate coordinates as (x << 18) | y, with 14 bits for y.
    // Supporting larger images requires changing Intel's packing algorithm.
    if (size.width > 16384 || size.height > 16384) {
        throw std::invalid_argument("Intel CMAA2 source dimensions exceed packed-coordinate limits");
    }
    if (!device.IsFormatSupported(ColorFormat,
                                  VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
                                  FormatType::Optimal) ||
        !device.IsFormatSupported(IntegerFormat,
                                  VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT,
                                  FormatType::Optimal) ||
        !device.IsFormatSupported(EdgeFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
                                  FormatType::Optimal)) {
        throw std::runtime_error("Intel CMAA2 requires RGBA16F render-target, R32UI and R8UI storage-image support");
    }
    const u64 pixels = static_cast<u64>(size.width) * size.height;
    if (pixels > std::numeric_limits<u32>::max() / 8) {
        throw std::invalid_argument("Intel CMAA2 source dimensions exceed intermediate-buffer limits");
    }
    const auto limits = device.GetPhysical().GetProperties().limits;
    if (std::max<u64>(1, pixels / 2) * 8 > limits.maxStorageBufferRange ||
        (std::max<u64>(1, pixels / 4) + 127) / 128 > limits.maxComputeWorkGroupCount[0]) {
        throw std::invalid_argument("Intel CMAA2 intermediates exceed this device's buffer/dispatch limits");
    }
    frames.resize(count);
    sampler = CreateWrappedSampler(device, VK_FILTER_NEAREST);
    CreateLayout();
    CreatePipelines();
    CreateFrames();
}
IntelCMAA2::~IntelCMAA2() = default;

void IntelCMAA2::CreateLayout() {
    constexpr std::array<VkDescriptorType, 9> kinds{
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // u1: packed edges R8UI
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  // u2: shape candidates
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  // u3: deferred locations
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  // u4: deferred blended items
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // u5: linked-list heads R32UI
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  // u6: control
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  // u7: indirect argument buffer
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   // t0 at binding 8
        VK_DESCRIPTOR_TYPE_SAMPLER,         // s0 at binding 12
    };
    constexpr std::array<u32, 9> binding_ids{1, 2, 3, 4, 5, 6, 7, 8, 12};
    std::array<VkDescriptorSetLayoutBinding, kinds.size()> bindings{};
    for (size_t n = 0; n < bindings.size(); ++n) {
        bindings[n] = VkDescriptorSetLayoutBinding{
            .binding = binding_ids[n], .descriptorType = kinds[n], .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT |
                          ((binding_ids[n] == 4 || binding_ids[n] == 5 ||
                            binding_ids[n] == 8 || binding_ids[n] == 12)
                               ? VK_SHADER_STAGE_FRAGMENT_BIT : 0U),
        };
    }
    descriptor_layout = device.GetLogical().CreateDescriptorSetLayout(VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<u32>(bindings.size()), .pBindings = bindings.data(),
    });
    const std::array<VkDescriptorPoolSize, 4> pool_sizes{{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, static_cast<u32>(2 * frames.size())},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<u32>(5 * frames.size())},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, static_cast<u32>(frames.size())},
        {VK_DESCRIPTOR_TYPE_SAMPLER, static_cast<u32>(frames.size())},
    }};
    descriptor_pool = device.GetLogical().CreateDescriptorPool(VkDescriptorPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = static_cast<u32>(frames.size()),
        .poolSizeCount = static_cast<u32>(pool_sizes.size()), .pPoolSizes = pool_sizes.data(),
    });
    pipeline_layout = CreateWrappedPipelineLayout(device, descriptor_layout);
}

void IntelCMAA2::CreatePipelines() {
    shaders[Edges] = CreateWrappedShaderModule(device, INTEL_CMAA2_EDGES_SPV);
    shaders[Arguments] = CreateWrappedShaderModule(device, INTEL_CMAA2_ARGS_SPV);
    shaders[CandidatesPass] = CreateWrappedShaderModule(device, INTEL_CMAA2_CANDIDATES_SPV);
    for (size_t i = 0; i < PassCount; ++i) {
        const VkPipelineShaderStageCreateInfo stage{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *shaders[i], .pName = "main",
        };
        pipelines[i] = device.GetLogical().CreateComputePipeline(VkComputePipelineCreateInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = stage, .layout = *pipeline_layout,
        });
    }
    render_pass = CreateWrappedRenderPass(device, ColorFormat, VK_IMAGE_LAYOUT_UNDEFINED);
    apply_vertex = CreateWrappedShaderModule(device, INTEL_CMAA2_APPLY_VERT_SPV);
    apply_fragment = CreateWrappedShaderModule(device, INTEL_CMAA2_APPLY_FRAG_SPV);
    apply_pipeline = CreateWrappedPipeline(device, render_pass, pipeline_layout,
                                           std::tie(apply_vertex, apply_fragment));
}

void IntelCMAA2::CreateFrames() {
    const u64 pixels = static_cast<u64>(extent.width) * extent.height;
    // Keep Intel's memory/performance compromise; a saturated pathological frame may drop candidates.
    const VkDeviceSize candidate_bytes = std::max<u64>(1, pixels / 4) * sizeof(u32);
    // Unused by the fullscreen resolve; retain a dummy binding for the shared layout.
    const VkDeviceSize location_bytes = 16;
    const VkDeviceSize item_bytes = std::max<u64>(1, pixels / 2) * 2 * sizeof(u32);
    for (Frame& frame : frames) {
        frame.output = MakeComputeImage(allocator, extent, ColorFormat,
                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        frame.output_view = CreateWrappedImageView(device, frame.output, ColorFormat);
        frame.output_framebuffer =
            CreateWrappedFramebuffer(device, render_pass, frame.output_view, extent);
        frame.edges = MakeComputeImage(allocator, {CeilDiv(extent.width, 2), extent.height},
                                       EdgeFormat, VK_IMAGE_USAGE_STORAGE_BIT);
        frame.edges_view = CreateWrappedImageView(device, frame.edges, EdgeFormat);
        frame.heads = MakeComputeImage(allocator,
                                       {CeilDiv(extent.width, 2), CeilDiv(extent.height, 2)},
                                       IntegerFormat, VK_IMAGE_USAGE_STORAGE_BIT);
        frame.heads_view = CreateWrappedImageView(device, frame.heads, IntegerFormat);
        frame.buffers[Candidates] = MakeStorageBuffer(allocator, candidate_bytes);
        frame.buffers[Locations] = MakeStorageBuffer(allocator, location_bytes);
        frame.buffers[Items] = MakeStorageBuffer(allocator, item_bytes);
        frame.buffers[Control] = MakeStorageBuffer(allocator, 16 * sizeof(u32));
        frame.buffers[Indirect] = MakeStorageBuffer(allocator, 4 * sizeof(u32), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        const VkDescriptorSetLayout layout = *descriptor_layout;
        frame.descriptors = CreateWrappedDescriptorSets(descriptor_pool, {layout});
        const VkDescriptorSet target = frame.descriptors[0];
        const std::array<VkDescriptorImageInfo, 2> storage_images{{
            {VK_NULL_HANDLE, *frame.edges_view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, *frame.heads_view, VK_IMAGE_LAYOUT_GENERAL},
        }};
        const std::array<u32, 2> image_bindings{1, 5};
        const std::array<u32, 5> buffer_bindings{2, 3, 4, 6, 7};
        std::array<VkDescriptorBufferInfo, 5> storage_buffers{};
        const std::array<VkDeviceSize, 5> sizes{{candidate_bytes, location_bytes, item_bytes, 64, 16}};
        for (size_t k = 0; k < storage_buffers.size(); ++k) {
            storage_buffers[k] = { *frame.buffers[k], 0, sizes[k] };
        }
        const VkDescriptorImageInfo sampler_info{*sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        std::array<VkWriteDescriptorSet, 8> writes{};
        for (size_t k = 0; k < storage_images.size(); ++k) {
            writes[k] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                         .dstSet = target, .dstBinding = image_bindings[k],
                         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                         .pImageInfo = &storage_images[k]};
        }
        for (size_t k = 0; k < storage_buffers.size(); ++k) {
            writes[2+k] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                           .dstSet = target, .dstBinding = buffer_bindings[k],
                           .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           .pBufferInfo = &storage_buffers[k]};
        }
        writes[7] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = target, .dstBinding = 12,
                     .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                     .pImageInfo = &sampler_info};
        device.GetLogical().UpdateDescriptorSets(writes, {});
    }
}

void IntelCMAA2::UpdateInputDescriptor(Frame& frame, VkImageView source_view) {
    const VkDescriptorImageInfo info{VK_NULL_HANDLE, source_view, VK_IMAGE_LAYOUT_GENERAL};
    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = frame.descriptors[0], .dstBinding = 8, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .pImageInfo = &info,
    };
    device.GetLogical().UpdateDescriptorSets(write, {});
}

void IntelCMAA2::Draw(Scheduler& scheduler, size_t image_index, VkImage* inout_image,
                      VkImageView* inout_view) {
    Frame& frame = frames.at(image_index);
    const VkImage input = *inout_image;
    UpdateInputDescriptor(frame, *inout_view);
    const VkDescriptorSet set = frame.descriptors[0];
    const VkPipelineLayout layout = *pipeline_layout;
    const std::array<VkPipeline, PassCount> pass_handles{{
        *pipelines[Edges], *pipelines[Arguments], *pipelines[CandidatesPass]}};
    const VkPipeline graphics_pipeline = *apply_pipeline;
    const VkRenderPass rp = *render_pass;
    const VkFramebuffer framebuffer = *frame.output_framebuffer;
    const VkBuffer indirect = *frame.buffers[Indirect];
    const VkBuffer control = *frame.buffers[Control];
    const VkImage output = *frame.output;
    const VkImage edges = *frame.edges;
    const VkImage heads = *frame.heads;
    const VkExtent2D size = extent;
    const bool first = !frame.initialized;
    frame.initialized = true;

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([=](vk::CommandBuffer cmd) {
        if (!first) {
            // Reused scratch images/buffers were read by compute, indirect and
            // fragment stages in the previous submission. Order all reuse.
            const VkMemoryBarrier reuse{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            };
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, reuse);
        }
        // Input is already in GENERAL (the presentation pipeline contract).
        // It may come from rendering, compute, or a framebuffer transfer.
        const VkImageMemoryBarrier source_barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = input,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, source_barrier);
        if (first) {
            const std::array<VkImage, 2> images{edges, heads};
            for (VkImage image : images) {
                const VkImageMemoryBarrier barrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = image,
                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                };
                cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, barrier);
            }
        }
        const auto run = [&](Pass pass, u32 x, u32 y) {
            cmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, pass_handles[pass]);
            cmd.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, set, {});
            cmd.Dispatch(x, y, 1);
        };
        const auto run_indirect = [&](Pass pass) {
            cmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, pass_handles[pass]);
            cmd.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, set, {});
            cmd.DispatchIndirect(indirect, 0);
        };
        // Arguments resets the candidate and item counters before CandidatesPass.
        // Only the first use of a slot needs a fill and its transfer barrier.
        if (first) {
            cmd.FillBuffer(control, 0, 64, 0);
            const VkMemoryBarrier control_barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            };
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, control_barrier);
        }
        // Edges initializes every linked-list head to the empty sentinel.
        // No fullscreen source copy: the final fragment shader resolves all pixels.
        run(Edges, CeilDiv(size.width, 28), CeilDiv(size.height, 28));
        ComputeBarrier(cmd);
        run(Arguments, 2, 1);
        ComputeBarrier(cmd, true);
        run_indirect(CandidatesPass);
        const VkMemoryBarrier list_read_barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, list_read_barrier);
        BeginRenderPass(cmd, rp, framebuffer, size);
        cmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
        cmd.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, set, {});
        cmd.Draw(3, 1, 0, 0);
        cmd.EndRenderPass();
        const VkMemoryBarrier output_read_barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, output_read_barrier);
    });
    *inout_image = output;
    *inout_view = *frame.output_view;
}
} // namespace Vulkan
