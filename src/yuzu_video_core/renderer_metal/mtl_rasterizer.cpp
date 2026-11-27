// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <fstream>
#include <cstdlib>
#include <filesystem>
#include <atomic>
#include <thread>
#include <cstdio>
#include <bit>
#include <chrono>
#include <map>
#include <bitset>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <boost/container/small_vector.hpp>

#include "yuzu_common/alignment.h"
#include "yuzu_common/cityhash.h"
#include "yuzu_common/div_ceil.h"
#include "yuzu_common/fs/fs.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/scope_exit.h"
#include "yuzu_common/settings.h"
#include "video_settings.h"
#include "yuzu_video_core/renderer_metal/metal_options.h"
#include "yuzu_video_core/control/channel_state.h"
#include "yuzu_video_core/dirty_flags.h"
#include "yuzu_video_core/engines/fermi_2d.h"
#include "yuzu_video_core/engines/kepler_compute.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/host1x/host1x.h"
#include "yuzu_shader_recompiler/backend/spirv/emit_spirv.h"
#include "yuzu_shader_recompiler/runtime_info.h"
#include "yuzu_video_core/memory_manager.h"
#include "yuzu_video_core/metal_common/metal_device.h"
#include "yuzu_video_core/surface.h"
#include "yuzu_video_core/renderer_metal/mtl_rasterizer.h"
#include "yuzu_video_core/renderer_metal/maxwell_to_mtl.h"
#include "yuzu_video_core/renderer_metal/mtl_scheduler.h"
#include "yuzu_video_core/textures/texture.h"
#include "yuzu_video_core/renderer_vulkan/descriptor_lookup.h"

namespace Metal {

/// Raised by every 3D register UpdatePipelineState may read. The registers below are the
/// ones titles rewrite between most draws and that it never reads; anything not listed
/// counts, so a register missed here only costs a rebuilt state, never a stale one.
constexpr u8 PIPELINE_STATE_DIRTY = VideoCommon::Dirty::LastCommonEntry;
constexpr u8 VIEWPORT_SCISSOR_DIRTY = VideoCommon::Dirty::LastCommonEntry + 1;
constexpr u8 XFB_SESSION_DIRTY = VideoCommon::Dirty::LastCommonEntry + 2;
using Tegra::Engines::Maxwell3D;
using VideoCommon::CacheType;

namespace {

/// Emits the texture handle of each element of `desc`, laid out the way the
/// shader recompiler describes it: `stride` bytes apart, optionally offset by an
/// index read from a constant buffer, composed with a secondary handle and
/// constant bits. A descriptor with `sources` names one handle per alternative.
/// `bind(bank, count)` returns the bound constant buffer, or nothing when the
/// bank is not enabled.
template <typename Desc, typename Bind, typename Sink>
void ForEachTextureHandle(const Desc& desc, Bind&& bind, bool via_header_index, Sink&& sink) {
    using Tegra::Texture::TexturePair;
    const auto emit = [&](const auto& d) {
        const auto primary = bind(d.cbuf_index, d.count);
        if (!primary) {
            for (u32 index = 0; index < d.count; ++index) {
                sink(TexturePair(0u, via_header_index));
            }
            return;
        }
        u32 uniform_offset = 0;
        bool checked = false;
        bool invalid = false;
        if constexpr (requires { d.has_index_cbuf; }) {
            if (d.has_index_cbuf) {
                checked = true;
                const auto index_buffer = bind(d.index_cbuf_index, 1);
                invalid = !index_buffer || index_buffer->size < sizeof(u32) ||
                          d.index_cbuf_offset > index_buffer->size - sizeof(u32);
                if (!invalid) {
                    uniform_offset = index_buffer->Read(d.index_cbuf_offset) << d.index_shift;
                }
            }
        }
        u32 secondary = 0;
        if constexpr (requires { d.has_secondary; }) {
            if (d.has_secondary) {
                if (const auto buffer = bind(d.secondary_cbuf_index, 1)) {
                    secondary = buffer->Read(d.secondary_cbuf_offset);
                }
            }
        }
        for (u32 index = 0; index < d.count; ++index) {
            const u32 offset = d.cbuf_offset + index * d.stride + uniform_offset;
            if (checked && (invalid || primary->size < sizeof(u32) ||
                            offset > primary->size - sizeof(u32))) {
                sink(TexturePair(0u, via_header_index));
                continue;
            }
            sink(TexturePair(
                Tegra::Texture::ComposeTextureHandle(d, primary->Read(offset), secondary),
                via_header_index));
        }
    };
    if constexpr (requires { desc.sources; }) {
        if (!desc.sources.empty()) {
            for (const auto& source : desc.sources) {
                emit(source);
            }
            return;
        }
    }
    emit(desc);
}
// Whether captured transform feedback is written back to guest RAM. Not dead
// code: Legends Z-A's character creator captures a mesh, fences and reads the
// vertices back on the CPU, so having this off is a known correctness
// compromise. Off anyway - the guest only sees the write if the drain waits for
// the capture first, and that wait costs 1758 flushes a second, GPU occupancy
// 88% -> 42%, Legends Z-A 21 -> 17 fps. Waiting on the capture's own tick
// instead of Finish()ing changes nothing, it is still in the unsubmitted chunk.
// Re-enable once captures can be published without a pipeline stall.
constexpr bool XFB_WRITEBACK_ENABLED = false;

/// Maxwell stores raw float/int/uint bits in regs.clear_color depending on the
/// target format. Every 32-bit int/uint fits losslessly in IEEE-754 double, which
/// Metal converts back to integer for integer attachments.
[[nodiscard]] std::array<double, 4> ConvertClearColor(
    const Tegra::Engines::Maxwell3D::Regs& regs, Tegra::RenderTargetFormat rt_format) {
    using VideoCore::Surface::IsPixelFormatInteger;
    using VideoCore::Surface::IsPixelFormatSignedInteger;
    using VideoCore::Surface::PixelFormatFromRenderTargetFormat;

    const auto format = PixelFormatFromRenderTargetFormat(rt_format);
    std::array<double, 4> out{};
    if (!IsPixelFormatInteger(format)) {
        for (size_t i = 0; i < 4; ++i) {
            out[i] = static_cast<double>(regs.clear_color[i]);
        }
        return out;
    }
    if (IsPixelFormatSignedInteger(format)) {
        for (size_t i = 0; i < 4; ++i) {
            out[i] = static_cast<double>(std::bit_cast<s32>(regs.clear_color[i]));
        }
    } else {
        for (size_t i = 0; i < 4; ++i) {
            out[i] = static_cast<double>(std::bit_cast<u32>(regs.clear_color[i]));
        }
    }
    return out;
}

/// A *storage* texel buffer decodes through the format the shader declared, not
/// the view's - guests routinely declare an image as R32_UINT over bytes
/// written as something else. Sampled texel buffers keep the view's.
[[nodiscard]] VideoCore::Surface::PixelFormat TexelBufferFormat(
    VideoCore::Surface::PixelFormat view_format, bool is_image, Shader::ImageFormat declared) {
    if (!is_image) {
        return view_format;
    }
    using VideoCore::Surface::PixelFormat;
    switch (declared) {
    case Shader::ImageFormat::Typeless:
        return view_format;
    case Shader::ImageFormat::R8_UINT:
        return PixelFormat::R8_UINT;
    case Shader::ImageFormat::R8_SINT:
        return PixelFormat::R8_SINT;
    case Shader::ImageFormat::R16_UINT:
        return PixelFormat::R16_UINT;
    case Shader::ImageFormat::R16_SINT:
        return PixelFormat::R16_SINT;
    case Shader::ImageFormat::R32_UINT:
        return PixelFormat::R32_UINT;
    case Shader::ImageFormat::R32G32_UINT:
        return PixelFormat::R32G32_UINT;
    case Shader::ImageFormat::R32G32B32A32_UINT:
        return PixelFormat::R32G32B32A32_UINT;
    }
    return view_format;
}

/// The sampler variant that is legal against this texture. Metal defines
/// neither linear filtering of an integer texture nor a comparison sampler on a
/// colour one, and it does not reject them either.
[[nodiscard]] MTL::SamplerState* SamplerFor(const Sampler& sampler, const ImageView& view) {
    if (view.IsIntegerFormat()) [[unlikely]] {
        if (sampler.HasLinearFiltering()) {
            return sampler.HandleWithNearestFilter();
        }
        if (sampler.HasDepthComparison() &&
            VideoCore::Surface::GetFormatType(view.format) ==
                VideoCore::Surface::SurfaceType::ColorTexture) {
            return sampler.HandleWithoutDepthComparison();
        }
    }
    if (sampler.HasAddedAnisotropy() && !view.SupportsAnisotropy()) {
        return sampler.HandleWithDefaultAnisotropy();
    }
    return sampler.Handle();
}

static_assert(Shader::Backend::SPIRV::NUM_TEXTURE_AND_IMAGE_SCALING_WORDS == 6);

/// Packs per-descriptor resolution-scaling bits in the exact order SPIR-V
/// expects in RescalingLayout (matches Vulkan::RescalingPushConstant).
class RescalingMaskBuilder {
public:
    void PushTexture(bool is_rescaled) noexcept {
        if (texture_word < Shader::Backend::SPIRV::NUM_TEXTURE_SCALING_WORDS) {
            if (is_rescaled) {
                words[texture_word] |= texture_bit;
            }
            texture_bit <<= 1u;
            if (texture_bit == 0u) {
                texture_bit = 1u;
                ++texture_word;
            }
        }
    }

    void PushImage(bool is_rescaled) noexcept {
        if (image_word < Shader::Backend::SPIRV::NUM_IMAGE_SCALING_WORDS) {
            if (is_rescaled) {
                words[Shader::Backend::SPIRV::NUM_TEXTURE_SCALING_WORDS + image_word] |= image_bit;
            }
            image_bit <<= 1u;
            if (image_bit == 0u) {
                image_bit = 1u;
                ++image_word;
            }
        }
    }

    [[nodiscard]] const std::array<u32, 6>& Data() const noexcept {
        return words;
    }

private:
    std::array<u32, 6> words{};
    u32 texture_word{0};
    u32 image_word{0};
    u32 texture_bit{1u};
    u32 image_bit{1u};
};

constexpr size_t NUM_VIEWPORTS = Tegra::Engines::Maxwell3D::Regs::NumViewports;

/// Vertex buffer slot reserved for constant attributes. Metal has only 31
/// slots, so NumVertexArrays (32) cannot be a slot index, it trips an assertion
/// inside setBufferIndex.
constexpr u32 CONSTANT_ATTRIBUTE_BINDING =
    BindingMap::MAX_VERTEX_BUFFER_SLOT - BindingMap::VERTEX_BUFFER_BASE;

struct DrawParams {
    u32 base_instance{};
    u32 num_instances{};
    u32 base_vertex{};
    u32 num_vertices{};
    u32 first_index{};
    bool is_indexed{};
};

DrawParams MakeDrawParams(const Tegra::Engines::DrawManager::State& draw_state,
                          u32 num_instances, bool is_indexed) {
    DrawParams params{
        .base_instance = draw_state.base_instance,
        .num_instances = num_instances,
        .base_vertex = is_indexed ? draw_state.base_index : draw_state.vertex_buffer.first,
        .num_vertices = is_indexed ? draw_state.index_buffer.count : draw_state.vertex_buffer.count,
        .first_index = is_indexed ? draw_state.index_buffer.first : 0,
        .is_indexed = is_indexed,
    };

    // An expanded topology becomes indexed, and `first` is folded into the
    // generated indices, which is why the base vertex drops to zero.
    if (!is_indexed && NeedsIndexConversion(draw_state.topology)) {
        params.num_vertices = ConvertedIndexCount(draw_state.topology, params.num_vertices);
        params.base_vertex = 0;
        params.first_index = 0;
        params.is_indexed = true;
    }
    return params;
}

/// Kept out of the pipeline key: a depth function change is not a recompile.
DepthStencilState MakeDepthStencilState(const Tegra::Engines::Maxwell3D::Regs& regs,
                                        bool has_depth, bool has_stencil) {
    DepthStencilState state{};
    // Gated on the attachment existing, since Metal (unlike Vulkan) tests a
    // stencil-enabled state against undefined contents when there is no stencil
    // attachment and kills every fragment. And gated on the *test*, not the
    // write flag: Metal has no test-enable, so Always+write really writes and
    // every test-off/write-on fullscreen quad becomes a depth eraser.
    state.depth_write =
        has_depth && regs.depth_test_enable != 0 && regs.depth_write_enabled != 0;
    state.depth_compare = has_depth && regs.depth_test_enable != 0
                              ? ComparisonOp(regs.depth_test_func)
                              : MTL::CompareFunctionAlways;

    state.stencil_enable = has_stencil && regs.stencil_enable != 0;
    if (state.stencil_enable) {
        state.front.compare = ComparisonOp(regs.stencil_front_op.func);
        state.front.fail = StencilOp(regs.stencil_front_op.fail);
        state.front.depth_fail = StencilOp(regs.stencil_front_op.zfail);
        state.front.pass = StencilOp(regs.stencil_front_op.zpass);
        state.front.read_mask = regs.stencil_front_func_mask;
        state.front.write_mask = regs.stencil_front_mask;

        if (regs.stencil_two_side_enable != 0) {
            state.back.compare = ComparisonOp(regs.stencil_back_op.func);
            state.back.fail = StencilOp(regs.stencil_back_op.fail);
            state.back.depth_fail = StencilOp(regs.stencil_back_op.zfail);
            state.back.pass = StencilOp(regs.stencil_back_op.zpass);
            state.back.read_mask = regs.stencil_back_func_mask;
            state.back.write_mask = regs.stencil_back_mask;
        } else {
            state.back = state.front;
        }
    }
    return state;
}

MTL::PrimitiveTopologyClass TopologyClass(
    Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology) {
    using Topology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
    switch (topology) {
    case Topology::Points:
        return MTL::PrimitiveTopologyClassPoint;
    case Topology::Lines:
    case Topology::LineLoop:
    case Topology::LineStrip:
        return MTL::PrimitiveTopologyClassLine;
    default:
        return MTL::PrimitiveTopologyClassTriangle;
    }
}

} // Anonymous namespace

// ---------------------------------------------------------------------------
// AccelerateDMA
// ---------------------------------------------------------------------------

AccelerateDMA::AccelerateDMA(BufferCache& buffer_cache_, TextureCache& texture_cache_,
                             Scheduler& scheduler_)
    : buffer_cache{buffer_cache_}, texture_cache{texture_cache_}, scheduler{scheduler_} {}

bool AccelerateDMA::BufferCopy(GPUVAddr start_address, GPUVAddr end_address, u64 amount) {
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.DeferBoundaryFlush(true);
    SCOPE_EXIT {
        scheduler.DeferBoundaryFlush(false);
    };
    {
        std::scoped_lock lock{buffer_cache.mutex};
        if (!buffer_cache.DMACopy(start_address, end_address, amount)) {
            return false;
        }
    }
    std::scoped_lock lock{texture_cache.mutex};
    texture_cache.NotifyUnsafeWrite(end_address, amount);
    return true;
}

bool AccelerateDMA::BufferClear(GPUVAddr src_address, u64 amount, u32 value) {
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.DeferBoundaryFlush(true);
    SCOPE_EXIT {
        scheduler.DeferBoundaryFlush(false);
    };
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.NotifyUnsafeWrite(src_address, amount * sizeof(u32));
    }
    std::scoped_lock lock{buffer_cache.mutex};
    return buffer_cache.DMAClear(src_address, amount, value);
}

template <bool IS_IMAGE_UPLOAD>
bool AccelerateDMA::DmaBufferImageCopy(const Tegra::DMA::ImageCopy& copy_info,
                                       const Tegra::DMA::BufferOperand& buffer_operand,
                                       const Tegra::DMA::ImageOperand& image_operand) {
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.DeferBoundaryFlush(true);
    SCOPE_EXIT {
        scheduler.DeferBoundaryFlush(false);
    };
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    const VideoCommon::ImageId image_id = texture_cache.DmaImageId(image_operand, IS_IMAGE_UPLOAD);
    if (image_id == VideoCommon::NULL_IMAGE_ID) {
        return false;
    }
    const u32 buffer_size = static_cast<u32>(buffer_operand.pitch * buffer_operand.height);
    constexpr auto sync_info = VideoCommon::ObtainBufferSynchronize::FullSynchronize;
    const auto post_op = IS_IMAGE_UPLOAD ? VideoCommon::ObtainBufferOperation::DoNothing
                                         : VideoCommon::ObtainBufferOperation::MarkAsWritten;
    const auto [buffer, offset] =
        buffer_cache.ObtainBuffer(buffer_operand.address, buffer_size, sync_info, post_op);
    if (buffer == nullptr) {
        return false;
    }
    buffer->MarkUsage(offset, buffer_size);
    buffer->MarkGpuCopy();

    const auto [image, copy] = texture_cache.DmaBufferImageCopy(
        copy_info, buffer_operand, image_operand, image_id, IS_IMAGE_UPLOAD);
    const std::span copy_span{&copy, 1};

    if constexpr (IS_IMAGE_UPLOAD) {
        texture_cache.PrepareImage(image_id, true, false);
        image->UploadMemory(buffer->Handle(), offset, copy_span);
    } else {
        if (offset % VideoCore::Surface::BytesPerBlock(image->info.format) != 0) {
            return false;
        }
        texture_cache.DownloadImageIntoBuffer(image, buffer->Handle(), offset, copy_span,
                                              buffer_operand.address, buffer_size);
    }
    return true;
}

bool AccelerateDMA::ImageToBuffer(const Tegra::DMA::ImageCopy& copy_info,
                                  const Tegra::DMA::ImageOperand& image_operand,
                                  const Tegra::DMA::BufferOperand& buffer_operand) {
    return DmaBufferImageCopy<false>(copy_info, buffer_operand, image_operand);
}

bool AccelerateDMA::BufferToImage(const Tegra::DMA::ImageCopy& copy_info,
                                  const Tegra::DMA::BufferOperand& buffer_operand,
                                  const Tegra::DMA::ImageOperand& image_operand) {
    return DmaBufferImageCopy<true>(copy_info, buffer_operand, image_operand);
}

// ---------------------------------------------------------------------------
// RasterizerMetal
// ---------------------------------------------------------------------------

RasterizerMetal::RasterizerMetal(Tegra::GPU& gpu, Tegra::MaxwellDeviceMemoryManager& device_memory,
                                 const Device& device, MemoryAllocator& memory_allocator,
                                 StagingBufferPool& staging_pool, Scheduler& scheduler)
    : m_gpu{gpu}, m_device_memory{device_memory}, m_device{device},
      m_memory_allocator{memory_allocator}, m_scheduler{scheduler},
      m_staging_pool{staging_pool}, m_blit_image{device, scheduler},
      m_texture_cache_runtime{device, scheduler, memory_allocator, staging_pool, m_blit_image},
      m_texture_cache{m_texture_cache_runtime, device_memory},
      m_buffer_cache_runtime{device, memory_allocator, scheduler, staging_pool},
      m_buffer_cache{device_memory, m_buffer_cache_runtime},
      m_pipeline_cache{device_memory, device, scheduler, gpu.ShaderNotify()},
      m_argument_buffer{device, scheduler, staging_pool},
      m_transform_feedback{device, scheduler, memory_allocator},
      m_fence_manager{*this,        gpu,           m_texture_cache,
                      m_buffer_cache, m_query_cache, scheduler},
      m_accelerate_dma{m_buffer_cache, m_texture_cache, scheduler} {
    m_query_cache.BindOcclusionState(&m_scheduler, &m_last_occlusion_tick,
                                     &m_pending_occlusion_count, &m_occlusion_needs_fence);
}

RasterizerMetal::~RasterizerMetal() = default;

bool RasterizerMetal::UpdateRenderTargets(bool is_clear, const ClearAspects& aspects) {
    if (maxwell3d == nullptr) {
        return false;
    }

    std::scoped_lock lock{m_texture_cache.mutex};
    if (maxwell3d->dirty.flags[VideoCommon::Dirty::RenderTargets]) {
        ++m_rt_dirty_generation;
    }
    m_texture_cache.SynchronizeGraphicsDescriptors();
    const bool unconditional_clear = is_clear && aspects.indirect_buffer == nullptr;
    m_texture_cache.UpdateRenderTargets(unconditional_clear,
                                        unconditional_clear ? aspects.layer_count : 0);

    Framebuffer* const framebuffer = m_texture_cache.GetFramebuffer();
    m_current_framebuffer = framebuffer;
    if (framebuffer == nullptr) {
        return false;
    }

    m_color_alias_mask = framebuffer->ColorAliasMask();
    m_color_alias_groups = framebuffer->ColorAliasGroups();
    if (m_alias_framebuffer != framebuffer) {
        m_alias_framebuffer = framebuffer;
        m_alias_scratch = m_color_alias_mask;
    }
    if (!is_clear) {
        // Not opened here on purpose. One encoder is open per command buffer, so an
        // upload recorded after this closes the render encoder and the draw does
        // nothing. The pass is remembered and reopened right before the draw.
        m_render_pass = framebuffer->GetRenderPassState();
        m_has_render_pass = true;
        return true;
    }
    RenderPassState pass = framebuffer->GetRenderPassState();

    if (is_clear) {
        // A clear is free on tile-based hardware when expressed as the pass's
        // load action, and expensive as a draw. Only the attachments the guest
        // named, though - clearing all of them wipes a target it meant to keep.
        const auto& regs = maxwell3d->regs;

        // A load action has no channel mask, no stencil write mask and no
        // rectangle. Decided BEFORE any, since the drawn path reuses `pass`.
        const bool partial_channels = aspects.color && aspects.color_mask != 0xfu;
        const bool partial_stencil = aspects.stencil && aspects.stencil_mask != 0xffu;

        const f32 clear_scale = m_texture_cache.IsRescaling()
                                    ? videoSettings.resolution_info.up_factor
                                    : 1.0f;
        const bool clear_lower_left =
            regs.window_origin.mode != Tegra::Engines::Maxwell3D::Regs::WindowOrigin::Mode::UpperLeft;
        const s64 area_w = static_cast<s64>(pass.render_area_width);
        const s64 area_h = static_cast<s64>(pass.render_area_height);
        s64 left = 0;
        s64 top = 0;
        s64 right = area_w;
        s64 bottom = area_h;
        if (regs.clear_control.use_scissor != 0 && regs.scissor_test[0].enable != 0) {
            const auto& sc = regs.scissor_test[0];
            s64 min_y = static_cast<s64>(sc.min_y);
            s64 max_y = static_cast<s64>(sc.max_y);
            if (clear_lower_left) {
                const s64 clip_h = static_cast<s64>(regs.surface_clip.height);
                const s64 flipped_min = clip_h - max_y;
                max_y = clip_h - min_y;
                min_y = flipped_min;
            }
            left = std::clamp<s64>(static_cast<s64>(sc.min_x * clear_scale), 0, area_w);
            top = std::clamp<s64>(static_cast<s64>(min_y * clear_scale), 0, area_h);
            right = std::clamp<s64>(static_cast<s64>(sc.max_x * clear_scale), left, area_w);
            bottom = std::clamp<s64>(static_cast<s64>(max_y * clear_scale), top, area_h);
        }
        const bool partial_area =
            left != 0 || top != 0 || right != area_w || bottom != area_h;
        const bool needs_draw = partial_area || partial_channels || partial_stencil ||
                                aspects.indirect_buffer != nullptr;

        if (!needs_draw) {
            for (u32 index = 0; index < pass.num_color; ++index) {
                if (pass.color[index].texture == nullptr) {
                    continue;
                }
                if (!aspects.color || index != aspects.color_target) {
                    continue;
                }
                pass.color[index].load_action = LoadAction::Clear;
                pass.color[index].clear_color =
                    ConvertClearColor(regs, regs.rt[index].format);
            }
            if (pass.depth_stencil.texture != nullptr) {
                if (aspects.depth) {
                    pass.depth_stencil.depth_load = LoadAction::Clear;
                    pass.depth_stencil.clear_depth = regs.clear_depth;
                }
                if (aspects.stencil && pass.depth_stencil.has_stencil) {
                    pass.depth_stencil.stencil_load = LoadAction::Clear;
                    pass.depth_stencil.clear_stencil = regs.clear_stencil;
                }
            }
        }

        // A load action otherwise covers every slice of the pass, and Metal
        // will not trim an over-long range.
        const u32 attachment_layers = (std::max)(1u, pass.num_layers);
        const u32 first_layer = (std::min)(aspects.layer, attachment_layers - 1);
        const u32 layer_count =
            (std::min)((std::max)(1u, aspects.layer_count), attachment_layers - first_layer);
        if (aspects.color && aspects.color_target < pass.color.size()) {
            pass.color[aspects.color_target].slice += first_layer;
            if (pass.color[aspects.color_target].resolve_texture != nullptr) {
                pass.color[aspects.color_target].resolve_slice += first_layer;
            }
        }
        if (aspects.depth || aspects.stencil) {
            pass.depth_stencil.slice += first_layer;
        }
        pass.num_layers = layer_count;

        if (!needs_draw && first_layer > 0 && layer_count > 1) {
            // Metal rejects non-zero attachment slice together with renderTargetArrayLength > 1.
            // Clear each requested slice individually when clearing a subrange starting past 0.
            RenderPassState slice_pass = pass;
            slice_pass.num_layers = 1;
            const u32 base_color_slice =
                (aspects.color && aspects.color_target < pass.color.size())
                    ? pass.color[aspects.color_target].slice
                    : 0;
            const u32 base_depth_slice = pass.depth_stencil.slice;
            ScratchUnwrittenAliases(
                slice_pass, aspects.color ? (1u << aspects.color_target) : 0u);
            for (u32 layer = 0; layer < layer_count; ++layer) {
                if (aspects.color && aspects.color_target < slice_pass.color.size()) {
                    slice_pass.color[aspects.color_target].slice = base_color_slice + layer;
                }
                if (aspects.depth || aspects.stencil) {
                    slice_pass.depth_stencil.slice = base_depth_slice + layer;
                }
                BeginPassVisibility(slice_pass);
                m_scheduler.RequestRenderPass(slice_pass);
            }
            m_render_pass = slice_pass;
            m_has_render_pass = true;
            return true;
        }

        ScratchUnwrittenAliases(
            pass, aspects.color ? (1u << aspects.color_target) : 0u);

        if (needs_draw) {
            // Metal has no clear-rect, so a partial clear is a scissored draw.
            std::array<u32, 4> raw_clear_color{};
            std::memcpy(raw_clear_color.data(), regs.clear_color.data(), sizeof(raw_clear_color));
            const bool clear_depth = aspects.depth && pass.depth_stencil.texture != nullptr;
            const bool clear_stencil = aspects.stencil && pass.depth_stencil.has_stencil;
            if (layer_count > 1) {
                // clear_vertex draws into layer 0 only (and Metal rejects slice > 0 with
                // renderTargetArrayLength > 1), so clear each slice individually.
                RenderPassState slice_pass = pass;
                slice_pass.num_layers = 1;
                const u32 base_color_slice =
                    (aspects.color && aspects.color_target < pass.color.size())
                        ? pass.color[aspects.color_target].slice
                        : 0;
                const u32 base_depth_slice = pass.depth_stencil.slice;
                for (u32 layer = 0; layer < layer_count; ++layer) {
                    if (aspects.color && aspects.color_target < slice_pass.color.size()) {
                        slice_pass.color[aspects.color_target].slice = base_color_slice + layer;
                    }
                    if (aspects.depth || aspects.stencil) {
                        slice_pass.depth_stencil.slice = base_depth_slice + layer;
                    }
                    BeginPassVisibility(slice_pass);
                    m_scheduler.RequestRenderPass(slice_pass);
                    m_blit_image.ClearAttachments(
                        slice_pass, aspects.color_target, aspects.color, clear_depth,
                        clear_stencil, raw_clear_color, regs.clear_depth, regs.clear_stencil,
                        static_cast<s32>(left), static_cast<s32>(top), static_cast<s32>(right),
                        static_cast<s32>(bottom), aspects.color_mask, aspects.stencil_mask,
                        aspects.indirect_buffer, aspects.indirect_offset);
                }
                m_render_pass = slice_pass;
                m_has_render_pass = true;
                return true;
            }
            BeginPassVisibility(pass);
            m_render_pass = pass;
            m_has_render_pass = true;
            m_scheduler.RequestRenderPass(pass);
            m_blit_image.ClearAttachments(
                pass, aspects.color_target, aspects.color, clear_depth, clear_stencil,
                raw_clear_color, regs.clear_depth, regs.clear_stencil, static_cast<s32>(left),
                static_cast<s32>(top), static_cast<s32>(right), static_cast<s32>(bottom),
                aspects.color_mask, aspects.stencil_mask, aspects.indirect_buffer,
                aspects.indirect_offset);
            return true;
        }
    }

    // Not opened here on purpose. One encoder is open per command buffer, so an
    // upload recorded after this closes the render encoder and the draw does
    // nothing. The pass is remembered and reopened right before the draw.
    BeginPassVisibility(pass);
    m_render_pass = pass;
    m_has_render_pass = true;

    if (is_clear) {
        m_scheduler.RequestRenderPass(pass);
    }
    return true;
}

u32 RasterizerMetal::WrittenColorAttachments(const GraphicsPipeline& pipeline) const {
    return pipeline.WrittenColorAttachments();
}

u32 RasterizerMetal::AliasScratch(u32 num_color, u32 written) const {
    // Two slots on one image each get their own tile memory, and which of them
    // a tile stores last differs from tile to tile: the image ends up a
    // checkerboard of both. Only a slot this draw writes keeps the real image,
    // the others render into memoryless scratch that is never stored.
    // A group this draw writes keeps only its writers. A group it leaves alone
    // keeps the previous draw's choice, so the open pass stays compatible.
    const u32 aliased = m_color_alias_mask & ((1u << num_color) - 1u);
    u32 scratch = 0;
    for (u32 bits = aliased; bits != 0; bits &= bits - 1) {
        const u32 group = m_color_alias_groups[std::countr_zero(bits)];
        u32 writers = group & written;
        // Several slots of one image each written: only one may store it, or every tile
        // ends up holding whichever slot stored last.
        if (std::popcount(writers) > 1) {
            writers = writers & (0u - writers);
        }
        scratch |= writers != 0 ? group & ~writers : group & m_alias_scratch;
    }
    return scratch;
}

void RasterizerMetal::ScratchUnwrittenAliases(RenderPassState& pass, u32 written) {
    const u32 aliased = m_color_alias_mask & ((1u << pass.num_color) - 1u);
    if (aliased == 0) {
        return;
    }
    u32 scratch = AliasScratch(pass.num_color, written);
    m_alias_scratch = scratch;
    if (std::popcount(aliased & written) > 1) {
        static std::atomic<u32> reported{0};
        if (reported++ < 4) {
            LOG_WARNING(Render_Metal, "Draw writes {} aliased colour attachments (mask {:#x})",
                        std::popcount(aliased & written), aliased & written);
        }
    }
    // Layered passes need array scratch targets, not handled yet.
    if (pass.num_layers > 1) {
        return;
    }
    for (; scratch != 0; scratch &= scratch - 1) {
        const u32 index = static_cast<u32>(std::countr_zero(scratch));
        ColorAttachment& attachment = pass.color[index];
        if (attachment.texture == nullptr || attachment.resolve_texture != nullptr) {
            continue;
        }
        MTL::Texture* const target = m_texture_cache_runtime.GetScratchAttachment(
            attachment.texture->pixelFormat(), pass.render_area_width, pass.render_area_height,
            pass.sample_count);
        if (target == nullptr) {
            continue;
        }
        attachment.texture = target;
        attachment.level = 0;
        attachment.slice = 0;
        attachment.load_action = LoadAction::DontCare;
        attachment.store_action = StoreAction::DontCare;
    }
}

void RasterizerMetal::CommitPendingReadbacks() {
    if (!m_texture_cache.has_pending_readback_commit.load(std::memory_order_relaxed)) {
        return;
    }
    if (m_texture_cache.has_pending_readback_commit.exchange(false, std::memory_order_relaxed)) {
        m_fence_manager.SignalReference();
    }
}

void RasterizerMetal::Draw(bool is_indexed, u32 instance_count) {
    DrawImpl(is_indexed, instance_count, nullptr);
}

/// Metal has no multi-draw, so the count becomes a loop over the stride.
bool RasterizerMetal::DrawIndirect() {
    if (!maxwell3d) {
        return true;
    }
    const auto& params = maxwell3d->draw_manager->GetIndirectParams();
    if (maxwell3d->regs.transform_feedback_enabled != 0 && Metal::Options::TransformFeedback()) {
        // Transform feedback capture runs as compute and needs concrete vertex counts on the CPU.
        return false;
    }
    if (gpu_memory) {
        gpu_memory->FlushCaching();
    }
    if (UpdateRenderTargets(/*is_clear=*/false, ClearAspects{})) {
        UpdatePipelineState();
        const GraphicsPipeline* const pipeline = m_pipeline_cache.CurrentGraphicsPipeline();
        if (pipeline != nullptr && pipeline->VtgParams() != nullptr) {
            const bool gpu_arguments =
                pipeline->CanDrawVtgIndirect() && !params.is_byte_count && !params.is_indexed &&
                !params.include_count && params.max_draw_counts == 1 &&
                m_buffer_cache_runtime.GetIndexPass().HasVtgIndirect();
            if (!gpu_arguments) {
                // Fall back to macro HLE CPU readback so indexed/multi/first-time/byte-count
                // indirect draws run through normal Draw() instead of being dropped.
                return false;
            }
        }
    }
    // buffer/offset stay null here: GetDrawIndirectBuffer reads channel state
    // only the buffer cache update inside DrawImpl fills in.
    IndirectDraw indirect{
        .buffer = nullptr,
        .offset = 0,
        .stride = static_cast<u32>(params.stride),
        .count = static_cast<u32>(params.max_draw_counts),
        .instance_mask = params.instance_mask,
    };
    // The real count lives in a second buffer the GPU writes this frame, so it
    // must NOT be read on the CPU (that gives last frame's value). DrawImpl
    // resolves it on the GPU via IndexPass::PatchIndirectCount.
    indirect.include_count = params.include_count;
    indirect.is_byte_count = params.is_byte_count;
    if (indirect.count == 0) {
        return true;
    }

    m_buffer_cache.SetDrawIndirect(&params);
    DrawImpl(params.is_indexed, 1, &indirect);
    m_buffer_cache.SetDrawIndirect(nullptr);
    return true;
}

#define DROP_DRAW(why)                                                                     \
    do {                                                                                       \
        static std::atomic<u64> drop_count{0};                                                 \
        const u64 drawdiag_c = drop_count.fetch_add(1, std::memory_order_relaxed) + 1;         \
        if (std::has_single_bit(drawdiag_c)) {                                                 \
            LOG_WARNING(Render_Metal, "Dropped {} draw(s): {}", drawdiag_c, why);     \
        }                                                                                      \
    } while (0)

namespace {
constexpr u32 PACK_CULL_SHIFT = 0;         // 2 bits
constexpr u32 PACK_WINDING_SHIFT = 2;      // 1 bit
constexpr u32 PACK_DEPTH_CLIP_SHIFT = 3;   // 1 bit
constexpr u32 PACK_FILL_SHIFT = 4;         // 1 bit
constexpr u32 PACK_PRIMITIVE_SHIFT = 5;    // 3 bits
constexpr u32 PACK_IS_INDEXED_BIT = 1u << 8;
constexpr u32 PACK_INDEX_U32_BIT = 1u << 9;
constexpr u32 PACK_VERTEX_GPU_TEX_BIT = 1u << 10;
constexpr u32 PACK_NEED_FPUSH_BIT = 1u << 11;
constexpr u32 PACK_NEED_VPUSH_BIT = 1u << 12;
constexpr u32 PACK_VERTEX_STORAGE_FRAME_BIT = 1u << 13;

[[nodiscard]] RenderEncoder ApplyDrawRenderState(
    CommandContext& context, GraphicsPipeline* pipeline, MTL::DepthStencilState* depth_stencil,
    u32 packed_flags, u32 visibility_slot, u64 vertex_arg_addr, u64 fragment_arg_addr,
    u64 vertex_table_arg_addr, u64 fragment_table_arg_addr,
    u32 stream_mask, std::span<const u64> stream_addrs,
    MTL::Texture* d24_equal_depth_tex = nullptr) {
    constexpr u32 NO_VIS_SLOT = ~0u;
    RenderEncoder encoder = context.CurrentRender();
    if (encoder == nullptr) {
        // Anything needing a blit or compute encoder between the pass
        // request and this draw closes it, and the draw vanishes silently.
        static std::atomic<u32> skipped{0};
        const u32 count = ++skipped;
        if (std::has_single_bit(count)) {
            LOG_WARNING(Render_Metal, "Skipped {} draw(s): render encoder closed underneath",
                        count);
        }
        return {};
    }

    pipeline->WaitBuilt();
    MTL::RenderPipelineState* const pipeline_state = pipeline->Handle();
    if (pipeline_state == nullptr) {
        static std::atomic<u32> skipped{0};
        const u32 count = ++skipped;
        if (std::has_single_bit(count)) {
            LOG_WARNING(Render_Metal, "Skipped {} draw(s): pipeline failed to build", count);
        }
        return {};
    }

    // Only what changed since the last draw on this encoder. Re-setting a
    // pipeline or depth-stencil object is real driver work.
    auto& cache = context.RenderCache();
    const bool vertex_reads_gpu_texture = (packed_flags & PACK_VERTEX_GPU_TEX_BIT) != 0;
    const bool needs_queue_barrier = context.NeedsVertexBarrierFor(
        vertex_reads_gpu_texture, pipeline->VertexReadsBuffers() || stream_mask != 0);
    if (needs_queue_barrier) {
        encoder->barrierAfterQueueStages(
            static_cast<MTL::Stages>(MTL::StageVertex | MTL::StageFragment), MTL::StageVertex,
            MTL4::VisibilityOptionDevice);
        encoder->barrierAfterEncoderStages(
            MTL::StageVertex, static_cast<MTL::Stages>(MTL::StageVertex | MTL::StageFragment),
            MTL4::VisibilityOptionDevice);
        context.MarkVertexBarrierDone();
    } else if (cache.has_drawn && (packed_flags & PACK_VERTEX_STORAGE_FRAME_BIT) != 0) {
        // Metal 4 does not implicitly order vertex-stage visibility between consecutive draws
        // in the same render encoder; only pay this barrier on frames that perform vertex-stage
        // storage writes (e.g. XC3 SSBO skinning) so non-skinning titles like TOTK pay zero cost.
        encoder->barrierAfterEncoderStages(MTL::StageVertex, MTL::StageVertex,
                                           MTL4::VisibilityOptionDevice);
    }
    cache.has_drawn = true;
    if (pipeline->HasStorageWrites()) {
        context.NoteRenderStorageWrite();
    }
    if (cache.visibility_slot != visibility_slot) {
        // Counting is only legal when the pass this encoder was opened with
        // named a visibility buffer. Asking anyway does not fail cleanly -
        // the process dies inside setVisibilityResultMode.
        const bool can_count = visibility_slot != NO_VIS_SLOT &&
                               context.CurrentPassVisibilityBuffer() != nullptr;
        if (visibility_slot != NO_VIS_SLOT && !can_count) {
            static std::atomic<u32> mismatched{0};
            const u32 count = mismatched.fetch_add(1, std::memory_order_relaxed) + 1;
            if (std::has_single_bit(count)) {
                LOG_WARNING(Render_Metal,
                            "Dropped {} occlusion quer(y/ies): the open pass has no "
                            "visibility buffer",
                            count);
            }
        }
        cache.visibility_slot = can_count ? visibility_slot : NO_VIS_SLOT;
        if (!can_count) {
            encoder->setVisibilityResultMode(MTL::VisibilityResultModeDisabled, 0);
        } else {
            encoder->setVisibilityResultMode(MTL::VisibilityResultModeCounting,
                                             visibility_slot * sizeof(u64));
        }
    }
    if (cache.pipeline != pipeline_state) {
        cache.pipeline = pipeline_state;
        encoder->setRenderPipelineState(pipeline_state);
    }
    if (depth_stencil != nullptr && cache.depth_stencil != depth_stencil) {
        cache.depth_stencil = depth_stencil;
        encoder->setDepthStencilState(depth_stencil);
    }
    if (const CommandContext::ViewportState& state = context.Viewports();
        cache.viewport_generation != state.generation) {
        cache.viewport_generation = state.generation;
        encoder->setViewports(state.viewports.data(), state.count);
        encoder->setScissorRects(state.scissors.data(), state.count);
    }
    const u32 cull_mode = (packed_flags >> PACK_CULL_SHIFT) & 3u;
    if (cache.cull_mode != cull_mode) {
        cache.cull_mode = cull_mode;
        encoder->setCullMode(static_cast<MTL::CullMode>(cull_mode));
    }
    const u32 winding = (packed_flags >> PACK_WINDING_SHIFT) & 1u;
    if (cache.winding != winding) {
        cache.winding = winding;
        encoder->setFrontFacingWinding(static_cast<MTL::Winding>(winding));
    }
    const u32 depth_clip = (packed_flags >> PACK_DEPTH_CLIP_SHIFT) & 1u;
    if (cache.depth_clip != depth_clip) {
        cache.depth_clip = depth_clip;
        encoder->setDepthClipMode(static_cast<MTL::DepthClipMode>(depth_clip));
    }
    const u32 fill_mode = (packed_flags >> PACK_FILL_SHIFT) & 1u;
    if (cache.fill_mode != fill_mode) {
        cache.fill_mode = fill_mode;
        encoder->setTriangleFillMode(static_cast<MTL::TriangleFillMode>(fill_mode));
    }
    if (const CommandContext::DynamicDrawState& dyn = context.DynamicState();
        cache.dynamic_generation != dyn.generation) {
        cache.dynamic_generation = dyn.generation;
        if (cache.stencil_front != dyn.stencil_front || cache.stencil_back != dyn.stencil_back) {
            cache.stencil_front = dyn.stencil_front;
            cache.stencil_back = dyn.stencil_back;
            encoder->setStencilReferenceValues(dyn.stencil_front, dyn.stencil_back);
        }
        if (!cache.depth_bias_valid || cache.depth_bias != dyn.depth_bias) {
            cache.depth_bias_valid = true;
            cache.depth_bias = dyn.depth_bias;
            encoder->setDepthBias(dyn.depth_bias[0], dyn.depth_bias[1], dyn.depth_bias[2]);
        }
        if (!cache.blend_color_valid || cache.blend_color != dyn.blend_color) {
            cache.blend_color_valid = true;
            cache.blend_color = dyn.blend_color;
            encoder->setBlendColor(dyn.blend_color[0], dyn.blend_color[1], dyn.blend_color[2],
                                   dyn.blend_color[3]);
        }
    }

    if (vertex_arg_addr != 0) {
        if (cache.vertex_addr[BindingMap::ARGUMENT_BUFFER_INDEX] != vertex_arg_addr) {
            cache.vertex_addr[BindingMap::ARGUMENT_BUFFER_INDEX] = vertex_arg_addr;
            encoder->setVertexAddress(vertex_arg_addr, BindingMap::ARGUMENT_BUFFER_INDEX);
        }
    }
    if (vertex_table_arg_addr != 0) {
        if (cache.vertex_addr[BindingMap::VERTEX_TABLE_ARGUMENT_BUFFER_INDEX] !=
            vertex_table_arg_addr) {
            cache.vertex_addr[BindingMap::VERTEX_TABLE_ARGUMENT_BUFFER_INDEX] =
                vertex_table_arg_addr;
            encoder->setVertexAddress(vertex_table_arg_addr,
                                      BindingMap::VERTEX_TABLE_ARGUMENT_BUFFER_INDEX);
        }
    }
    if (fragment_arg_addr != 0) {
        if (cache.fragment_addr != fragment_arg_addr) {
            cache.fragment_addr = fragment_arg_addr;
            encoder->setFragmentAddress(fragment_arg_addr, BindingMap::ARGUMENT_BUFFER_INDEX);
        }
    }
    if (fragment_table_arg_addr != 0) {
        if (cache.fragment_table_addr != fragment_table_arg_addr) {
            cache.fragment_table_addr = fragment_table_arg_addr;
            encoder->setFragmentAddress(fragment_table_arg_addr,
                                        BindingMap::FRAGMENT_TABLE_ARGUMENT_BUFFER_INDEX);
        }
    }
    if ((packed_flags & (PACK_NEED_FPUSH_BIT | PACK_NEED_VPUSH_BIT)) != 0) {
        const CommandContext::PushConstantState& push_state = context.PushConstants();
        if ((packed_flags & PACK_NEED_FPUSH_BIT) != 0 &&
            cache.fragment_push_generation != push_state.generation) {
            cache.fragment_push_generation = push_state.generation;
            const auto push = Shader::Backend::SPIRV::MakeGraphicsPushConstants(
                push_state.rescaling_masks, std::bit_cast<f32>(push_state.down_factor_bits),
                push_state.fragment_attachment_reads, push_state.render_area);
            encoder->setFragmentBytes(&push, sizeof(push),
                                      BindingMap::FRAGMENT_PUSH_CONSTANT_INDEX);
        }
        if ((packed_flags & PACK_NEED_VPUSH_BIT) != 0 &&
            cache.vertex_push_generation != push_state.generation) {
            cache.vertex_push_generation = push_state.generation;
            const auto push = Shader::Backend::SPIRV::MakeGraphicsPushConstants(
                push_state.rescaling_masks, std::bit_cast<f32>(push_state.down_factor_bits),
                Shader::Backend::SPIRV::NO_ATTACHMENT_READS, push_state.render_area);
            encoder->setVertexBytes(&push, sizeof(push), BindingMap::VERTEX_PUSH_CONSTANT_INDEX);
        }
    }

    if (d24_equal_depth_tex != nullptr) {
        encoder->setFragmentTexture(d24_equal_depth_tex, 0);
    }

    size_t addr_idx = 0;
    for (u32 bits = stream_mask; bits != 0; bits &= bits - 1) {
        // Do NOT zero the guest's streams for geometry emulated draws. It looks
        // right, the emulated stage should only read the emitted vertices, but it
        // brought the screen-wide smeared geometry straight back: it does read them.
        const u32 slot = BindingMap::VERTEX_BUFFER_BASE + static_cast<u32>(std::countr_zero(bits));
        const u64 addr = stream_addrs[addr_idx++];
        // Metal 4 has no offset-only rebind, the binding IS a GPU address.
        if (cache.vertex_addr[slot] != addr) {
            cache.vertex_addr[slot] = addr;
            encoder->setVertexAddress(addr, slot);
        }
    }
    return encoder;
}
} // Anonymous namespace

void RasterizerMetal::DrawImpl(bool is_indexed, u32 instance_count,
                               const IndirectDraw* indirect) {
    CommitPendingReadbacks();
    if (!maxwell3d) {
        return;
    }
    // Read per draw: it toggles around the objects a title tests for visibility.
    m_zpass_enabled = maxwell3d->regs.zpass_pixel_count_enable != 0;
    if (m_zpass_enabled && !m_occlusion_used) {
        m_occlusion_used = true;
    }

    FlushWork();
    m_gpu.TickWork();
    // Guest writes are coalesced in the invalidate accumulator; undrained, a
    // draw can be served from a buffer whose guest memory was overwritten.
    if (gpu_memory) {
        gpu_memory->FlushCaching();
    }

    // Attachment formats are part of the pipeline key.
    if (!UpdateRenderTargets(false, ClearAspects{})) {
        DROP_DRAW("render targets could not be resolved");
        return;
    }
    UpdatePipelineState();

    if (m_has_render_pass && m_render_pass.incomplete) {
        // The pass lost a declared attachment (see RenderPassState::incomplete)
        // so the pipeline's attachment formats no longer match it. The mismatch
        // was observed writing into the previously open pass's tile memory.
        static std::atomic<u64> skipped{0};
        const u64 count = skipped.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count % 10000 == 0) {
            LOG_WARNING(Render_Metal, "Skipped {} draw(s): incomplete render pass", count);
        }
        return;
    }

    ++m_frame_draws;
    GraphicsPipeline* pipeline = m_pipeline_cache.CurrentGraphicsPipeline();
    if (pipeline == nullptr && m_pipeline_cache.SkippedUnfinished()) {
        // Skipping a draw whose pipeline is still compiling only hides it until a later frame
        // draws it again. A target never drawn in an earlier frame may be drawn only this once
        // (e.g. a reflection probe cubemap or lightmap baked once on scene load), so wait for
        // its pipeline instead of leaving the surface blank/black for the entire scene.
        // Likewise, transform-feedback and rasterizer-discard (vertex-write) draws may populate
        // buffers read for many subsequent frames and must not be skipped.
        const bool must_wait = maxwell3d->regs.transform_feedback_enabled != 0 ||
                               maxwell3d->regs.rasterize_enable == 0;
        bool drawn_before = false;
        if (!must_wait) {
            std::scoped_lock lock{m_texture_cache.mutex};
            drawn_before = m_texture_cache.RenderTargetsDrawnBefore();
        }
        if (drawn_before) {
            ++m_frame_skipped_draws;
            return;
        }
        pipeline = m_pipeline_cache.WaitForCurrentGraphicsPipeline();
    }
    if (pipeline == nullptr) {
        static std::atomic<u64> skipped{0};
        const u64 count = skipped.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count % 10000 == 0) {
            LOG_WARNING(Render_Metal, "Skipped {} draw(s): no usable pipeline", count);
        }
        return;
    }
    // Non-null when the guest's geometry stage is emulated: the vertex program
    // runs as a compute kernel, a second expands its output into primitives, and
    // the render pipeline's vertex function is a generated passthrough.
    const GeometryEmulation* geometry = pipeline->GeometryParams();
    const VtgEmulation* vtg = pipeline->VtgParams();

    const GeometryEmulation* xfb_capture = nullptr;
    if (geometry != nullptr && geometry->xfb_only) {
        xfb_capture = geometry;
        geometry = nullptr;
    }

    const bool vtg_xfb = vtg != nullptr && vtg->xfb_enabled &&
                         maxwell3d->regs.transform_feedback_enabled != 0 &&
                         Metal::Options::TransformFeedback();
    const bool discard_with_vertex_writes =
        pipeline->IsRasterizerDiscard() && pipeline->HasVertexStorageWrites();
    if (pipeline->IsRasterizerDiscard() && xfb_capture == nullptr && !vtg_xfb &&
        !discard_with_vertex_writes) {
        // Culling both faces produces no fragments and Metal has no cull mode
        // for it. NOT dropped for a capture draw or vertex storage writes.
        DROP_DRAW("rasterizer discard (cull both faces), no xfb capture");
        return;
    }

    const bool vtg_gpu_indirect = vtg != nullptr && indirect != nullptr && !is_indexed &&
                                  !indirect->include_count && !indirect->is_byte_count &&
                                  indirect->count == 1;
    if (vtg != nullptr && indirect != nullptr && !vtg_gpu_indirect) {
        static std::atomic<u32> warned{0};
        if (warned++ < 8) {
            LOG_WARNING(Render_Metal,
                        "Unsupported indirect draw variant with emulated geometry stage");
        }
        return;
    }

    bool use_gpu_predicate = false;
    if (m_gpu_predicate.active && m_gpu_predicate.engine == maxwell3d) {
        if (m_gpu_predicate.cpu_resolved) {
            if (!m_gpu_predicate.cpu_result) {
                return;
            }
        } else if (!m_gpu_predicate.from_buffer && m_scheduler.IsFree(m_gpu_predicate.wait_tick)) {
            if (!EvaluateGpuPredicateOnCpu()) {
                return;
            }
        } else if (indirect != nullptr || vtg != nullptr || xfb_capture != nullptr) {
            if (!EvaluateGpuPredicateOnCpu()) {
                return;
            }
        } else {
            // Ensure the pass/compute that wrote the visibility slots or predicate buffer
            // is in a strictly earlier submission so RecordUploadCompute on
            // mtl4_upload_encoder reads finalized data without closing this draw's TBDR pass.
            if (m_gpu_predicate.wait_tick >= m_scheduler.CurrentTick()) {
                m_scheduler.Flush();
            }
            use_gpu_predicate = true;
        }
    }

    if (indirect != nullptr && indirect->include_count && m_scheduler.HasWrittenBuffers() &&
        (!m_has_render_pass || !m_scheduler.IsCurrentRenderPass(m_render_pass))) {
        // Commit prior occlusion/compute writes at this render-pass boundary so every
        // DrawIndirectCount in the upcoming pass can patch on the upload compute encoder
        // without breaking the active TBDR render pass.
        m_scheduler.Flush();
    }

    m_buffer_cache_runtime.ResetDrawBindings();

    std::array<StageResources, NUM_PIPELINE_STAGES> stage_resources{};
    // A flush between binding and recording would leave this draw's buffer usage marked
    // under the previous submission, and a reordered upload could then run before it.
    if (m_has_render_pass) {
        m_scheduler.FlushIfPassBoundaryDue(m_render_pass);
    }
    // The shared buffer cache marks vertex buffer usage only for dirty streams. Vulkan
    // dirties them for every new command buffer, so a new submission does the same here.
    const u64 bind_tick = m_scheduler.CurrentTick();
    if (bind_tick != m_vertex_usage_tick) {
        m_vertex_usage_tick = bind_tick;
        auto& flags = maxwell3d->dirty.flags;
        flags[VideoCommon::Dirty::VertexBuffers] = true;
        for (u32 index = VideoCommon::Dirty::VertexBuffer0;
             index <= VideoCommon::Dirty::VertexBuffer31; ++index) {
            flags[index] = true;
        }
    }
    m_scheduler.DeferBoundaryFlush(true);
    SCOPE_EXIT {
        m_scheduler.DeferBoundaryFlush(false);
    };
    const auto& regs = maxwell3d->regs;
    const auto& draw_state = maxwell3d->draw_manager->GetDrawState();
    DrawParams draw = MakeDrawParams(draw_state, instance_count, is_indexed);

    VtgStageBindings vtg_bindings{};
    if (vtg != nullptr) {
        vtg_bindings.is_indexed = is_indexed;
        vtg_bindings.is_indirect = vtg_gpu_indirect;
        vtg_bindings.indirect_instance_mask =
            vtg_gpu_indirect ? indirect->instance_mask : 0xFFFFFFFFu;
        vtg_bindings.vertex_count =
            is_indexed ? draw_state.index_buffer.count : draw_state.vertex_buffer.count;
        vtg_bindings.instance_count = instance_count;
        vtg_bindings.base_vertex =
            is_indexed ? draw_state.base_index : draw_state.vertex_buffer.first;
        vtg_bindings.first_index = is_indexed ? draw_state.index_buffer.first : 0;
        vtg_bindings.base_instance = draw_state.base_instance;
        vtg_bindings.index_bytes =
            is_indexed ? draw_state.index_buffer.FormatSizeInBytes() : 0;
    }

    const u64 alias_syncs = m_texture_cache.AliasSyncCount();
    ConfigureGraphicsResources(*pipeline, is_indexed, stage_resources, /*descriptors_synced=*/true,
                               vtg != nullptr ? &vtg_bindings : nullptr);
    // Vulkan fills the sampled views first and prepares the render targets after, so a
    // target is always newer than the aliases this draw reads. Here the targets were
    // prepared first, and syncing a sampled alias (a padded twin) stamped it newer than
    // the target this draw is about to write: the next read of the alias skipped the copy
    // and saw stale texels. TOTK lost its shadows in rectangles on a resolution switch.
    // Re-marking the targets restores Vulkan's order without rebuilding anything. Only an
    // alias synced for this draw can be stamped newer than a target.
    if (m_texture_cache.AliasSyncCount() != alias_syncs) {
        std::scoped_lock lock{m_texture_cache.mutex};
        if (!maxwell3d->dirty.flags[VideoCommon::Dirty::RenderTargets]) {
            m_texture_cache.UpdateRenderTargets(false);
        }
    }

    // What gl_BaseVertex and gl_BaseInstance read in capture and geometry kernels.
    m_capture_draw_base = {draw.base_vertex, draw.base_instance};

    // A submission with four render encoders on a 1024x1024 depth target ran 45
    // SECONDS at 100% GPU, which is not slow, that is a draw asking for work
    // that cannot finish. The counts come straight from guest registers and
    // that same session showed index data landing far outside anything the
    // bound streams hold.
    if (draw.num_vertices == 0 && indirect == nullptr) {
        if (vtg != nullptr && !is_indexed && !vtg->has_largest_draw) {
            // A zero-vertex fallback from DrawArraysIndirect (e.g. culled GPU emitters)
            // would otherwise leave has_largest_draw == false forever, causing a
            // synchronous ReadBlock() -> Scheduler::Wait() stall on every frame.
            vtg->largest_draw = (std::max<u64>)(vtg->largest_draw, 256u);
            vtg->has_largest_draw = true;
        }
        if (geometry != nullptr || xfb_capture != nullptr) {
            static std::atomic<u32> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
                LOG_WARNING(Render_Metal,
                            "geometry/xfb draw dropped: zero CPU vertex count (indexed={} "
                            "topology={} index_count={})",
                            is_indexed, static_cast<u32>(draw_state.topology),
                            draw_state.index_buffer.count);
            }
        }
        DROP_DRAW("zero CPU vertex count");
        return;
    }

    // Non-indexed fans, polygons and line loops. The shared buffer cache only
    // generates indices for Quads and QuadStrip, so the rest get expanded here.
    if (vtg == nullptr) {
        if (!is_indexed && NeedsIndexConversion(draw_state.topology) &&
            !m_buffer_cache_runtime.GetIndexBinding().converted) {
            if (!m_buffer_cache_runtime.BindGeneratedIndexBuffer(draw_state.topology,
                                                                 draw_state.vertex_buffer.first,
                                                                 draw_state.vertex_buffer.count)) {
                DROP_DRAW("generated index buffer rejected");
                return;
            }
        }

        const IndexBufferBinding& converted = m_buffer_cache_runtime.GetIndexBinding();
        if (converted.converted) {
            draw.is_indexed = true;
            draw.num_vertices = converted.num_indices;
            draw.first_index = 0;
            if (converted.folds_base_vertex) {
                draw.base_vertex = 0;
            }
        }
        if (draw.is_indexed && !converted.valid) {
            if (geometry != nullptr || xfb_capture != nullptr) {
                static std::atomic<u32> logged{0};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
                    LOG_WARNING(Render_Metal,
                                "geometry/xfb draw dropped: invalid index binding (converted={} "
                                "topology={} count={} format={})",
                                converted.converted, static_cast<u32>(draw_state.topology),
                                draw.num_vertices,
                                static_cast<u32>(draw_state.index_buffer.format));
                }
            }
            return;
        }
    }

    const MTL::CullMode cull_mode =
        regs.gl_cull_test_enabled != 0 ? CullFace(regs.gl_cull_face) : MTL::CullModeNone;
    MTL::Winding winding = FrontFace(regs.gl_front_face);
    if (regs.window_origin.flip_y != 0) {
        // A flip_y guest hands geometry wound for a bottom-up raster.
        winding = winding == MTL::WindingClockwise ? MTL::WindingCounterClockwise
                                                   : MTL::WindingClockwise;
    }
    // An expanded topology is drawn as whatever its generated indices describe,
    // not as whatever PrimitiveTopology() would fall back to.
    MTL::PrimitiveType primitive =
        geometry != nullptr             ? MTL::PrimitiveTypeTriangle
        : NeedsIndexConversion(draw_state.topology)
            ? ConvertedPrimitive(draw_state.topology)
            : PrimitiveTopology(m_device, draw_state.topology);
    if (vtg != nullptr) {
        switch (vtg->geometry_bindings.output_topology) {
        case Shader::OutputTopology::PointList:
            primitive = MTL::PrimitiveTypePoint;
            break;
        case Shader::OutputTopology::LineStrip:
            primitive = MTL::PrimitiveTypeLineStrip;
            break;
        case Shader::OutputTopology::TriangleStrip:
            primitive = MTL::PrimitiveTypeTriangleStrip;
            break;
        }
    }

    DepthStencilState ds_state =
        discard_with_vertex_writes
            ? DepthStencilState{}
            : MakeDepthStencilState(regs, m_render_pass.depth_stencil.texture != nullptr,
                                    m_render_pass.depth_stencil.has_stencil);
    MTL::Texture* d24_equal_depth_tex = nullptr;
    if (pipeline->State().d24_frag_depth_equal && !ds_state.depth_write &&
        ds_state.depth_compare == MTL::CompareFunctionEqual &&
        m_render_pass.depth_stencil.texture != nullptr) {
        const Shader::Info* const fs_info = pipeline->StageInfo(PipelineStage::Fragment);
        if (fs_info != nullptr && fs_info->stores_frag_depth) {
            ds_state.depth_compare = MTL::CompareFunctionAlways;
            d24_equal_depth_tex = m_render_pass.depth_stencil.texture;
        }
    }
    MTL::DepthStencilState* depth_stencil = m_pipeline_cache.GetDepthStencilCache().Get(ds_state);
    const u32 stencil_ref_front = regs.stencil_front_ref;
    const u32 stencil_ref_back =
        regs.stencil_two_side_enable != 0 ? regs.stencil_back_ref : regs.stencil_front_ref;

    const auto geometry_clip = regs.viewport_clip_control.geometry_clip;
    using GeometryClip = Tegra::Engines::Maxwell3D::Regs::ViewportClipControl::GeometryClip;
    const bool depth_clamp_disabled = geometry_clip == GeometryClip::Passthrough ||
                                      geometry_clip == GeometryClip::FrustumXYZ ||
                                      geometry_clip == GeometryClip::FrustumZ;
    const MTL::DepthClipMode depth_clip_mode =
        depth_clamp_disabled ? MTL::DepthClipModeClip : MTL::DepthClipModeClamp;

    const bool offset_enabled = [&] {
        const MTL::PrimitiveTopologyClass offset_class = [&] {
            if (vtg != nullptr) {
                switch (vtg->geometry_bindings.output_topology) {
                case Shader::OutputTopology::PointList:
                    return MTL::PrimitiveTopologyClassPoint;
                case Shader::OutputTopology::LineStrip:
                    return MTL::PrimitiveTopologyClassLine;
                case Shader::OutputTopology::TriangleStrip:
                    return MTL::PrimitiveTopologyClassTriangle;
                }
            }
            return TopologyClass(draw_state.topology);
        }();
        switch (offset_class) {
        case MTL::PrimitiveTopologyClassPoint:
            return regs.polygon_offset_point_enable != 0;
        case MTL::PrimitiveTopologyClassLine:
            return regs.polygon_offset_line_enable != 0;
        default:
            return regs.polygon_offset_fill_enable != 0;
        }
    }();
    // Halved because Maxwell's depth-bias units are twice the host's.
    const f32 depth_bias_units = offset_enabled ? regs.depth_bias / 2.0f : 0.0f;
    const f32 depth_bias_slope = offset_enabled ? regs.slope_scale_depth_bias : 0.0f;
    const f32 depth_bias_clamp = offset_enabled ? regs.depth_bias_clamp : 0.0f;

    // Kept out of the pipeline key, Metal takes the blend constant per encoder.
    // It still has to be set, or Constant* blend factors multiply by Metal's
    // default of zero.
    const auto& blend_color = regs.blend_color;

    // Metal has no point-fill mode, so points fall back to lines.
    const MTL::TriangleFillMode fill_mode =
        regs.polygon_mode_front == Tegra::Engines::Maxwell3D::Regs::PolygonMode::Fill
            ? MTL::TriangleFillModeFill
            : MTL::TriangleFillModeLines;

    // Copied out because the command runs on the worker thread, after the cache
    // has moved on.
    IndexBufferBinding index_binding = m_buffer_cache_runtime.GetIndexBinding();

    // Only the slots this pipeline's descriptor reads may be bound: the binding
    // array persists between draws, so an untouched slot can still hold a
    // buffer from an unrelated draw that has since been destroyed.
    u32 enabled_vertex_slots = pipeline->EnabledVertexSlots();
    // Only up to the last slot the pipeline reads: copying every slot spilled
    // the inline storage onto the heap on each draw.
    // Up to the last slot the pipeline reads, for the capture kernels.
    const std::span<const BufferBinding> vertex_bindings = [&] {
        const std::span<const BufferBinding> all = m_buffer_cache_runtime.GetVertexBindings();
        return all.first(std::min<size_t>(
            all.size(), static_cast<size_t>(std::bit_width(enabled_vertex_slots))));
    }();
    // Pack enabled vertex streams as a bitmask + flat GPU address array (half the
    // size of {u64, u8} structs due to padding) so more draws fit per CommandChunk.
    const u64 null_buffer_addr = m_buffer_cache_runtime.GetNullBufferAddress();
    u32 vertex_stream_mask = 0;
    boost::container::small_vector<u64, 4> vertex_stream_addrs;
    {
        // Metal has 31 buffer slots and the buffer cache offers 33 bindings, so anything
        // past the limit is dropped. The pipeline's vertex descriptor is clamped the same way.
        constexpr size_t MAX_VERTEX_SLOTS =
            BindingMap::MAX_BUFFER_SLOTS - BindingMap::VERTEX_BUFFER_BASE;
        const bool has_constant = pipeline->State().uses_constant_attributes;
        const u32 constant_binding = pipeline->State().constant_binding;
        for (u32 bits = enabled_vertex_slots; bits != 0; bits &= bits - 1) {
            const u32 index = static_cast<u32>(std::countr_zero(bits));
            if (index >= std::min(vertex_bindings.size(), MAX_VERTEX_SLOTS)) {
                break;
            }
            // A constant attribute slot always reads the zero buffer.
            const bool is_constant_slot = has_constant && index == constant_binding;
            const BufferBinding& binding = vertex_bindings[index];
            const u64 addr = (!is_constant_slot && binding.buffer != nullptr)
                                 ? (binding.gpu_address + binding.offset)
                                 : null_buffer_addr;
            if (addr != 0) {
                vertex_stream_mask |= 1u << index;
                vertex_stream_addrs.push_back(addr);
            }
        }
    }

    if (regs.transform_feedback_enabled != 0) {
        BeginOrSyncXfbSession();
        // With the capture off the guest's feedback buffers keep what they had.
        const bool xfb_disabled = !Metal::Options::TransformFeedback();
        if (!xfb_disabled && xfb_capture != nullptr && indirect == nullptr) {
            RecordXfbCapture(*pipeline, *xfb_capture, draw_state.topology, draw.is_indexed,
                             draw.num_vertices, draw.first_index, draw.base_vertex, instance_count,
                             stage_resources,
                             vertex_bindings,
                             enabled_vertex_slots);
        } else if (!xfb_disabled && xfb_capture != nullptr) {
            // Never silent: a guest captures once per mesh change and consumes
            // for hundreds of frames, so one skipped batch starves all of them.
            LOG_WARNING(Render_Metal, "XFB capture SKIPPED: indirect draw");
        }
    } else {
        m_xfb_session_active = false;
    }

    // Recorded before rasterizer-discard exit so combined Geometry Shader + XFB
    // runs its VS+GS compute stages and compacts emitted vertices into XFB buffers
    // entirely on the GPU without any CPU readback.
    VtgDispatch vtg_dispatch{};
    if (vtg != nullptr) {
        if (!RecordVtgDispatch(*pipeline, *vtg, vtg_bindings, stage_resources, vtg_dispatch)) {
            return;
        }
        if (vtg_xfb) {
            for (u32 stream = 0; stream < 4; ++stream) {
                if (m_xfb_vtg_pending[stream]) {
                    ResolvePendingVtgXfbCounters(stream);
                }
            }
            if (!m_xfb_emitted_buffer) {
                m_xfb_emitted_buffer = Adopt(m_device.GetDevice()->newBuffer(
                    4 * sizeof(u32), MTL::ResourceStorageModeShared));
                if (m_xfb_emitted_buffer) {
                    m_xfb_emitted_buffer->setLabel(MTLSTR("eden vtg xfb emitted counts"));
                    std::memset(m_xfb_emitted_buffer->contents(), 0, 4 * sizeof(u32));
                }
            }
            const auto runtime_bindings = m_buffer_cache_runtime.GetTransformFeedbackBindings();
            boost::container::small_vector<TransformFeedbackBinding, 4> gather_bindings;
            for (size_t i = 0; i < runtime_bindings.size(); ++i) {
                const BufferBinding& binding = runtime_bindings[i];
                const u64 append = i < 4 ? m_xfb_bytes[i] : 0ull;
                const u32 clamped = static_cast<u32>(std::min<u64>(append, binding.size));
                gather_bindings.push_back(TransformFeedbackBinding{
                    .buffer = binding.buffer,
                    .offset = binding.offset + clamped,
                    .size = binding.size - clamped,
                });
            }
            VideoCommon::TransformFeedbackState xfb_state{};
            for (size_t i = 0; i < xfb_state.layouts.size(); ++i) {
                const auto& control = regs.transform_feedback.controls[i];
                xfb_state.layouts[i] = {
                    .stream = control.stream,
                    .varying_count = control.varying_count,
                    .stride = control.stride,
                };
            }
            xfb_state.varyings = regs.stream_out_layout;
            const u32 sentinel = vtg_bindings.emits_points ? vtg_bindings.output_vertices
                                                           : Shader::VTG_RESTART_INDEX;
            const bool gathered = m_transform_feedback.GatherVtg(
                m_staging_pool, vtg_bindings.geometry_out,
                vtg->geometry_stride * static_cast<u32>(sizeof(u32)),
                vtg_bindings.output_vertices, vtg_bindings.index_out,
                vtg_bindings.total_primitives, vtg->geometry_bindings.indices_per_primitive,
                sentinel, vtg->geometry_bindings.output_topology, vtg->geometry_layout, xfb_state,
                gather_bindings, m_xfb_emitted_buffer.get(), 0);
            if (gathered) {
                const u32 est_verts_per_prim =
                    vtg->geometry_bindings.output_topology == Shader::OutputTopology::PointList ? 1u
                    : (vtg->geometry_bindings.output_topology == Shader::OutputTopology::LineStrip
                           ? 2u
                           : 3u);
                const u64 tick = m_scheduler.CurrentTick();
                for (u32 stream = 0; stream < 4; ++stream) {
                    const auto& control = regs.transform_feedback.controls[stream];
                    if (control.varying_count == 0 && control.stride == 0) {
                        continue;
                    }
                    if (m_xfb_emitted_buffer) {
                        m_xfb_vtg_pending[stream] = true;
                        m_xfb_vtg_tick[stream] = tick;
                        m_xfb_vtg_stride[stream] = control.stride;
                        m_xfb_vtg_verts_per_prim[stream] = est_verts_per_prim;
                    } else {
                        m_xfb_primitives[stream] += vtg_bindings.total_primitives;
                        m_xfb_bytes[stream] += u64{vtg_bindings.total_primitives} *
                                               est_verts_per_prim * control.stride;
                    }
                }
                m_recorded_gpu_work += 4;
            }
        }
    }

    if (pipeline->IsRasterizerDiscard() && (!discard_with_vertex_writes || vtg != nullptr)) {
        if (m_buffer_cache.HasSparseWrites()) {
            std::scoped_lock lock{m_buffer_cache.mutex};
            m_buffer_cache.CopySparseWrites();
        }
        DROP_DRAW("rasterizer discard after xfb capture (capture-only draw)");
        return;
    }

    if (vtg != nullptr) {
        draw.is_indexed = true;
        draw.num_vertices = vtg_dispatch.index_count;
        draw.num_instances = 1;
        draw.first_index = 0;
        draw.base_vertex = 0;
        draw.base_instance = 0;
        enabled_vertex_slots = 0;
        vertex_stream_mask = 0;
        vertex_stream_addrs.clear();
        index_binding = IndexBufferBinding{
            .buffer = vtg_dispatch.index_buffer,
            .gpu_address = vtg_dispatch.index_gpu_address,
            .offset = vtg_dispatch.index_offset,
            .type = MTL::IndexTypeUInt32,
            .valid = true,
            .num_indices = vtg_dispatch.index_count,
            .converted = false,
            .folds_base_vertex = false,
        };
    }

    IndirectDraw indirect_draw = indirect != nullptr ? *indirect : IndirectDraw{};
    if (vtg != nullptr && vtg_dispatch.indirect_draw_buffer != nullptr) {
        indirect_draw = IndirectDraw{
            .buffer = vtg_dispatch.indirect_draw_buffer,
            .offset = vtg_dispatch.indirect_draw_offset,
            .stride = 5 * sizeof(u32),
            .include_count = false,
            .is_byte_count = false,
            .count = 1,
        };
    } else if (indirect != nullptr) {
        // Resolved only now: ConfigureGraphicsResources above ran the buffer
        // cache update, which gives the indirect binding its buffer id.
        std::scoped_lock lock{m_buffer_cache.mutex};
        const auto [indirect_buffer, indirect_offset] = m_buffer_cache.GetDrawIndirectBuffer();
        if (indirect_buffer == nullptr || indirect_buffer->Handle() == nullptr) {
            DROP_DRAW("indirect buffer could not be resolved");
            return;
        }
        indirect_draw.buffer = indirect_buffer->Handle();
        indirect_draw.offset = indirect_offset;

        if (indirect_draw.is_byte_count) {
            // The "indirect buffer" IS the counter, not draw arguments.
            const PatchedIndirect args =
                m_buffer_cache_runtime.GetIndexPass().MakeByteCountDrawArgs(
                    indirect_draw.buffer, indirect_draw.offset, indirect_draw.stride);
            if (!args.valid) {
                return;
            }
            indirect_draw.buffer = args.buffer;
            indirect_draw.offset = args.offset;
            indirect_draw.stride = args.stride;
            indirect_draw.count = 1;
        } else if (indirect_draw.include_count) {
            // Metal encodes one command per entry and cannot take a count
            // buffer, so every allowed entry is encoded and the ones past the
            // real count are rewritten to draw nothing.
            const auto [count_buffer, count_offset] = m_buffer_cache.GetDrawIndirectCount();
            if (count_buffer != nullptr && count_buffer->Handle() != nullptr) {
                const bool use_upload =
                    !m_scheduler.IsBufferWritten(indirect_draw.buffer) &&
                    !m_scheduler.IsBufferWritten(count_buffer->Handle());
                const PatchedIndirect patched =
                    m_buffer_cache_runtime.GetIndexPass().PatchIndirectCount(
                        indirect_draw.buffer, indirect_draw.offset, indirect_draw.stride,
                        indirect_draw.count, count_buffer->Handle(), count_offset, is_indexed,
                        use_upload);
                if (patched.valid) {
                    indirect_draw.buffer = patched.buffer;
                    indirect_draw.offset = patched.offset;
                    indirect_draw.stride = patched.stride;
                } else if (gpu_memory != nullptr) {
                    const auto& params = maxwell3d->draw_manager->GetIndirectParams();
                    const u32 count = gpu_memory->Read<u32>(params.count_start_address);
                    indirect_draw.count = (std::min)(count, indirect_draw.count);
                }
            }
        }
        if (indirect_draw.count == 0) {
            DROP_DRAW("indirect draw count is zero");
            return;
        }
    }
    // A stage that wanted an argument buffer and did not get one cannot be
    // drawn: the encoder still holds the previous draw's buffer, so the draw
    // would sample another object's resources.
    if (std::ranges::any_of(stage_resources,
                            [](const StageResources& stage) { return stage.resources_failed; })) {
        static std::atomic<u32> skipped{0};
        const u32 count = ++skipped;
        if (std::has_single_bit(count)) {
            LOG_WARNING(Render_Metal, "Skipped {} draw(s): argument buffer allocation failed",
                        count);
        }
        return;
    }

    if (use_gpu_predicate) {
        u32 pred_draw_count = draw.num_vertices;
        const u32 pred_base_vertex = draw.base_vertex;
        if (draw.is_indexed) {
            const u32 index_size = index_binding.type == MTL::IndexTypeUInt32 ? 4u : 2u;
            pred_draw_count =
                index_binding.converted ? index_binding.num_indices : draw.num_vertices;
            const u32 effective_offset =
                (index_binding.converted || index_binding.first_index_baked)
                    ? index_binding.offset
                    : index_binding.offset + draw.first_index * index_size;
            index_binding.offset = effective_offset;
        }
        if (pred_draw_count == 0 || draw.num_instances == 0) {
            return;
        }
        u32 pflags = draw.is_indexed ? 16u : 0u;
        if (m_gpu_predicate.from_buffer) {
            pflags |= 32u;
        } else {
            if (m_gpu_predicate.a_from_gpu) {
                pflags |= 1u | (m_gpu_predicate.a_is_64bit ? 2u : 0u);
            }
            if (m_gpu_predicate.b_from_gpu) {
                pflags |= 4u | (m_gpu_predicate.b_is_64bit ? 8u : 0u);
            }
        }
        const ConditionalDrawParams cond_params{
            .a_cpu_value = m_gpu_predicate.a_cpu_value,
            .b_cpu_value = m_gpu_predicate.b_cpu_value,
            .mode = static_cast<u32>(m_gpu_predicate.mode),
            .flags = pflags,
            .a_first_slot = static_cast<u32>(m_gpu_predicate.a_first_slot % VISIBILITY_SLOTS),
            .a_slot_count =
                static_cast<u32>(m_gpu_predicate.a_end_slot - m_gpu_predicate.a_first_slot),
            .b_first_slot = static_cast<u32>(m_gpu_predicate.b_first_slot % VISIBILITY_SLOTS),
            .b_slot_count =
                static_cast<u32>(m_gpu_predicate.b_end_slot - m_gpu_predicate.b_first_slot),
            .vis_mask = VISIBILITY_SLOTS - 1u,
            .draw_count = pred_draw_count,
            .num_instances = draw.num_instances,
            .base_vertex = pred_base_vertex,
            .base_instance = draw.base_instance,
        };
        MTL::Buffer* pred_src_buffer = m_visibility_buffer.get();
        u32 pred_src_offset = 0u;
        if (m_gpu_predicate.from_buffer &&
            !ResolveBufferPredicate(pred_src_buffer, pred_src_offset)) {
            if (!EvaluateGpuPredicateOnCpu()) {
                return;
            }
            use_gpu_predicate = false;
        }
        const PatchedIndirect cond_args =
            use_gpu_predicate
                ? m_buffer_cache_runtime.GetIndexPass().MakeConditionalDrawArgs(
                      pred_src_buffer, cond_params, pred_src_offset)
                : PatchedIndirect{};
        if (cond_args.valid) {
            if (!m_gpu_predicate.from_buffer) {
                const u64 cur_tick = m_scheduler.CurrentTick();
                if (m_gpu_predicate.a_from_gpu) {
                    for (u64 s = m_gpu_predicate.a_first_slot; s < m_gpu_predicate.a_end_slot;
                         ++s) {
                        const u32 idx = static_cast<u32>(s % VISIBILITY_SLOTS);
                        m_visibility_slot_tick[idx] =
                            std::max(m_visibility_slot_tick[idx], cur_tick);
                    }
                }
                if (m_gpu_predicate.b_from_gpu) {
                    for (u64 s = m_gpu_predicate.b_first_slot; s < m_gpu_predicate.b_end_slot;
                         ++s) {
                        const u32 idx = static_cast<u32>(s % VISIBILITY_SLOTS);
                        m_visibility_slot_tick[idx] =
                            std::max(m_visibility_slot_tick[idx], cur_tick);
                    }
                }
            }
            indirect_draw = IndirectDraw{
                .buffer = cond_args.buffer,
                .offset = cond_args.offset,
                .stride = cond_args.stride,
                .include_count = false,
                .is_byte_count = false,
                .count = 1,
            };
        } else if (!EvaluateGpuPredicateOnCpu()) {
            return;
        }
    }

    // Build viewports/scissors AFTER all early returns (XFB discard, geometry failure,
    // zero indirect count, resource failure) so VIEWPORT_SCISSOR_DIRTY is never cleared
    // on a dropped draw and skipped draws pay zero viewport work.
    const f32 scale = m_texture_cache.IsRescaling()
                          ? videoSettings.resolution_info.up_factor
                          : 1.0f;
    const bool scale_offset_enabled = regs.viewport_scale_offset_enabled != 0;
    const bool lower_left =
        regs.window_origin.mode != Tegra::Engines::Maxwell3D::Regs::WindowOrigin::Mode::UpperLeft;
    {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            LOG_INFO(Render_Metal,
                     "orientation: scale_offset={} lower_left={} flip_y={} scale_y={} swizzle_y={}",
                     scale_offset_enabled, lower_left, regs.window_origin.flip_y.Value(),
                     regs.viewport_transform[0].scale_y,
                     static_cast<u32>(regs.viewport_transform[0].swizzle.y.Value()));
        }
    }

    const s64 area_width = static_cast<s64>(m_render_pass.render_area_width);
    const s64 area_height = static_cast<s64>(m_render_pass.render_area_height);

    // D24 depth buffers are promoted to D32F on Apple Silicon. Add a 1-ULP D24
    // compare offset on read-only ordered depth tests (matches Vulkan PR #426)
    // so decals/shadows that would compare equal in 24-bit do not Z-fight.
    const f32 depth_offset = [&]() -> f32 {
        if (m_device.SupportsDepth24Stencil8() || regs.depth_test_enable == 0 ||
            regs.depth_write_enabled != 0) {
            return 0.0f;
        }
        const bool is_d24 = regs.zeta.format == Tegra::DepthFormat::Z24_UNORM_S8_UINT ||
                            regs.zeta.format == Tegra::DepthFormat::X8Z24_UNORM ||
                            regs.zeta.format == Tegra::DepthFormat::S8Z24_UNORM ||
                            regs.zeta.format == Tegra::DepthFormat::V8Z24_UNORM;
        if (!is_d24) {
            return 0.0f;
        }
        static constexpr f32 STEP = 1.0f / 16777215.0f;
        using Op = Tegra::Engines::Maxwell3D::Regs::ComparisonOp;
        switch (regs.depth_test_func) {
        case Op::GreaterEqual_D3D:
        case Op::GreaterEqual_GL:
            return STEP;
        case Op::LessEqual_D3D:
        case Op::LessEqual_GL:
            return -STEP;
        default:
            return 0.0f;
        }
    }();

    auto& draw_dirty = maxwell3d->dirty.flags;
    const bool viewports_need_rebuild =
        !m_viewports_recorded || draw_dirty[VIEWPORT_SCISSOR_DIRTY] ||
        m_cached_vp_engine != maxwell3d || m_cached_vp_area_width != area_width ||
        m_cached_vp_area_height != area_height || m_cached_vp_scale != scale ||
        m_cached_vp_depth_offset != depth_offset;

    std::array<MTL::Viewport, NUM_VIEWPORTS> mtl_viewports;
    std::array<MTL::ScissorRect, NUM_VIEWPORTS> mtl_scissors;
    bool any_scissor = m_cached_vp_any_scissor;
    // Binding all sixteen makes the driver validate sixteen rectangles per
    // draw, and titles almost always leave them identical.
    size_t viewport_count = m_recorded_viewport_count;

    if (viewports_need_rebuild) {
        draw_dirty[VIEWPORT_SCISSOR_DIRTY] = false;
        m_cached_vp_engine = maxwell3d;
        m_cached_vp_area_width = area_width;
        m_cached_vp_area_height = area_height;
        m_cached_vp_scale = scale;
        m_cached_vp_depth_offset = depth_offset;
        any_scissor = false;
        viewport_count = 1;

        for (size_t index = 0; index < NUM_VIEWPORTS; ++index) {
            const auto& src_viewport = regs.viewport_transform[index];
            const auto& src_scissor = regs.scissor_test[index];

            if (index > 0 &&
                std::memcmp(&src_viewport, &regs.viewport_transform[0], sizeof(src_viewport)) == 0 &&
                std::memcmp(&src_scissor, &regs.scissor_test[0], sizeof(src_scissor)) == 0) {
                mtl_viewports[index] = mtl_viewports[0];
                mtl_scissors[index] = mtl_scissors[0];
                continue;
            }
            if (index > 0) {
                viewport_count = NUM_VIEWPORTS;
            }

            MTL::Viewport& mtl_viewport = mtl_viewports[index];
            double vk_y;
            double vk_height;
            if (!scale_offset_enabled) {
                mtl_viewport.originX = static_cast<double>(regs.surface_clip.x) * scale;
                vk_y = static_cast<double>(regs.surface_clip.y) * scale;
                mtl_viewport.width =
                    static_cast<double>((std::max)(1, static_cast<s32>(regs.surface_clip.width))) *
                    scale;
                vk_height =
                    static_cast<double>((std::max)(1, static_cast<s32>(regs.surface_clip.height))) *
                    scale;
                if (lower_left) {
                    vk_y += vk_height;
                    vk_height = -vk_height;
                }
                mtl_viewport.znear = 0.0;
                mtl_viewport.zfar = 1.0;
            } else {
                mtl_viewport.originX = (src_viewport.translate_x - src_viewport.scale_x) * scale;
                vk_y = (src_viewport.translate_y - src_viewport.scale_y) * scale;
                mtl_viewport.width = src_viewport.scale_x * 2.0f * scale;
                vk_height = src_viewport.scale_y * 2.0f * scale;
                if (lower_left) {
                    // Mirror around the surface clip height so lower-left viewports stay
                    // aligned with lower-left scissors and fragment Y coordinates (PR #625).
                    vk_y = static_cast<double>(regs.surface_clip.height) * scale - vk_y;
                    vk_height = -vk_height;
                }
                if (src_viewport.swizzle.y.Value() ==
                    Tegra::Engines::Maxwell3D::Regs::ViewportSwizzle::NegativeY) {
                    vk_y += vk_height;
                    vk_height = -vk_height;
                }
                const f32 reduce_z =
                    regs.depth_mode == Tegra::Engines::Maxwell3D::Regs::DepthMode::MinusOneToOne
                        ? 1.0f
                        : 0.0f;
                mtl_viewport.znear = std::clamp(
                    src_viewport.translate_z - src_viewport.scale_z * reduce_z + depth_offset,
                    0.0f, 1.0f);
                mtl_viewport.zfar =
                    std::clamp(src_viewport.translate_z + src_viewport.scale_z + depth_offset,
                               0.0f, 1.0f);
            }
            mtl_viewport.originY = vk_y + vk_height;
            mtl_viewport.height = -vk_height;
            if (mtl_viewport.width < 0.0) {
                mtl_viewport.originX += mtl_viewport.width;
                mtl_viewport.width = -mtl_viewport.width;
            }

            s64 left = 0;
            s64 top = 0;
            s64 right = area_width;
            s64 bottom = area_height;
            if (!scale_offset_enabled) {
                // Matches Vulkan SurfaceClipScissor: surface_clip takes precedence over
                // scissor_test when viewport_scale_offset_enabled is 0, and is not flipped.
                left = static_cast<s64>(regs.surface_clip.x * scale);
                top = static_cast<s64>(regs.surface_clip.y * scale);
                right = left + static_cast<s64>(
                                   (std::max)(1, static_cast<s32>(regs.surface_clip.width)) * scale);
                bottom = top + static_cast<s64>(
                                   (std::max)(1, static_cast<s32>(regs.surface_clip.height)) * scale);
            } else if (src_scissor.enable != 0) {
                s64 min_y = static_cast<s64>(src_scissor.min_y);
                s64 max_y = static_cast<s64>(src_scissor.max_y);
                if (lower_left) {
                    const s64 clip_h = static_cast<s64>(regs.surface_clip.height);
                    const s64 flipped_min = clip_h - max_y;
                    max_y = clip_h - min_y;
                    min_y = flipped_min;
                }
                left = static_cast<s64>(src_scissor.min_x * scale);
                top = static_cast<s64>(min_y * scale);
                right = static_cast<s64>(src_scissor.max_x * scale);
                bottom = static_cast<s64>(max_y * scale);
            }
            left = std::clamp<s64>(left, 0, area_width);
            top = std::clamp<s64>(top, 0, area_height);
            right = std::clamp<s64>(right, left, area_width);
            bottom = std::clamp<s64>(bottom, top, area_height);

            MTL::ScissorRect& mtl_scissor = mtl_scissors[index];
            mtl_scissor.x = static_cast<NS::UInteger>(left);
            mtl_scissor.y = static_cast<NS::UInteger>(top);
            mtl_scissor.width = static_cast<NS::UInteger>(right - left);
            mtl_scissor.height = static_cast<NS::UInteger>(bottom - top);
            if (mtl_scissor.width != 0 && mtl_scissor.height != 0) {
                any_scissor = true;
            }
        }
        m_cached_vp_any_scissor = any_scissor;
    }

    if (!any_scissor) {
        if (discard_with_vertex_writes && area_width > 0 && area_height > 0) {
            mtl_scissors[0] = MTL::ScissorRect{0, 0, 1, 1};
            viewport_count = 1;
            any_scissor = true;
        } else {
            // A degenerate rect is an encoder error, not an empty draw.
            static std::atomic<u32> skipped{0};
            const u32 count = ++skipped;
            if (std::has_single_bit(count)) {
                LOG_WARNING(Render_Metal, "Skipped {} draw(s): every scissor is empty", count);
            }
            return;
        }
    }

    // Open the pass now that every upload is recorded, so the encoder is still
    // current when this draw runs. Must stay BELOW the indirect-argument
    // resolution above: MakeByteCountDrawArgs and PatchIndirectCount both need
    // a compute encoder, which closes the render one, so opening the pass first
    // left every DrawIndirectCount and DrawIndirectByteCount draw with a null
    // CurrentRender() and dropped it on the worker.
    // What this draw can modify, for the feedback-loop test.
    // Mirrors the pipeline's write masks: an output the shader never stores is masked off.
    u32 written_attachments = discard_with_vertex_writes ? 0u : WrittenColorAttachments(*pipeline);
    if (!discard_with_vertex_writes) {
        const bool writes_depth = m_render_pass.depth_stencil.texture != nullptr &&
                                  regs.depth_test_enable != 0 && regs.depth_write_enabled != 0;
        const bool writes_stencil =
            m_render_pass.depth_stencil.has_stencil && regs.stencil_enable != 0;
        if (writes_depth || writes_stencil) {
            written_attachments |= Scheduler::DEPTH_ATTACHMENT_BIT;
        }
    }

    if (m_has_render_pass) {
        ScratchUnwrittenAliases(m_render_pass, written_attachments);
        // Before the descriptor is built: the visibility buffer is named by it,
        // and an encoder opened without one cannot start counting later.
        BeginPassVisibility(m_render_pass);
    }
    if (!discard_with_vertex_writes && m_has_render_pass && m_zpass_enabled &&
        m_render_pass.visibility_buffer != nullptr) {
        // Ensure PassSerial() reflects the pass this draw will actually open before
        // CountingSlot() decides whether to reuse or allocate a new visibility slot.
        m_scheduler.RequestRenderPass(m_render_pass);
        m_visibility_pass_slot = CountingSlot();
    } else {
        m_visibility_pass_slot = NO_VISIBILITY_SLOT;
    }

    const bool viewport_changed =
        viewports_need_rebuild &&
        (!m_viewports_recorded || m_recorded_viewport_count != viewport_count ||
         std::memcmp(m_recorded_viewports.data(), mtl_viewports.data(),
                     viewport_count * sizeof(MTL::Viewport)) != 0 ||
         std::memcmp(m_recorded_scissors.data(), mtl_scissors.data(),
                     viewport_count * sizeof(MTL::ScissorRect)) != 0);
    if (viewport_changed) {
        m_viewports_recorded = true;
        m_recorded_viewport_count = static_cast<u32>(viewport_count);
        m_recorded_viewports = mtl_viewports;
        m_recorded_scissors = mtl_scissors;
    }

    const u32 visibility_slot = m_visibility_pass_slot;
    // Extract only pre-resolved GPU addresses so the worker thread executes zero
    // Objective-C gpuAddress() calls on the hot draw path.
    const StageResources& vs_stage = stage_resources[static_cast<size_t>(PipelineStage::Vertex)];
    const StageResources& fs_stage = stage_resources[static_cast<size_t>(PipelineStage::Fragment)];
    const u64 vertex_arg_addr = vtg != nullptr ? vtg_dispatch.vertex_arg_buffer.gpu_address
                                               : vs_stage.argument_buffer.gpu_address;
    const u64 fragment_arg_addr = fs_stage.argument_buffer.gpu_address;
    const u64 vertex_table_arg_addr =
        vtg != nullptr ? 0ull : vs_stage.table_argument_buffer.gpu_address;
    const u64 fragment_table_arg_addr = fs_stage.table_argument_buffer.gpu_address;
    const bool fragment_reads_attachments = fs_stage.reads_attachments;
    const u32 fragment_attachment_reads = fs_stage.attachment_reads;
    const Shader::Info* const vs_info = pipeline->StageInfo(PipelineStage::Vertex);
    const Shader::Info* const fs_info = pipeline->StageInfo(PipelineStage::Fragment);
    const bool vertex_uses_render_area = vs_info != nullptr && vs_info->uses_render_area;
    const bool fragment_uses_render_area = fs_info != nullptr && fs_info->uses_render_area;
    const bool vertex_uses_rescaling = vs_stage.uses_rescaling && vtg == nullptr;
    const bool vertex_reads_gpu_texture = vs_stage.reads_gpu_texture;
    const bool fragment_uses_rescaling = fs_stage.uses_rescaling;
    const bool need_fragment_push =
        fragment_reads_attachments || fragment_uses_render_area || fragment_uses_rescaling;
    const bool need_vertex_push = vertex_uses_render_area || vertex_uses_rescaling;
    const f32 down_factor =
        m_texture_cache.IsRescaling() ? videoSettings.resolution_info.down_factor : 1.0f;
    // Non-VTG Vertex and Fragment share one rescaling mask array; under VTG,
    // vertex_uses_rescaling is false so only Fragment's mask is needed here.
    const std::array<u32, 6> rescaling_masks =
        fragment_uses_rescaling ? fs_stage.rescaling_masks : vs_stage.rescaling_masks;
    std::array<f32, 4> render_area_data{};
    if (vertex_uses_render_area || fragment_uses_render_area) {
        const u32 clip_w = regs.surface_clip.width;
        const u32 clip_h = regs.surface_clip.height;
        render_area_data[0] = static_cast<f32>(clip_w);
        render_area_data[1] = static_cast<f32>(clip_h);
        if (lower_left) {
            render_area_data[2] = static_cast<f32>(
                m_texture_cache.IsRescaling() ? videoSettings.resolution_info.ScaleUp(clip_h)
                                              : clip_h);
            render_area_data[3] = 1.0f;
        }
    }

    const std::array<f32, 3> depth_bias_arr{depth_bias_units, depth_bias_slope, depth_bias_clamp};
    const std::array<f32, 4> blend_color_arr{blend_color.r, blend_color.g, blend_color.b,
                                             blend_color.a};
    const bool dynamic_changed =
        !m_dynamic_recorded || m_recorded_stencil_front != stencil_ref_front ||
        m_recorded_stencil_back != stencil_ref_back || m_recorded_depth_bias != depth_bias_arr ||
        m_recorded_blend_color != blend_color_arr;
    if (dynamic_changed) {
        m_dynamic_recorded = true;
        m_recorded_stencil_front = stencil_ref_front;
        m_recorded_stencil_back = stencil_ref_back;
        m_recorded_depth_bias = depth_bias_arr;
        m_recorded_blend_color = blend_color_arr;
    }

    const u32 down_factor_bits = std::bit_cast<u32>(down_factor);
    const u32 effective_attachment_reads =
        need_fragment_push ? fragment_attachment_reads : m_recorded_attachment_reads;
    const bool push_changed =
        (need_fragment_push || need_vertex_push) &&
        (!m_push_recorded || m_recorded_attachment_reads != effective_attachment_reads ||
         m_recorded_down_factor_bits != down_factor_bits ||
         m_recorded_render_area != render_area_data ||
         m_recorded_rescaling_masks != rescaling_masks);
    if (push_changed) {
        m_push_recorded = true;
        m_recorded_attachment_reads = effective_attachment_reads;
        m_recorded_down_factor_bits = down_factor_bits;
        m_recorded_render_area = render_area_data;
        m_recorded_rescaling_masks = rescaling_masks;
    }

    if (discard_with_vertex_writes) {
        m_frame_had_vertex_storage_writes = true;
    }
    const u32 packed_flags =
        (static_cast<u32>(cull_mode) << PACK_CULL_SHIFT) |
        (static_cast<u32>(winding) << PACK_WINDING_SHIFT) |
        (static_cast<u32>(depth_clip_mode) << PACK_DEPTH_CLIP_SHIFT) |
        (static_cast<u32>(fill_mode) << PACK_FILL_SHIFT) |
        (static_cast<u32>(primitive) << PACK_PRIMITIVE_SHIFT) |
        (draw.is_indexed ? PACK_IS_INDEXED_BIT : 0u) |
        (index_binding.type == MTL::IndexTypeUInt32 ? PACK_INDEX_U32_BIT : 0u) |
        (vertex_reads_gpu_texture ? PACK_VERTEX_GPU_TEX_BIT : 0u) |
        (need_fragment_push ? PACK_NEED_FPUSH_BIT : 0u) |
        (need_vertex_push ? PACK_NEED_VPUSH_BIT : 0u) |
        (m_frame_had_vertex_storage_writes ? PACK_VERTEX_STORAGE_FRAME_BIT : 0u);

    const auto emit_state_and_draw = [&](auto&& record_fn) {
        if (viewport_changed) {
            if (viewport_count == 1) {
                record_fn([vp = mtl_viewports[0], sc = mtl_scissors[0]](CommandContext& context) {
                    CommandContext::ViewportState& state = context.Viewports();
                    state.viewports[0] = vp;
                    state.scissors[0] = sc;
                    state.count = 1;
                    ++state.generation;
                });
            } else {
                record_fn([viewports = mtl_viewports, scissors = mtl_scissors,
                           count = static_cast<u32>(viewport_count)](CommandContext& context) {
                    CommandContext::ViewportState& state = context.Viewports();
                    std::copy_n(viewports.begin(), count, state.viewports.begin());
                    std::copy_n(scissors.begin(), count, state.scissors.begin());
                    state.count = count;
                    ++state.generation;
                });
            }
        }
        if (dynamic_changed) {
            record_fn([sf = stencil_ref_front, sb = stencil_ref_back, bias = depth_bias_arr,
                       blend = blend_color_arr](CommandContext& context) {
                CommandContext::DynamicDrawState& state = context.DynamicState();
                state.stencil_front = sf;
                state.stencil_back = sb;
                state.depth_bias = bias;
                state.blend_color = blend;
                ++state.generation;
            });
        }
        if (push_changed) {
            record_fn([att = effective_attachment_reads, df = down_factor_bits,
                       ra = render_area_data, rm = rescaling_masks](CommandContext& context) {
                CommandContext::PushConstantState& state = context.PushConstants();
                state.fragment_attachment_reads = att;
                state.down_factor_bits = df;
                state.render_area = ra;
                state.rescaling_masks = rm;
                ++state.generation;
            });
        }
        if (indirect_draw.buffer != nullptr) {
            record_fn([pipeline, depth_stencil, packed_flags, visibility_slot, vertex_arg_addr,
                       fragment_arg_addr, vertex_table_arg_addr, fragment_table_arg_addr,
                       vertex_stream_mask, addrs = std::move(vertex_stream_addrs), indirect_draw,
                       index_binding, d24_equal_depth_tex](CommandContext& context) {
                RenderEncoder encoder = ApplyDrawRenderState(
                    context, pipeline, depth_stencil, packed_flags, visibility_slot,
                    vertex_arg_addr, fragment_arg_addr, vertex_table_arg_addr,
                    fragment_table_arg_addr, vertex_stream_mask, addrs, d24_equal_depth_tex);
                if (encoder == nullptr) {
                    return;
                }
                const auto prim =
                    static_cast<MTL::PrimitiveType>((packed_flags >> PACK_PRIMITIVE_SHIFT) & 7u);
                const bool indexed = (packed_flags & PACK_IS_INDEXED_BIT) != 0;
                for (u32 entry = 0; entry < indirect_draw.count; ++entry) {
                    const u32 at = indirect_draw.offset + entry * indirect_draw.stride;
                    if (indexed) {
                        encoder->drawIndexedPrimitives(prim, index_binding.type,
                                                       index_binding.buffer, index_binding.offset,
                                                       indirect_draw.buffer, at);
                    } else {
                        encoder->drawPrimitives(prim, indirect_draw.buffer, at);
                    }
                }
            });
        } else {
            u32 draw_count = draw.num_vertices;
            u64 index_addr = 0;
            if (draw.is_indexed) {
                const u32 index_size = index_binding.type == MTL::IndexTypeUInt32 ? 4u : 2u;
                draw_count = index_binding.converted ? index_binding.num_indices : draw.num_vertices;
                const u32 index_offset =
                    (index_binding.converted || index_binding.first_index_baked)
                        ? index_binding.offset
                        : index_binding.offset + draw.first_index * index_size;
                const u64 base =
                    index_binding.gpu_address != 0
                        ? index_binding.gpu_address
                        : (index_binding.buffer != nullptr ? index_binding.buffer->gpuAddress()
                                                           : 0ull);
                if (base != 0) {
                    index_addr = base + index_offset;
                }
            }
            record_fn([pipeline, depth_stencil, vertex_arg_addr, fragment_arg_addr,
                       vertex_table_arg_addr, fragment_table_arg_addr, index_addr, draw_count,
                       num_instances = draw.num_instances, base_vertex = draw.base_vertex,
                       base_instance = draw.base_instance, visibility_slot, packed_flags,
                       vertex_stream_mask, addrs = std::move(vertex_stream_addrs),
                       d24_equal_depth_tex](CommandContext& context) {
                RenderEncoder encoder = ApplyDrawRenderState(
                    context, pipeline, depth_stencil, packed_flags, visibility_slot,
                    vertex_arg_addr, fragment_arg_addr, vertex_table_arg_addr,
                    fragment_table_arg_addr, vertex_stream_mask, addrs, d24_equal_depth_tex);
                if (encoder == nullptr || draw_count == 0 || num_instances == 0) {
                    return;
                }
                const auto prim =
                    static_cast<MTL::PrimitiveType>((packed_flags >> PACK_PRIMITIVE_SHIFT) & 7u);
                if ((packed_flags & PACK_IS_INDEXED_BIT) != 0) {
                    if (index_addr != 0) {
                        const MTL::IndexType itype = (packed_flags & PACK_INDEX_U32_BIT) != 0
                                                         ? MTL::IndexTypeUInt32
                                                         : MTL::IndexTypeUInt16;
                        encoder->drawIndexedPrimitives(
                            prim, draw_count, itype, index_addr, num_instances,
                            static_cast<NS::Integer>(static_cast<s32>(base_vertex)), base_instance);
                    }
                } else {
                    encoder->drawPrimitives(prim, base_vertex, draw_count, num_instances,
                                            base_instance);
                }
            });
        }
    };

    if (m_has_render_pass) {
        m_scheduler.RecordInRenderPass(m_render_pass, [&](Scheduler& scheduler) {
            emit_state_and_draw(
                [&](auto&& cmd) { scheduler.RecordLocked(std::forward<decltype(cmd)>(cmd)); });
        });
    } else {
        emit_state_and_draw(
            [&](auto&& cmd) { m_scheduler.Record(std::forward<decltype(cmd)>(cmd)); });
    }
    m_scheduler.DeferBoundaryFlush(false);

    // Once depth is written, the next shader sampling it needs a pass break.
    m_scheduler.MarkPassDrawn(written_attachments, pipeline->HasStorageWrites());
    const u64 pass_serial = m_scheduler.PassSerial();
    if (m_pass_marked_rt_serial != pass_serial) {
        m_pass_marked_rt_serial = pass_serial;
        m_pass_marked_rt_mask = 0;
    }
    const u32 newly_written = written_attachments & ~m_pass_marked_rt_mask;
    if (newly_written != 0) {
        m_pass_marked_rt_mask |= newly_written;
        // Per image, for TextureCache::CheckFeedbackLoop: only an image this pass
        // wrote can differ from memory when a later draw samples it.
        std::scoped_lock lock{m_texture_cache.mutex};
        m_texture_cache.MarkRenderTargetsWritten(
            newly_written & ~Scheduler::DEPTH_ATTACHMENT_BIT,
            (newly_written & Scheduler::DEPTH_ATTACHMENT_BIT) != 0);
    }
    if (!discard_with_vertex_writes && regs.depth_write_enabled != 0) {
        m_scheduler.MarkDepthWritten();
    }
    if (m_buffer_cache.HasSparseWrites()) {
        std::scoped_lock lock{m_buffer_cache.mutex};
        m_buffer_cache.CopySparseWrites();
    }

    m_last_draw_tick = m_scheduler.CurrentTick();
    if (visibility_slot != NO_VISIBILITY_SLOT && visibility_slot < m_visibility_slot_tick.size()) {
        // Keep the slot's tick >= the submission that actually recorded this counting draw
        // (even if a concurrent NCE thread flushed between CountingSlot and RecordInRenderPass).
        m_visibility_slot_tick[visibility_slot] = m_last_draw_tick;
    }
    // If a concurrent thread flushed between ConfigureGraphicsResources (MarkUsage at bind_tick)
    // and RecordInRenderPass (recorded at m_last_draw_tick), flush now so a later draw's
    // reordered upload in m_last_draw_tick cannot run ahead of this draw.
    if (m_last_draw_tick != bind_tick) [[unlikely]] {
        m_scheduler.Flush();
    }
    ++m_draw_counter;
}

bool RasterizerMetal::RecordVtgDispatch(
    GraphicsPipeline& pipeline, const VtgEmulation& vtg, const VtgStageBindings& bindings,
    const std::array<StageResources, NUM_PIPELINE_STAGES>& stages, VtgDispatch& out) {
    if (!bindings.runs) {
        return false;
    }

    const StageResources& vertex_stage = stages[static_cast<size_t>(PipelineStage::Vertex)];
    const StageResources& geometry_stage = stages[static_cast<size_t>(PipelineStage::Geometry)];
    if (vertex_stage.resources_failed || geometry_stage.resources_failed ||
        bindings.passthrough_arg_buffer.buffer == nullptr ||
        bindings.index_out.buffer == nullptr) {
        return false;
    }

    m_scheduler.RequestOutsideRenderPassOperationContext();

    PatchedIndirect indirect_commands{};
    if (bindings.is_indirect) {
        if (!vtg.feedback_buffer) {
            vtg.feedback_buffer =
                m_memory_allocator.CreateBuffer(2 * sizeof(u32), MemoryUsage::Download);
            if (vtg.feedback_buffer && vtg.feedback_buffer->contents() != nullptr) {
                std::memset(vtg.feedback_buffer->contents(), 0, 2 * sizeof(u32));
            }
        }
        if (!vtg.feedback_buffer || bindings.indirect_args_buffer == nullptr) {
            return false;
        }
        indirect_commands = m_buffer_cache_runtime.GetIndexPass().PrepareVtgIndirect(
            bindings.indirect_args_buffer, bindings.indirect_args_offset,
            bindings.info_buffer.buffer, static_cast<u32>(bindings.info_buffer.offset),
            vtg.feedback_buffer.get(), 0, static_cast<u32>(vtg.topology),
            bindings.indirect_capacity, vtg.geometry_bindings.indices_per_primitive,
            bindings.indirect_instance_mask);
        if (!indirect_commands.valid) {
            return false;
        }
    }

    GraphicsPipeline* const pipeline_ptr = &pipeline;
    const u32 vertex_groups = bindings.vertex_invocations / Shader::VTG_LOCAL_SIZE;
    const u32 geometry_groups = bindings.geometry_invocations / Shader::VTG_LOCAL_SIZE;

    const f32 vtg_down_factor =
        m_texture_cache.IsRescaling() ? videoSettings.resolution_info.down_factor : 1.0f;
    const bool vtg_vs_rescaling = vertex_stage.uses_rescaling;
    const Shader::Backend::SPIRV::RescalingLayout vtg_vs_push =
        Shader::Backend::SPIRV::MakeGraphicsPushConstants(
            vertex_stage.rescaling_masks, vtg_down_factor,
            Shader::Backend::SPIRV::NO_ATTACHMENT_READS, {})
            .scaling;
    const bool vtg_gs_rescaling = geometry_stage.uses_rescaling;
    const Shader::Backend::SPIRV::RescalingLayout vtg_gs_push =
        Shader::Backend::SPIRV::MakeGraphicsPushConstants(
            geometry_stage.rescaling_masks, vtg_down_factor,
            Shader::Backend::SPIRV::NO_ATTACHMENT_READS, {})
            .scaling;

    m_scheduler.Record([pipeline_ptr, vertex_arg = vertex_stage.argument_buffer,
                        vertex_table = vertex_stage.table_argument_buffer, vertex_groups,
                        indirect_commands, vtg_vs_rescaling,
                        vtg_vs_push](CommandContext& context) {
        pipeline_ptr->WaitBuilt();
        MTL::ComputePipelineState* const state = pipeline_ptr->VtgVertexHandle();
        if (state == nullptr) {
            return;
        }
        ComputeEncoder encoder = context.RequestCompute();
        if (encoder == nullptr) {
            return;
        }
        encoder->setComputePipelineState(state);
        if (vertex_arg.buffer != nullptr) {
            encoder->setBuffer(vertex_arg.buffer, vertex_arg.offset,
                               BindingMap::ARGUMENT_BUFFER_INDEX);
        }
        if (vertex_table.buffer != nullptr) {
            encoder->setBuffer(vertex_table.buffer, vertex_table.offset,
                               BindingMap::COMPUTE_TABLE_ARGUMENT_BUFFER_INDEX);
        }
        if (vtg_vs_rescaling) {
            encoder->setBytes(&vtg_vs_push, sizeof(vtg_vs_push),
                              BindingMap::COMPUTE_PUSH_CONSTANT_INDEX);
        }
        if (indirect_commands.valid) {
            encoder->dispatchThreadgroups(indirect_commands.buffer, indirect_commands.offset,
                                          MTL::Size::Make(Shader::VTG_LOCAL_SIZE, 1, 1));
        } else {
            encoder->dispatchThreadgroups(MTL::Size::Make(vertex_groups, 1, 1),
                                          MTL::Size::Make(Shader::VTG_LOCAL_SIZE, 1, 1));
        }
    });

    if (!bindings.emits_points && bindings.index_size > 0) {
        m_scheduler.Record([index_buf = bindings.index_out.buffer,
                            index_offset = bindings.index_out.offset,
                            index_size = bindings.index_size](CommandContext& context) {
            BlitEncoder blit = context.RequestBlit();
            if (blit == nullptr) {
                return;
            }
            blit->fillBuffer(index_buf, NS::Range::Make(index_offset, index_size), 0xFF);
        });
    }

    m_scheduler.Record([pipeline_ptr, geometry_arg = geometry_stage.argument_buffer,
                        geometry_table = geometry_stage.table_argument_buffer,
                        geometry_groups, indirect_commands, vtg_gs_rescaling,
                        vtg_gs_push](CommandContext& context) {
        pipeline_ptr->WaitBuilt();
        MTL::ComputePipelineState* const state = pipeline_ptr->VtgGeometryHandle();
        if (state == nullptr) {
            return;
        }
        ComputeEncoder encoder = context.RequestCompute();
        if (encoder == nullptr) {
            return;
        }
        encoder->setComputePipelineState(state);
        if (geometry_arg.buffer != nullptr) {
            encoder->setBuffer(geometry_arg.buffer, geometry_arg.offset,
                               BindingMap::ARGUMENT_BUFFER_INDEX);
        }
        if (geometry_table.buffer != nullptr) {
            encoder->setBuffer(geometry_table.buffer, geometry_table.offset,
                               BindingMap::COMPUTE_TABLE_ARGUMENT_BUFFER_INDEX);
        }
        if (vtg_gs_rescaling) {
            encoder->setBytes(&vtg_gs_push, sizeof(vtg_gs_push),
                              BindingMap::COMPUTE_PUSH_CONSTANT_INDEX);
        }
        if (indirect_commands.valid) {
            encoder->dispatchThreadgroups(indirect_commands.buffer,
                                          indirect_commands.offset + 3 * sizeof(u32),
                                          MTL::Size::Make(Shader::VTG_LOCAL_SIZE, 1, 1));
        } else {
            encoder->dispatchThreadgroups(MTL::Size::Make(geometry_groups, 1, 1),
                                          MTL::Size::Make(Shader::VTG_LOCAL_SIZE, 1, 1));
        }
    });

    out.vertex_arg_buffer = bindings.passthrough_arg_buffer;
    out.index_buffer = bindings.index_out.buffer;
    out.index_gpu_address = bindings.index_out.gpu_address - bindings.index_out.offset;
    out.index_offset = bindings.index_out.offset;
    out.index_count = bindings.total_primitives * vtg.geometry_bindings.indices_per_primitive;
    if (indirect_commands.valid) {
        out.indirect_draw_buffer = indirect_commands.buffer;
        out.indirect_draw_offset = indirect_commands.offset + 6 * sizeof(u32);
    }
    switch (vtg.geometry_bindings.output_topology) {
    case Shader::OutputTopology::PointList:
        out.primitive = MTL::PrimitiveTypePoint;
        break;
    case Shader::OutputTopology::LineStrip:
        out.primitive = MTL::PrimitiveTypeLineStrip;
        break;
    case Shader::OutputTopology::TriangleStrip:
        out.primitive = MTL::PrimitiveTypeTriangleStrip;
        break;
    }
    return true;
}

bool RasterizerMetal::RecordXfbCapture(
    GraphicsPipeline& pipeline, const GeometryEmulation& xfb,
    Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology, bool is_indexed, u32 count,
    u32 first_index, u32 base_vertex, u32 num_instances,
    const std::array<StageResources, NUM_PIPELINE_STAGES>& stages,
    std::span<const BufferBinding> vertex_bindings, u32 enabled_vertex_slots) {
    m_scheduler.RequestOutsideRenderPassOperationContext();

    using Topology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
    using IndexFormat = Tegra::Engines::Maxwell3D::Regs::IndexFormat;

    const auto refuse = [&](const char* why) {
        static std::atomic<u64> refused{0};
        const u64 n = refused.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || n % 10000 == 0) {
            LOG_WARNING(Render_Metal, "XFB capture refused ({}), {} so far", why, n);
        }
        return false;
    };

    if (count == 0 || num_instances == 0) {
        return refuse("empty draw");
    }

    const IndexBufferBinding& index_binding = m_buffer_cache_runtime.GetIndexBinding();
    Topology effective_topology = topology;
    IndexFormat index_format = IndexFormat::UnsignedInt;
    MTL::Buffer* index_source = nullptr;
    u64 index_source_offset = 0;
    bool source_indexed = false;

    if (index_binding.converted) {
        if (ConvertedPrimitive(topology) != MTL::PrimitiveTypeTriangle) {
            return refuse("rewritten topology is not a triangle list");
        }
        effective_topology = Topology::Triangles;
        source_indexed = true;
        index_source = index_binding.buffer;
        index_source_offset = index_binding.offset;
    } else if (is_indexed) {
        if (!index_binding.valid || index_binding.buffer == nullptr) {
            return refuse("indexed draw with no index binding");
        }
        const u32 index_size = index_binding.type == MTL::IndexTypeUInt32 ? 4 : 2;
        index_format = index_binding.type == MTL::IndexTypeUInt32 ? IndexFormat::UnsignedInt
                                                                  : IndexFormat::UnsignedShort;
        source_indexed = true;
        index_source = index_binding.buffer;
        index_source_offset =
            index_binding.offset +
            (index_binding.first_index_baked ? 0ull : u64{first_index} * index_size);
    }

    const u32 verts_per_prim = IndexPass::VerticesPerPrimitive(effective_topology);
    if (verts_per_prim == 0) {
        return refuse("topology has no primitive expansion");
    }
    const u32 primitives = IndexPass::PrimitiveCount(effective_topology, count);
    if (primitives == 0) {
        return refuse("no complete primitives");
    }
    const u32 per_instance_vertices = primitives * verts_per_prim;
    const u64 total_primitives = u64{primitives} * num_instances;
    const u64 captured_vertices_u64 = u64{per_instance_vertices} * num_instances;

    constexpr u64 FLOAT4 = 16;
    constexpr u64 MAX_XFB_CAPTURE_BYTES = 64ull * 1024 * 1024;
    const u64 captured_bytes = captured_vertices_u64 * xfb.input_stride * FLOAT4;
    if (captured_vertices_u64 == 0 || captured_vertices_u64 > std::numeric_limits<u32>::max() ||
        captured_bytes == 0 || captured_bytes > MAX_XFB_CAPTURE_BYTES) {
        return refuse("capture size out of range");
    }
    const u32 captured_vertices = static_cast<u32>(captured_vertices_u64);

    ConvertedIndices order = m_buffer_cache_runtime.GetIndexPass().ExpandPrimitives(
        effective_topology, index_format, source_indexed, count, base_vertex, index_source,
        static_cast<u32>(index_source_offset));
    if (!order.valid || order.buffer == nullptr || order.num_indices != per_instance_vertices) {
        // buffer checked explicitly: a non-indexed expansion can report valid
        // with nothing behind it, and setBuffer(nullptr) crashes in the driver.
        return refuse("could not flatten the primitives");
    }

    // Never a shared growing buffer: growth reallocates and invalidates both
    // earlier segments and in-flight lambdas' pointers.
    MTL::Buffer* const capture =
        m_transform_feedback.AcquireCaptureSegment(static_cast<u32>(captured_bytes));
    if (capture == nullptr) {
        return refuse("no capture buffer");
    }

    GraphicsPipeline* const pipeline_ptr = &pipeline;
    MTL::Buffer* const null_buffer = m_buffer_cache_runtime.GetNullBuffer();
    const StageResources& vertex_stage = stages[static_cast<size_t>(PipelineStage::Vertex)];
    boost::container::small_vector<BufferBinding, 8> streams;
    for (const BufferBinding& binding : vertex_bindings) {
        streams.push_back(binding);
    }
    // The lambda holds a raw pointer past the pool's own lifetime.
    capture->retain();
    const std::array<u32, 2> draw_base = m_capture_draw_base;
    m_scheduler.Record([pipeline_ptr, capture, order, per_instance_vertices, num_instances,
                        enabled_vertex_slots, null_buffer, vertex_stage, draw_base,
                        streams = std::move(streams)](CommandContext& context) {
        const auto release_capture = [capture]() { capture->release(); };
        const struct Releaser {
            const decltype(release_capture)& fn;
            ~Releaser() {
                fn();
            }
        } releaser{release_capture};
        pipeline_ptr->WaitBuilt();
        MTL::ComputePipelineState* const state = pipeline_ptr->CaptureHandle();
        if (state == nullptr) {
            DROP_DRAW("xfb capture: no capture pipeline");
            return;
        }
        ComputeEncoder encoder = context.RequestCompute();
        if (encoder == nullptr) {
            DROP_DRAW("xfb capture: no compute encoder");
            return;
        }
        encoder->setComputePipelineState(state);

        if (vertex_stage.argument_buffer.buffer != nullptr) {
            encoder->setBuffer(vertex_stage.argument_buffer.buffer,
                               vertex_stage.argument_buffer.offset,
                               BindingMap::ARGUMENT_BUFFER_INDEX);
        }

        constexpr size_t MAX_VERTEX_SLOTS =
            BindingMap::MAX_BUFFER_SLOTS - BindingMap::VERTEX_BUFFER_BASE;
        const size_t slot_count = std::min<size_t>(streams.size(), MAX_VERTEX_SLOTS);
        for (size_t index = 0; index < slot_count; ++index) {
            if ((enabled_vertex_slots & (1u << index)) == 0) {
                continue;
            }
            const u32 slot = BindingMap::VERTEX_BUFFER_BASE + static_cast<u32>(index);
            if (IsCaptureReservedBufferSlot(slot)) {
                continue;
            }
            MTL::Buffer* const buffer =
                streams[index].buffer != nullptr ? streams[index].buffer : null_buffer;
            if (buffer == nullptr) {
                continue;
            }
            const u64 offset = streams[index].buffer != nullptr ? streams[index].offset : 0;
            encoder->setBuffer(buffer, offset, slot);
        }

        encoder->setBuffer(order.buffer, order.offset, CAPTURE_INDEX_BUFFER_INDEX);
        encoder->setBuffer(capture, 0, CAPTURE_OUTPUT_BUFFER_INDEX);

        // Without the stream sizes a guest-supplied index addresses raw memory
        // unchecked and eventually faults the GPU, killing the queue.
        std::array<u32, CAPTURE_DRAW_BASE_SLOT + 2> limits{};
        for (size_t index = 0; index < slot_count; ++index) {
            const u32 slot = BindingMap::VERTEX_BUFFER_BASE + static_cast<u32>(index);
            if (slot >= limits.size()) {
                break;
            }
            if (IsCaptureReservedBufferSlot(slot)) {
                limits[index] = 0;
                const bool stream_is_live = (enabled_vertex_slots & (1u << index)) != 0 &&
                                            streams[index].buffer != nullptr &&
                                            streams[index].size != 0;
                if (stream_is_live) {
                    static std::atomic<u32> collisions{0};
                    const u32 n = collisions.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (n <= 4 || n % 1000 == 0) {
                        LOG_WARNING(Render_Metal,
                                    "Vertex stream {} is bound ({} bytes) but wants buffer slot "
                                    "{}, which the capture and geometry kernels reserve ({} so "
                                    "far): its attributes come out as zeroes",
                                    index, streams[index].size, slot, n);
                    }
                }
                continue;
            }
            limits[index] = streams[index].buffer != nullptr ? streams[index].size : 0u;
        }
        limits[CAPTURE_DRAW_BASE_SLOT] = draw_base[0];
        limits[CAPTURE_DRAW_BASE_SLOT + 1] = draw_base[1];
        encoder->setBytes(limits.data(), limits.size() * sizeof(u32),
                          CAPTURE_LIMITS_BUFFER_INDEX);

        const u32 width =
            std::min<u32>(static_cast<u32>(state->maxTotalThreadsPerThreadgroup()), 32);
        encoder->dispatchThreads(MTL::Size::Make(per_instance_vertices, num_instances, 1),
                                 MTL::Size::Make(std::max(width, 1u), 1, 1));
    });

    // Into the cache buffers VideoCommon bound this draw: frame end is too late
    // for a draw consuming the capture later in the same frame.
    const auto runtime_bindings = m_buffer_cache_runtime.GetTransformFeedbackBindings();
    boost::container::small_vector<TransformFeedbackBinding, 4> gather_bindings;
    for (size_t i = 0; i < runtime_bindings.size(); ++i) {
        if (i < 4 && m_xfb_vtg_pending[i]) {
            ResolvePendingVtgXfbCounters(static_cast<u32>(i));
        }
        const BufferBinding& binding = runtime_bindings[i];
        const u64 append = i < 4 ? m_xfb_bytes[i] : 0ull;
        const u32 clamped = static_cast<u32>(std::min<u64>(append, binding.size));
        gather_bindings.push_back(TransformFeedbackBinding{
            .buffer = binding.buffer,
            .offset = binding.offset + clamped,
            .size = binding.size - clamped,
        });
    }

    // Counted for Query: updated after computing the current draw's append offset.
    for (u32 stream = 0; stream < 4; ++stream) {
        const auto& control = maxwell3d->regs.transform_feedback.controls[stream];
        if (control.varying_count == 0 && control.stride == 0) {
            continue;
        }
        m_xfb_primitives[stream] += total_primitives;
        m_xfb_bytes[stream] += u64{captured_vertices} * control.stride;
    }
    if (gather_bindings.empty() || gather_bindings[0].buffer == nullptr) {
        static std::atomic<u32> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
            const auto& tfb = maxwell3d->regs.transform_feedback;
            LOG_WARNING(Render_Metal,
                        "XFB gather missing binding 0: enable={} size={} addr={:#x} "
                        "varying_count={} control_stride={} runtime_bindings={} b0={} off={} sz={}",
                        tfb.buffers[0].enable, tfb.buffers[0].size, tfb.buffers[0].Address(),
                        tfb.controls[0].varying_count, tfb.controls[0].stride,
                        runtime_bindings.size(),
                        runtime_bindings.empty()
                            ? nullptr
                            : static_cast<const void*>(runtime_bindings[0].buffer),
                        runtime_bindings.empty() ? 0u : runtime_bindings[0].offset,
                        runtime_bindings.empty() ? 0u : runtime_bindings[0].size);
        }
    }
    m_transform_feedback.Gather(capture, xfb.input_stride * static_cast<u32>(FLOAT4),
                                captured_vertices,
                                std::span(xfb.xfb_varyings.data(), xfb.xfb_varying_count),
                                gather_bindings, xfb.xfb_slot_of_generic, xfb.xfb_position_slot);

    // The CPU-side copy of the same gather, deferred to TickFrame: the guest's
    // own CPU reads see guest RAM and nothing else puts the result there.
    if (!XFB_WRITEBACK_ENABLED) {
        m_transform_feedback.ReleaseCaptureSegment(capture);
    } else {
        const auto& tfb_regs = maxwell3d->regs.transform_feedback;
        const auto slot_offset_of_word = [&](u32 word) -> s32 {
            constexpr u32 POSITION_WORD = 28;
            constexpr u32 GENERIC_BASE_WORD = 32;
            if (word >= POSITION_WORD && word < POSITION_WORD + 4) {
                return xfb.xfb_position_slot < 0
                           ? -1
                           : static_cast<s32>(xfb.xfb_position_slot * 16 +
                                              (word - POSITION_WORD) * 4);
            }
            if (word < GENERIC_BASE_WORD) {
                return -1;
            }
            const u32 generic = (word - GENERIC_BASE_WORD) / 4;
            const u32 component = (word - GENERIC_BASE_WORD) % 4;
            if (generic >= xfb.xfb_slot_of_generic.size() ||
                xfb.xfb_slot_of_generic[generic] < 0) {
                return -1;
            }
            return static_cast<s32>(xfb.xfb_slot_of_generic[generic] * 16 + component * 4);
        };

        PendingXfbGather pending{};
        pending.capture_stride = xfb.input_stride * static_cast<u32>(FLOAT4);
        pending.vertices = captured_vertices;

        for (u32 buffer_index = 0; buffer_index < 4; ++buffer_index) {
            PendingXfbGather::Destination destination{};
            for (u32 word = 0; word < xfb.xfb_varyings.size(); ++word) {
                const Shader::TransformFeedbackVarying& varying = xfb.xfb_varyings[word];
                if (varying.components == 0 || varying.buffer != buffer_index) {
                    continue;
                }
                const s32 src = slot_offset_of_word(word);
                if (src < 0) {
                    continue;
                }
                destination.out_stride = varying.stride;
                destination.copies.push_back(XfbCopy{
                    .src_offset = static_cast<u32>(src),
                    .dst_offset = varying.offset,
                    .components = varying.components,
                });
            }
            if (destination.copies.empty() || destination.out_stride == 0) {
                continue;
            }
            destination.guest_address =
                tfb_regs.buffers[buffer_index].Address() +
                static_cast<u64>(tfb_regs.buffers[buffer_index].start_offset);
            if (destination.guest_address == 0) {
                continue;
            }
            const u64 declared = static_cast<u64>(tfb_regs.buffers[buffer_index].size);
            const u64 start = static_cast<u64>(tfb_regs.buffers[buffer_index].start_offset);
            destination.guest_size = declared > start ? declared - start : 0;
            if (destination.guest_size == 0) {
                continue;
            }
            pending.destinations.push_back(std::move(destination));
        }
        if (pending.destinations.empty()) {
            m_transform_feedback.ReleaseCaptureSegment(capture);
        } else {
            pending.capture = capture;
            pending.tick = m_scheduler.GetMasterSemaphore().CurrentTick();
            m_pending_xfb.push_back(std::move(pending));
        }
    }

    m_recorded_gpu_work += 4;

    {
        static std::atomic<u64> captures{0};
        const u64 n = captures.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 16 || n % 1000 == 0) {
            LOG_INFO(Render_Metal, "XFB capture {}: {} vertices, {} KiB", n, captured_vertices,
                     captured_bytes / 1024);
        }
    }
    return true;
}

// Handles come from the launch descriptor's constant buffers instead of
// Maxwell's shader stages, and header indexing from linked_tsc. Otherwise the
// same walk in declaration order, which is what SPIRV-Cross numbers by.
void RasterizerMetal::ConfigureComputeResources(
    const ComputePipeline& pipeline, StagingBufferRef& out_argument_buffer,
    StagingBufferRef& out_table_argument_buffer, std::array<u32, 6>& out_rescaling_masks,
    std::array<u32, BufferCache::MAX_WRITE_TRACK_SLOTS * 2>& out_write_tracking,
    u32& out_write_tracking_bytes, RuntimeImageWriteState& out_runtime_image_writes) {
    out_table_argument_buffer = {};
    out_rescaling_masks.fill(0);
    out_write_tracking.fill(BufferCache::WRITE_TRACK_DISABLED);
    out_write_tracking_bytes = 0;
    out_runtime_image_writes.mask = {};
    out_runtime_image_writes.mask_words = 0;
    out_runtime_image_writes.has_candidates = false;
    out_runtime_image_writes.views.clear();
    const Shader::Info& info = pipeline.GetInfo();
    const auto& qmd = kepler_compute->launch_description;
    const auto& cbufs = qmd.const_buffer_config;
    const bool via_header_index = qmd.linked_tsc != 0;

    boost::container::small_vector<VideoCommon::ImageViewInOut, 32> views;
    boost::container::small_vector<VideoCommon::SamplerId, 32> samplers;

    std::scoped_lock lock{m_texture_cache.mutex, m_buffer_cache.mutex};

    m_texture_cache.SynchronizeComputeDescriptors();
    m_buffer_cache.SetComputeUniformBufferState(info.constant_buffer_mask,
                                                &pipeline.UniformBufferSizes());

    m_buffer_cache.UnbindComputeStorageBuffers();
    u32 ssbo_index = 0;
    for (const auto& desc : info.storage_buffers_descriptors) {
        m_buffer_cache.BindComputeStorageBuffer(ssbo_index, desc.cbuf_index, desc.cbuf_offset,
                                                desc.is_written, desc.untracked_writes_only);
        ++ssbo_index;
    }

    // A descriptor can name a constant buffer the launch descriptor never
    // enabled, whose address walks into unmapped guest memory.
    const auto cbuf_enabled = [&](u32 index) {
        return ((qmd.const_buffer_enable_mask.Value() >> index) & 1) != 0;
    };
    Vulkan::DescriptorCbufReader<8> cbuf_reader;
    const auto bind = [&](u32 bank, u32 count) {
        using Bound = decltype(cbuf_reader.Bind(*gpu_memory, 0, 0, 0, 0));
        if (bank >= cbufs.size() || !cbuf_enabled(bank)) {
            return std::optional<Bound>{};
        }
        return std::optional<Bound>{cbuf_reader.Bind(*gpu_memory, bank, cbufs[bank].Address(),
                                                     cbufs[bank].size.Value(), count)};
    };
    const auto add_views = [&](const auto& descriptors, bool images) {
        for (const auto& desc : descriptors) {
            bool blacklist = false;
            if constexpr (requires { desc.is_written; }) {
                blacklist = images && desc.is_written;
            }
            if constexpr (requires { desc.is_direct; }) {
                if (desc.is_direct) {
                    // The shader indexes the texture table with a runtime handle.
                    for (u32 index = 0; index < desc.count; ++index) {
                        const bool is_3d = m_texture_cache.IsCompute3DImage(
                            index, desc.is_integer, desc.is_signed);
                        views.push_back({.index = is_3d ? index : UINT32_MAX,
                                         .blacklist = blacklist});
                    }
                    continue;
                }
            }
            ForEachTextureHandle(desc, bind, via_header_index, [&](auto handle) {
                views.push_back({.index = handle.first, .blacklist = blacklist});
            });
        }
    };

    add_views(info.texture_buffer_descriptors, false);
    add_views(info.image_buffer_descriptors, false);
    for (const auto& desc : info.texture_descriptors) {
        if (desc.is_direct) {
            for (u32 index = 0; index < desc.count; ++index) {
                views.push_back({.index = index, .is_table = true});
            }
            continue;
        }
        ForEachTextureHandle(desc, bind, via_header_index, [&](auto handle) {
            views.push_back({handle.first});
            samplers.push_back(m_texture_cache.GetComputeSamplerId(handle.second));
        });
    }
    add_views(info.image_descriptors, true);

    m_texture_cache.FillComputeImageViews(std::span(views.data(), views.size()));

    // Texel buffers are linear textures the *buffer* cache builds. The texture
    // cache resolved them only to recover each one's address and size.
    {
        m_buffer_cache.UnbindComputeTextureBuffers();
        size_t texel_view_it = 0;
        size_t index = 0;
        const auto bind = [&](u32 count, bool is_image, bool is_written,
                              Shader::ImageFormat declared) {
            for (u32 element = 0; element < count; ++element) {
                const VideoCommon::ImageViewId id = views[texel_view_it++].id;
                if (id) {
                    ImageView& view = m_texture_cache.GetImageView(id);
                    m_buffer_cache.BindComputeTextureBuffer(index, view.gpu_addr,
                                                            view.BufferSize(),
                                                            TexelBufferFormat(view.format, is_image,
                                                                              declared),
                                                            is_written, is_image);
                } else {
                    // Unreachable today, FillImageViews always assigns an id.
                    static std::atomic<u32> reported{0};
                    if (reported++ < 4) {
                        LOG_ERROR(Render_Metal,
                                  "Texel buffer slot {} has no view; later bindings will shift",
                                  index);
                    }
                }
                ++index;
            }
        };
        for (const auto& desc : info.texture_buffer_descriptors) {
            bind(desc.count, false, false, Shader::ImageFormat::Typeless);
        }
        for (const auto& desc : info.image_buffer_descriptors) {
            bind(desc.count, true, desc.is_written, desc.format);
        }
    }

    m_buffer_cache.UpdateComputeBuffers();

    m_argument_buffer.Reset();
    m_buffer_cache_runtime.ClearPendingBindings();
    m_buffer_cache.SetComputeWriteTracking(info.write_tracking_slots);
    m_buffer_cache.BindHostComputeBuffers();
    const std::span<const u32> tracking_words = m_buffer_cache.ComputeWriteTracking();
    out_write_tracking_bytes = static_cast<u32>(tracking_words.size_bytes());
    std::ranges::copy(tracking_words, out_write_tracking.begin());
    // Same over-read protection the graphics path has. Metal has no
    // robustBufferAccess to bound it. Uniform buffers appear first in pending
    // bindings in ascending constant_buffer_mask bit order, followed by SSBOs.
    const auto& compute_sizes = pipeline.UniformBufferSizes();
    const u32 num_cbufs = static_cast<u32>(std::popcount(info.constant_buffer_mask));
    u32 remaining_cbuf_mask = info.constant_buffer_mask;
    u32 pending_idx = 0;
    for (const BufferBinding& binding : m_buffer_cache_runtime.GetPendingBindings()) {
        BufferBinding bound = binding;
        if (pending_idx < num_cbufs && remaining_cbuf_mask != 0) {
            const u32 slot = static_cast<u32>(std::countr_zero(remaining_cbuf_mask));
            remaining_cbuf_mask &= remaining_cbuf_mask - 1;
            if (slot < compute_sizes.size()) {
                m_buffer_cache_runtime.ExtendUniformBinding(bound, compute_sizes[slot]);
            }
        }
        ++pending_idx;
        if (bound.is_written) {
            m_argument_buffer.AddStorageBuffer(bound.buffer, bound.offset, bound.size, true);
        } else {
            m_argument_buffer.AddUniformBuffer(bound.buffer, bound.offset, bound.size);
        }
    }

    const std::span<MTL::Texture* const> texel_views = m_buffer_cache_runtime.GetTexelViews();
    MTL::Texture* const null_texel = m_buffer_cache_runtime.GetNullTexelView();
    size_t texel_it = 0;
    const auto next_texel = [&]() -> MTL::Texture* {
        MTL::Texture* const view = texel_it < texel_views.size() ? texel_views[texel_it++] : nullptr;
        return view != nullptr ? view : null_texel;
    };

    const bool track_rescaling = m_texture_cache.IsRescaling() && info.uses_rescaling_uniform;
    RescalingMaskBuilder rescaling{};

    size_t view_it = 0;
    size_t sampler_it = 0;
    for (const auto& desc : info.texture_buffer_descriptors) {
        for (u32 element = 0; element < desc.count; ++element) {
            ++view_it;
            // Read view, then the view with the guest's size.
            m_argument_buffer.AddImage(next_texel(), false);
            m_argument_buffer.AddImage(next_texel(), false);
        }
    }
    for (const auto& desc : info.image_buffer_descriptors) {
        for (u32 element = 0; element < desc.count; ++element) {
            ++view_it;
            m_argument_buffer.AddImage(next_texel(), desc.is_written);
            m_argument_buffer.AddImage(next_texel(), desc.is_written);
        }
    }
    const ImageView& null_view = m_texture_cache.GetImageView(VideoCommon::NULL_IMAGE_VIEW_ID);
    const u64 current_tick = m_scheduler.CurrentTick();
    struct DirectTextureEntry {
        const Shader::TextureDescriptor* desc;
        size_t start_view;
    };
    boost::container::small_vector<DirectTextureEntry, 8> direct_textures;
    u64 table_signature = 0x517CC1B727220A95ull ^ info.runtime_sampler_count;
    u32 total_direct_views = 0;
    {
        size_t scan_view = view_it;
        for (const auto& desc : info.texture_descriptors) {
            if (desc.is_direct) {
                direct_textures.push_back(DirectTextureEntry{&desc, scan_view});
                total_direct_views += desc.count;
                table_signature ^= (static_cast<u64>(desc.is_multisample) << 40) |
                                   (static_cast<u64>(desc.type) << 32) |
                                   static_cast<u64>(desc.count);
                table_signature *= 0xBF58476D1CE4E5B9ull;
            }
            scan_view += desc.count;
        }
    }
    const DirectTableCacheEntry* cached_table = nullptr;
    const u64 view_gen = m_texture_cache_runtime.ViewGeneration();
    if (!direct_textures.empty() || info.runtime_sampler_count != 0) {
        for (const DirectTableCacheEntry& entry : m_direct_table_cache) {
            if (!entry.is_compute || entry.tick != current_tick ||
                entry.view_generation != view_gen || entry.signature != table_signature ||
                entry.is_rescaling != track_rescaling ||
                entry.total_views != total_direct_views ||
                entry.runtime_sampler_count != info.runtime_sampler_count ||
                entry.table_buffer.buffer == nullptr) {
                continue;
            }
            bool match = true;
            size_t check_idx = 0;
            for (const auto& [desc_ptr, start_view] : direct_textures) {
                const u32 count = desc_ptr->count;
                for (u32 e = 0; e < count; ++e) {
                    if (entry.view_ids[check_idx++] != views[start_view + e].id) {
                        match = false;
                        break;
                    }
                }
                if (!match) {
                    break;
                }
            }
            if (match) {
                for (u32 s = 0; s < info.runtime_sampler_count; ++s) {
                    if (entry.sampler_ids[s] != m_texture_cache.GetComputeSamplerId(s)) {
                        match = false;
                        break;
                    }
                }
            }
            if (match) {
                cached_table = &entry;
                break;
            }
        }
    }
    u32 direct_rescaled_mask = 0;
    u32 direct_idx = 0;
    boost::container::small_vector<u64, 64> array_samplers;
    for (const auto& desc : info.texture_descriptors) {
        bool desc_rescaled = false;
        if (desc.is_direct) {
            if (cached_table != nullptr) {
                view_it += desc.count;
                if (track_rescaling) {
                    const bool bit =
                        direct_idx < 32 && ((cached_table->rescaled_mask >> direct_idx) & 1u) != 0;
                    rescaling.PushTexture(bit);
                }
                ++direct_idx;
                continue;
            }
            for (u32 element = 0; element < desc.count; ++element) {
                const VideoCommon::ImageViewId view_id = views[view_it++].id;
                if (view_id && track_rescaling) {
                    const ImageView& view = m_texture_cache.GetImageView(view_id);
                    if (!view.IsIntegerFormat()) {
                        desc_rescaled |= m_texture_cache.IsRescaling(view);
                    }
                }
            }
            if (desc_rescaled && direct_idx < 32) {
                direct_rescaled_mask |= 1u << direct_idx;
            }
            ++direct_idx;
            if (track_rescaling) {
                rescaling.PushTexture(desc_rescaled);
            }
            continue;
        }
        const u64 null_tex_id = null_view.ResourceId(desc.type, desc.is_multisample);
        // SPIRV-Cross lays out combined sampled-image arrays in Tier 2 argument
        // buffers as two contiguous arrays: `array<texture, N> [[id(b)]]` followed
        // by `array<sampler, N> [[id(b + N)]]`, NOT interleaved (tex_i, smplr_i).
        const bool is_array = desc.count > 1;
        if (is_array) {
            array_samplers.clear();
            array_samplers.reserve(desc.count);
        }
        for (u32 element = 0; element < desc.count; ++element) {
            const VideoCommon::ImageViewId view_id = views[view_it++].id;
            const VideoCommon::SamplerId sampler_id = samplers[sampler_it++];
            const Sampler& sampler_state = m_texture_cache.GetSampler(sampler_id);
            u64 texture_id = 0;
            u64 sampler = 0;
            if (view_id) {
                const ImageView& view = m_texture_cache.GetImageView(view_id);
                if (track_rescaling) {
                    desc_rescaled |= m_texture_cache.IsRescaling(view);
                }
                texture_id = view.ResourceId(desc.type, desc.is_multisample);
                sampler = sampler_state.ResourceId(SamplerFor(sampler_state, view));
            }
            if (texture_id == 0) {
                texture_id = null_tex_id;
                sampler = sampler_state.ResourceId(sampler_state.Handle());
            }
            if (is_array) {
                m_argument_buffer.AddSlot(texture_id);
                array_samplers.push_back(texture_id != 0 ? sampler : 0);
            } else {
                m_argument_buffer.AddTextureIds(texture_id, sampler);
            }
        }
        if (is_array) {
            m_argument_buffer.AddRawSlots(array_samplers);
        }
        if (track_rescaling) {
            rescaling.PushTexture(desc_rescaled);
        }
    }
    if (info.runtime_image_write_mask_words != 0) {
        out_runtime_image_writes.mask_words = info.runtime_image_write_mask_words;
        out_runtime_image_writes.views.assign(info.runtime_image_write_mask_words * 32u,
                                              VideoCommon::NULL_IMAGE_VIEW_ID);
        size_t img_view_it = view_it;
        for (const auto& desc : info.image_descriptors) {
            for (u32 element = 0; element < desc.count; ++element) {
                const VideoCommon::ImageViewId id = views[img_view_it++].id;
                if (desc.is_direct && id != VideoCommon::NULL_IMAGE_VIEW_ID &&
                    element < out_runtime_image_writes.views.size()) {
                    out_runtime_image_writes.views[element] = id;
                    out_runtime_image_writes.has_candidates = true;
                }
            }
        }
    }
    for (const auto& desc : info.image_descriptors) {
        bool desc_rescaled = false;
        MTL::Texture* const null_img = null_view.Handle(desc.type);
        for (u32 element = 0; element < desc.count; ++element) {
            const VideoCommon::ImageViewId id = views[view_it++].id;
            MTL::Texture* image = nullptr;
            if (id) {
                ImageView& view = m_texture_cache.GetImageView(id);
                if (desc.is_written && !desc.is_direct) {
                    m_texture_cache.MarkModification(view.image_id);
                }
                if (track_rescaling) {
                    desc_rescaled |= m_texture_cache.IsRescaling(view);
                }
                image = view.StorageView(desc.type, desc.format);
            }
            m_argument_buffer.AddImage(image != nullptr ? image : null_img, desc.is_written);
        }
        if (track_rescaling) {
            rescaling.PushImage(desc_rescaled);
        }
    }
    if (info.runtime_image_write_mask_words != 0) {
        const u32 mask_bytes = info.runtime_image_write_mask_words * sizeof(u32);
        out_runtime_image_writes.mask = m_staging_pool.Request(mask_bytes, MemoryUsage::Download);
        if (out_runtime_image_writes.mask.buffer != nullptr) {
            std::memset(out_runtime_image_writes.mask.mapped_span.data(), 0, mask_bytes);
            m_argument_buffer.AddBufferAddress(out_runtime_image_writes.mask.gpu_address);
        } else {
            out_runtime_image_writes.has_candidates = false;
            m_argument_buffer.AddBufferAddress(m_buffer_cache_runtime.GetWriteSinkAddress());
        }
    }
    if (info.write_tracking_slots != 0) {
        MTL::Buffer* const bitmap = m_buffer_cache_runtime.WriteTrackBitmap();
        m_argument_buffer.AddBufferAddress(bitmap != nullptr
                                               ? bitmap->gpuAddress()
                                               : m_buffer_cache_runtime.GetWriteSinkAddress());
    }

    if (track_rescaling) {
        out_rescaling_masks = rescaling.Data();
    }
    const u32 written = m_argument_buffer.SlotCount();
    out_argument_buffer = m_argument_buffer.Build();

    u32 written_table = 0;
    if (!direct_textures.empty() || info.runtime_sampler_count != 0) {
        if (cached_table != nullptr) {
            out_table_argument_buffer = cached_table->table_buffer;
            written_table = pipeline.IsBuilt() ? pipeline.ExpectedTableArgumentSlots() : 0u;
        } else {
            m_argument_buffer.Reset();
            if (pipeline.IsBuilt()) {
                m_argument_buffer.Reserve(pipeline.ExpectedTableArgumentSlots());
            }
            bool any_volume_copy = false;
            for (const auto& [desc_ptr, start_view] : direct_textures) {
                const Shader::TextureDescriptor& desc = *desc_ptr;
                const u64 null_tex_id = null_view.ResourceId(desc.type, desc.is_multisample);
                for (u32 element = 0; element < desc.count; ++element) {
                    const VideoCommon::ImageViewId view_id = views[start_view + element].id;
                    u64 texture_id = 0;
                    if (view_id) {
                        const ImageView& view = m_texture_cache.GetImageView(view_id);
                        any_volume_copy |= view.HasVolumeCopy();
                        if (!view.IsIntegerFormat()) {
                            texture_id = view.ResourceId(desc.type, desc.is_multisample);
                        }
                    }
                    m_argument_buffer.AddBufferAddress(texture_id != 0 ? texture_id : null_tex_id);
                }
            }
            for (u32 index = 0; index < info.runtime_sampler_count; ++index) {
                const VideoCommon::SamplerId sampler_id = m_texture_cache.GetComputeSamplerId(index);
                const Sampler& state = m_texture_cache.GetSampler(sampler_id);
                MTL::SamplerState* const handle = state.HasAddedAnisotropy()
                                                      ? state.HandleWithDefaultAnisotropy()
                                                      : state.Handle();
                m_argument_buffer.AddBufferAddress(state.ResourceId(handle));
            }
            written_table = m_argument_buffer.SlotCount();
            out_table_argument_buffer = m_argument_buffer.Build();
            if (!any_volume_copy && out_table_argument_buffer.buffer != nullptr &&
                direct_textures.size() <= 32) {
                DirectTableCacheEntry& slot =
                    m_direct_table_cache[m_direct_table_cache_next++ &
                                         (m_direct_table_cache.size() - 1)];
                slot.tick = current_tick;
                slot.view_generation = m_texture_cache_runtime.ViewGeneration();
                slot.signature = table_signature;
                slot.total_views = total_direct_views;
                slot.runtime_sampler_count = info.runtime_sampler_count;
                slot.rescaled_mask = direct_rescaled_mask;
                slot.is_rescaling = track_rescaling;
                slot.is_compute = true;
                slot.table_buffer = out_table_argument_buffer;
                slot.view_ids.clear();
                slot.view_ids.reserve(total_direct_views);
                for (const auto& [desc_ptr, start_view] : direct_textures) {
                    const u32 count = desc_ptr->count;
                    for (u32 e = 0; e < count; ++e) {
                        slot.view_ids.push_back(views[start_view + e].id);
                    }
                }
                slot.sampler_ids.clear();
                slot.sampler_ids.reserve(info.runtime_sampler_count);
                for (u32 s = 0; s < info.runtime_sampler_count; ++s) {
                    slot.sampler_ids.push_back(m_texture_cache.GetComputeSamplerId(s));
                }
            }
        }
    }
    if (pipeline.IsBuilt()) {
        const u32 expected = pipeline.ExpectedArgumentSlots();
        const u32 expected_table = pipeline.ExpectedTableArgumentSlots();
        if (expected != written || expected_table != written_table) {
            static std::atomic<u32> warned{0};
            if (warned++ < 16) {
                LOG_WARNING(Render_Metal,
                            "Compute argument buffer mismatch: expected={}, wrote={}, "
                            "expected_table={}, wrote_table={}, rt_img_mask_words={}, typeless_w={}",
                            expected, written, expected_table, written_table,
                            info.runtime_image_write_mask_words, info.uses_typeless_image_writes);
            }
        }
    }
}

template <typename Reads>
u32 RasterizerMetal::SelectAttachmentReads(const Shader::Info& fragment, const Reads& reads,
                                          std::span<VideoCommon::ImageViewInOut> views,
                                          u32 scratch) {
    using Maxwell = Tegra::Engines::Maxwell3D::Regs;
    u32 selector{~0U};
    const Framebuffer* const framebuffer = m_texture_cache.GetFramebuffer();
    if (framebuffer == nullptr || framebuffer->Samples() != 1) {
        return selector;
    }
    const auto& regs = maxwell3d->regs;
    const bool lower_left = regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft;
    // The viewport maps the screen position varying to the texel of the pixel being shaded:
    // it covers the whole texture, flipped like a Vulkan viewport with a negative height.
    const auto covers = [&](const VideoCommon::Extent3D& size) {
        const auto& viewport = regs.viewport_transform[0];
        if (regs.viewport_scale_offset_enabled == 0 ||
            viewport.swizzle.x != Maxwell::ViewportSwizzle::PositiveX ||
            (viewport.swizzle.y != Maxwell::ViewportSwizzle::PositiveY &&
             viewport.swizzle.y != Maxwell::ViewportSwizzle::NegativeY)) {
            return false;
        }
        const f32 x = viewport.translate_x - viewport.scale_x;
        const f32 width = viewport.scale_x * 2.0f;
        f32 y = viewport.translate_y - viewport.scale_y;
        f32 height = viewport.scale_y * 2.0f;
        if (lower_left) {
            y = static_cast<f32>(regs.surface_clip.height) - y;
            height = -height;
        }
        if (viewport.swizzle.y == Maxwell::ViewportSwizzle::NegativeY) {
            y += height;
            height = -height;
        }
        const f32 w = static_cast<f32>(size.width);
        const f32 h = static_cast<f32>(size.height);
        return x == 0.0f && width == w && y == h && height == -h;
    };
    // An aliased slot drawn through scratch holds nothing of the image; another slot of
    // the same image may still hold it.
    const u32 mask = fragment.input_attachment_mask & ~scratch;
    for (size_t slot = 0; slot < reads.size(); ++slot) {
        const Shader::PixelLocalRead kind = reads[slot].kind;
        if (lower_left && kind == Shader::PixelLocalRead::FragCoord) {
            // The reflected coordinate may read a different row than the current fragment.
            continue;
        }
        VideoCommon::ImageViewInOut& view = views[reads[slot].view];
        if (!view.id) {
            continue;
        }
        const ImageView& sampled = m_texture_cache.GetImageView(view.id);
        for (u32 index = 0; index < Maxwell::NumRenderTargets; ++index) {
            const VideoCommon::ImageViewId target_id = m_texture_cache.ColorBufferViewId(index);
            if (((mask >> index) & 1) == 0 || !target_id) {
                continue;
            }
            const ImageView& target = m_texture_cache.GetImageView(target_id);
            if (sampled.image_id != target.image_id || sampled.range.base != target.range.base) {
                continue;
            }
            // Should the view not match this slot, another aliased one cannot match it either.
            if (sampled.type == VideoCommon::ImageViewType::e2D &&
                sampled.range.extent.levels == 1 && sampled.range.extent.layers == 1 &&
                sampled.format == target.format && sampled.HasIdentitySwizzle() &&
                (kind != Shader::PixelLocalRead::ScreenPosition || covers(sampled.size))) {
                const u32 shift = static_cast<u32>(slot) * 4;
                selector = (selector & ~(0xFU << shift)) | (index << shift);
                // Sampling nothing, the read sees what earlier draws wrote to the attachment.
                view.id = VideoCommon::NULL_IMAGE_VIEW_ID;
            }
            break;
        }
    }
    return selector;
}

void RasterizerMetal::ConfigureGraphicsResources(
    const GraphicsPipeline& pipeline, bool is_indexed,
    std::array<StageResources, NUM_PIPELINE_STAGES>& out, bool descriptors_synced,
    VtgStageBindings* vtg_bindings) {
    using Maxwell = Tegra::Engines::Maxwell3D::Regs;

    const auto& regs = maxwell3d->regs;
    const bool via_header_index =
        regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding;

    auto& views = m_scratch_views;
    auto& samplers = m_scratch_samplers;
    views.clear();
    samplers.clear();

    std::scoped_lock lock{m_texture_cache.mutex, m_buffer_cache.mutex};

    if (!descriptors_synced) {
        m_texture_cache.SynchronizeGraphicsDescriptors();
    }
    m_buffer_cache.SetUniformBuffersState(pipeline.EnabledUniformBufferMasks(),
                                          &pipeline.UniformBufferSizes());

    const auto for_each_stage = [&](auto&& func) {
        for (size_t index = 0; index < NUM_PIPELINE_STAGES; ++index) {
            const auto metal_stage = static_cast<PipelineStage>(index);
            if (const Shader::Info* info = pipeline.StageInfo(metal_stage)) {
                func(index, MAXWELL_STAGE_INDEX[index], *info);
            }
        }
    };

    m_sampler_cache.Reset();
    const bool memoize_samplers = m_sampler_cache.Enabled();
    const auto lookup_sampler = [&](u32 index) {
        return m_texture_cache.GetGraphicsSamplerId(index);
    };

    // Pixel local texture reads of the fragment stage: the view each one samples.
    struct AttachmentRead {
        size_t view;
        Shader::PixelLocalRead kind;
    };
    boost::container::small_vector<AttachmentRead, 4> attachment_reads;
    constexpr size_t FRAGMENT_STAGE = static_cast<size_t>(PipelineStage::Fragment);
    constexpr size_t VERTEX_STAGE = static_cast<size_t>(PipelineStage::Vertex);
    constexpr u32 MAX_CACHED_ARRAY_COUNT = 16384;

    const u64 current_tick = m_scheduler.CurrentTick();
    const u64 pre_fill_view_gen = m_texture_cache_runtime.ViewGeneration();
    const bool global_rescaling = m_texture_cache.IsRescaling();
    const bool has_vtg = pipeline.VtgParams() != nullptr;

    struct LargeDescRecord {
        LargeSet0ArrayCacheEntry* hit_entry{};
        u32 pushed_views{};
        bool can_cache{};
        GPUVAddr cbuf_addr{};
        const u8* raw_cbuf_ptr{};
        u32 handle_or_const{};
    };
    boost::container::small_vector<LargeDescRecord, 32> large_desc_records;
    std::array<size_t, NUM_PIPELINE_STAGES> stage_view_end{};

    for_each_stage([&](size_t metal_index, size_t stage, const Shader::Info& info) {
        m_buffer_cache.UnbindGraphicsStorageBuffers(stage);
        u32 ssbo_index = 0;
        for (const auto& desc : info.storage_buffers_descriptors) {
            m_buffer_cache.BindGraphicsStorageBuffer(stage, ssbo_index, desc.cbuf_index,
                                                     desc.cbuf_offset, desc.is_written,
                                                     desc.untracked_writes_only);
            ++ssbo_index;
        }

        const auto& cbufs = maxwell3d->state.shader_stages[stage].const_buffers;
        Vulkan::DescriptorCbufReader<18> cbuf_reader;
        const auto bind = [&](u32 bank, u32 count) {
            using Bound = decltype(cbuf_reader.Bind(*gpu_memory, 0, 0, 0, 0));
            if (bank >= cbufs.size()) {
                return std::optional<Bound>{};
            }
            return std::optional<Bound>{cbuf_reader.Bind(*gpu_memory, bank, cbufs[bank].address,
                                                         static_cast<u32>(cbufs[bank].size),
                                                         count)};
        };
        const auto add_views = [&](const auto& descriptors, bool images) {
            for (const auto& desc : descriptors) {
                bool blacklist = false;
            if constexpr (requires { desc.is_written; }) {
                blacklist = images && desc.is_written;
            }
                ForEachTextureHandle(desc, bind, via_header_index, [&](auto handle) {
                    views.push_back({.index = handle.first, .blacklist = blacklist});
                });
            }
        };

        add_views(info.texture_buffer_descriptors, false);
        add_views(info.image_buffer_descriptors, false);
        const bool track_stage_rescaling =
            global_rescaling && (!has_vtg || info.uses_rescaling_uniform);
        for (const auto& desc : info.texture_descriptors) {
            if (desc.is_direct) {
                for (u32 index = 0; index < desc.count; ++index) {
                    views.push_back({.index = index, .is_table = true});
                }
                large_desc_records.push_back(LargeDescRecord{.pushed_views = desc.count});
                continue;
            }
            if (metal_index == FRAGMENT_STAGE && info.input_attachment_mask != 0 &&
                desc.pixel_local_read != Shader::PixelLocalRead::None) {
                attachment_reads.push_back({views.size(), desc.pixel_local_read});
            }
            if (metal_index != VERTEX_STAGE && desc.count >= 64 &&
                desc.count <= MAX_CACHED_ARRAY_COUNT && desc.sources.empty() &&
                !desc.has_index_cbuf) {
                const auto primary = bind(desc.cbuf_index, desc.count);
                const u64 needed_bytes =
                    static_cast<u64>(desc.count - 1u) * desc.stride + sizeof(u32);
                if (primary && primary->span != nullptr && desc.cbuf_offset <= primary->size &&
                    needed_bytes <= primary->size - desc.cbuf_offset) {
                    u32 secondary = 0;
                    if (desc.has_secondary) {
                        if (const auto sec_buf = bind(desc.secondary_cbuf_index, 1)) {
                            secondary = sec_buf->Read(desc.secondary_cbuf_offset);
                        }
                    }
                    const u32 handle_or_const =
                        (desc.has_secondary
                             ? ((secondary << desc.secondary_shift_left) & desc.secondary_mask)
                             : 0u) |
                        desc.handle_or_bits;
                    const u8* const raw_ptr = primary->span + desc.cbuf_offset;

                    LargeSet0ArrayCacheEntry* hit = nullptr;
                    for (LargeSet0ArrayCacheEntry& entry : m_large_set0_cache) {
                        if (entry.tick != current_tick ||
                            entry.view_generation != pre_fill_view_gen ||
                            entry.cbuf_addr != primary->address ||
                            entry.cbuf_offset != desc.cbuf_offset || entry.stride != desc.stride ||
                            entry.count != desc.count || entry.shift_left != desc.shift_left ||
                            entry.mask != desc.mask || entry.handle_or_const != handle_or_const ||
                            entry.type != desc.type ||
                            entry.is_multisample != desc.is_multisample ||
                            entry.via_header_index != via_header_index ||
                            entry.is_rescaling != track_stage_rescaling ||
                            entry.raw_handles.size() != desc.count) {
                            continue;
                        }
                        bool raw_match = true;
                        if (desc.stride == sizeof(u32)) {
                            raw_match = std::memcmp(entry.raw_handles.data(), raw_ptr,
                                                    static_cast<size_t>(desc.count) *
                                                        sizeof(u32)) == 0;
                        } else {
                            const u32* const cached = entry.raw_handles.data();
                            const u8* p = raw_ptr;
                            const size_t stride = desc.stride;
                            for (u32 e = 0; e < desc.count; ++e, p += stride) {
                                u32 v = 0;
                                std::memcpy(&v, p, sizeof(u32));
                                if (cached[e] != v) {
                                    raw_match = false;
                                    break;
                                }
                            }
                        }
                        if (raw_match) {
                            hit = &entry;
                            break;
                        }
                    }
                    if (hit != nullptr) {
                        // Push only the deduplicated unique TIC/TSC handles so
                        // FillGraphicsImageViews, CheckFeedbackLoop, and GetGraphicsSamplerId
                        // still observe every silent guest TIC/TSC or texture update.
                        for (const u32 tic_idx : hit->unique_tic_indices) {
                            views.push_back({tic_idx});
                        }
                        for (const u32 tsc_idx : hit->unique_tsc_indices) {
                            samplers.push_back(memoize_samplers
                                                   ? m_sampler_cache.Get(tsc_idx, lookup_sampler)
                                                   : lookup_sampler(tsc_idx));
                        }
                        large_desc_records.push_back(LargeDescRecord{
                            .hit_entry = hit,
                            .pushed_views = static_cast<u32>(hit->unique_tic_indices.size()),
                            .can_cache = true,
                            .cbuf_addr = primary->address,
                            .raw_cbuf_ptr = raw_ptr,
                            .handle_or_const = handle_or_const,
                        });
                        continue;
                    }
                    // Miss on an eligible large cbuf array: decode handles directly from raw_ptr.
                    for (u32 element = 0; element < desc.count; ++element) {
                        u32 raw_val = 0;
                        std::memcpy(&raw_val, raw_ptr + static_cast<size_t>(element) * desc.stride,
                                    sizeof(u32));
                        const u32 composed =
                            ((raw_val << desc.shift_left) & desc.mask) | handle_or_const;
                        const auto handle =
                            Tegra::Texture::TexturePair(composed, via_header_index);
                        views.push_back({handle.first});
                        samplers.push_back(memoize_samplers
                                               ? m_sampler_cache.Get(handle.second, lookup_sampler)
                                               : lookup_sampler(handle.second));
                    }
                    large_desc_records.push_back(LargeDescRecord{
                        .hit_entry = nullptr,
                        .pushed_views = desc.count,
                        .can_cache = true,
                        .cbuf_addr = primary->address,
                        .raw_cbuf_ptr = raw_ptr,
                        .handle_or_const = handle_or_const,
                    });
                    continue;
                }
            }
            ForEachTextureHandle(desc, bind, via_header_index, [&](auto handle) {
                views.push_back({handle.first});
                samplers.push_back(memoize_samplers
                                       ? m_sampler_cache.Get(handle.second, lookup_sampler)
                                       : lookup_sampler(handle.second));
            });
            large_desc_records.push_back(LargeDescRecord{.pushed_views = desc.count});
        }
        add_views(info.image_descriptors, true);
        stage_view_end[metal_index] = views.size();
    });

    m_texture_cache.FillGraphicsImageViews<true>(std::span(views.data(), views.size()));

    if (!attachment_reads.empty()) {
        out[FRAGMENT_STAGE].reads_attachments = true;
        out[FRAGMENT_STAGE].attachment_reads = SelectAttachmentReads(
            *pipeline.StageInfo(PipelineStage::Fragment), attachment_reads,
            std::span(views.data(), views.size()),
            AliasScratch(m_render_pass.num_color, WrittenColorAttachments(pipeline)));
    }

    // A draw sampling a texture that is also an attachment of the open pass must
    // end the pass first: on tile-based hardware the attachment is still in tile
    // memory, so sampling it returns pre-pass contents.
    m_texture_cache.CheckFeedbackLoop(
        std::span<const VideoCommon::ImageViewInOut>(views.data(), views.size()));

    {
        size_t texel_view_it = 0;
        for_each_stage([&](size_t metal_index, size_t stage, const Shader::Info& info) {
            m_buffer_cache.UnbindGraphicsTextureBuffers(stage);
            size_t index = 0;
            const auto bind = [&](u32 count, bool is_image, bool is_written,
                                  Shader::ImageFormat declared) {
                for (u32 element = 0; element < count; ++element) {
                    const VideoCommon::ImageViewId id = views[texel_view_it++].id;
                    if (id) {
                        ImageView& view = m_texture_cache.GetImageView(id);
                        m_buffer_cache.BindGraphicsTextureBuffer(
                            stage, index, view.gpu_addr, view.BufferSize(),
                            TexelBufferFormat(view.format, is_image, declared), is_written,
                            is_image);
                        } else {
                        // Unreachable today, see the compute path's copy of this.
                        static std::atomic<u32> reported{0};
                        if (reported++ < 4) {
                            LOG_ERROR(Render_Metal,
                                          "Texel buffer slot {} has no view; later bindings will shift",
                                          index);
                        }
                    }

                    ++index;
                }
            };
            for (const auto& desc : info.texture_buffer_descriptors) {
                bind(desc.count, false, false, Shader::ImageFormat::Typeless);
            }
            for (const auto& desc : info.image_buffer_descriptors) {
                bind(desc.count, true, desc.is_written, desc.format);
            }
            texel_view_it = stage_view_end[metal_index];
        });
    }

    // Before the index buffer is bound: Metal's restart index is fixed, and a
    // guest using a different one needs the buffer rewritten.
    m_buffer_cache_runtime.SetPrimitiveRestart(regs.primitive_restart.enabled != 0,
                                               regs.primitive_restart.index,
                                               maxwell3d->draw_manager->GetDrawState().topology);
    m_buffer_cache.UpdateGraphicsBuffers(is_indexed);
    m_buffer_cache.BindHostGeometryBuffers(is_indexed);

    const VtgEmulation* const vtg = pipeline.VtgParams();
    boost::container::small_vector<u64, 3 + Shader::VtgDrawInfo::NUM_ATTRIBUTES> vtg_vertex_ssbos;
    std::array<u64, 4> vtg_geometry_ssbos{};
    u64 vtg_geometry_size = 0;

    if (vtg != nullptr && vtg_bindings != nullptr) {
        using Shader::VTG_LOCAL_SIZE;
        namespace DrawInfo = Shader::VtgDrawInfo;
        static constexpr u32 MAX_INVOCATIONS = 1U << 20;
        static constexpr u64 MAX_SCRATCH_SIZE = 256ULL << 20;

        const u32 verts_per_prim = vtg->geometry_bindings.vertices_per_primitive;
        const u32 indices_per_prim = vtg->geometry_bindings.indices_per_primitive;
        const auto scratch_fits = [&](u64 vertices, u64 primitives) {
            return vertices <= MAX_INVOCATIONS && primitives <= MAX_INVOCATIONS &&
                   (vertices * vtg->vertex_stride +
                    primitives *
                        (u64{verts_per_prim} * vtg->geometry_stride + indices_per_prim)) *
                           sizeof(u32) <=
                       MAX_SCRATCH_SIZE;
        };

        u32 vertex_count = 0;
        u32 instance_count = 0;
        u32 primitive_count = 0;
        u64 total_vertices = 0;
        u64 total_primitives = 0;
        if (vtg_bindings->is_indirect) {
            const auto [args_buf, args_off] = m_buffer_cache.GetDrawIndirectBuffer();
            if (args_buf == nullptr || args_buf->Handle() == nullptr) {
                vtg_bindings->runs = false;
                return;
            }
            vtg_bindings->indirect_args_buffer = args_buf->Handle();
            vtg_bindings->indirect_args_offset = args_off;

            u32 requested = 0;
            if (vtg->feedback_buffer && vtg->feedback_buffer->contents() != nullptr) {
                auto* const words = static_cast<u32*>(vtg->feedback_buffer->contents());
                requested = std::atomic_ref<u32>(words[0]).load(std::memory_order_relaxed);
            }
            const u64 largest = (std::max<u64>)(vtg->largest_draw, requested);
            vtg->indirect_capacity = static_cast<u32>((std::min<u64>)(
                (std::max<u64>)(vtg->indirect_capacity, 2 * std::bit_ceil(largest)),
                MAX_INVOCATIONS));
            while (vtg->indirect_capacity > VTG_LOCAL_SIZE &&
                   !scratch_fits(vtg->indirect_capacity, vtg->indirect_capacity)) {
                vtg->indirect_capacity /= 2;
            }
            vtg_bindings->indirect_capacity = vtg->indirect_capacity;
            total_vertices = vtg->indirect_capacity;
            total_primitives = vtg->indirect_capacity;
        } else {
            vertex_count = vtg_bindings->vertex_count;
            instance_count = vtg_bindings->instance_count;
            primitive_count = Shader::VtgPrimitiveCount(vtg->topology, vertex_count);
            total_vertices = u64{vertex_count} * instance_count;
            total_primitives = u64{primitive_count} * instance_count;
            if (!vtg_bindings->is_indexed) {
                vtg->largest_draw = (std::max)(vtg->largest_draw, total_vertices);
                vtg->has_largest_draw = true;
                if (vtg->feedback_buffer && vtg->feedback_buffer->contents() != nullptr) {
                    auto* const words = static_cast<u32*>(vtg->feedback_buffer->contents());
                    const u32 requested =
                        std::atomic_ref<u32>(words[0]).load(std::memory_order_relaxed);
                    if (requested > vtg->largest_draw) {
                        vtg->largest_draw = requested;
                    }
                    // Clear the truncation flag now that largest_draw has absorbed the
                    // fallback/feedback size so subsequent indirect draws stay on the GPU.
                    std::atomic_ref<u32>(words[1]).store(0u, std::memory_order_relaxed);
                }
            }
        }

        const bool fits = scratch_fits(total_vertices, total_primitives);
        if (!fits) {
            LOG_WARNING(Render_Metal,
                        "VTG draw of {} vertices and {} instances is too large to run as compute",
                        vertex_count, instance_count);
        }
        if ((!vtg_bindings->is_indirect && total_primitives == 0) || !fits) {
            vtg_bindings->runs = false;
            return;
        }

        const u32 vertex_invocations =
            Common::AlignUp(static_cast<u32>(total_vertices), VTG_LOCAL_SIZE);
        const u32 geometry_invocations =
            Common::AlignUp(static_cast<u32>(total_primitives), VTG_LOCAL_SIZE);
        const u32 output_vertices = geometry_invocations * verts_per_prim;
        const u32 output_indices = geometry_invocations * indices_per_prim;
        const bool emits_points =
            vtg->geometry_bindings.output_topology == Shader::OutputTopology::PointList;

        const u64 vertex_size = u64{vertex_invocations} * vtg->vertex_stride * sizeof(u32);
        const u64 geometry_size =
            u64{output_vertices + 1} * vtg->geometry_stride * sizeof(u32);
        const u64 index_size = u64{output_indices} * sizeof(u32);
        vtg_geometry_size = geometry_size;

        const StagingBufferRef info_ref =
            m_staging_pool.Request(DrawInfo::NUM_WORDS * sizeof(u32), MemoryUsage::Upload);
        const StagingBufferRef vertex_out =
            m_staging_pool.Request(vertex_size, MemoryUsage::DeviceLocal);
        const StagingBufferRef geometry_out = m_staging_pool.Request(
            geometry_size, emits_points ? MemoryUsage::Upload : MemoryUsage::DeviceLocal);
        const StagingBufferRef index_out = m_staging_pool.Request(
            index_size, emits_points ? MemoryUsage::Upload : MemoryUsage::DeviceLocal);
        if (info_ref.buffer == nullptr || vertex_out.buffer == nullptr ||
            geometry_out.buffer == nullptr || index_out.buffer == nullptr) {
            LOG_WARNING(Render_Metal, "VTG scratch allocation failed ({} / {} / {} bytes)",
                        vertex_size, geometry_size, index_size);
            vtg_bindings->runs = false;
            return;
        }

        constexpr u64 SSBO_ALIGN = 16;
        const u64 null_addr = m_buffer_cache_runtime.GetNullBufferAddress();
        const auto guest_addr = [&](const std::tuple<Buffer*, u32, u32>& binding,
                                    u32& misalignment) -> u64 {
            const auto [buf, off, sz] = binding;
            misalignment = 0;
            if (buf == nullptr || buf->Handle() == nullptr) {
                return null_addr != 0 ? null_addr : info_ref.gpu_address;
            }
            const u64 aligned = Common::AlignDown(u64{off}, SSBO_ALIGN);
            misalignment = static_cast<u32>(off - aligned);
            return buf->GpuAddress() + aligned;
        };

        std::array<u32, Maxwell::NumVertexArrays> misalignments{};
        u32 index_misalignment = 0;
        vtg_vertex_ssbos.push_back(info_ref.gpu_address);
        vtg_vertex_ssbos.push_back(vertex_out.gpu_address);
        vtg_vertex_ssbos.push_back(
            vtg_bindings->is_indexed
                ? guest_addr(m_buffer_cache.IndexBufferBinding(), index_misalignment)
                : info_ref.gpu_address);
        for (const auto& [stream, binding_idx] : vtg->vertex_bindings.vertex_buffers) {
            if (vtg->uses_constant_attributes && stream == vtg->constant_binding) {
                misalignments[stream] = 0;
                vtg_vertex_ssbos.push_back(null_addr != 0 ? null_addr : info_ref.gpu_address);
            } else {
                vtg_vertex_ssbos.push_back(
                    guest_addr(m_buffer_cache.VertexBufferBinding(stream), misalignments[stream]));
            }
        }

        std::array<u32, DrawInfo::NUM_WORDS> words{};
        const u32 index_bytes = vtg_bindings->index_bytes;
        words[DrawInfo::VERTEX_COUNT] = vertex_count;
        words[DrawInfo::INSTANCE_COUNT] = std::max(instance_count, 1U);
        words[DrawInfo::BASE_VERTEX] = vtg_bindings->base_vertex;
        words[DrawInfo::BASE_INSTANCE] = vtg_bindings->base_instance;
        words[DrawInfo::INDEX_SIZE] = index_bytes;
        words[DrawInfo::INDEX_OFFSET] =
            index_misalignment +
            (vtg_bindings->is_indexed ? vtg_bindings->first_index * index_bytes : 0);
        words[DrawInfo::PRIMITIVE_COUNT] = primitive_count;
        words[DrawInfo::INDEX_MASK] =
            index_bytes == 1 ? 0xffU : (index_bytes == 2 ? 0xffffU : 0xffffffffU);
        for (size_t index = 0; index < DrawInfo::NUM_ATTRIBUTES; ++index) {
            const auto& attribute = regs.vertex_attrib_format[index];
            u32* const attribute_words =
                &words[DrawInfo::ATTRIBUTES + index * DrawInfo::ATTRIBUTE_WORDS];
            if (attribute.constant != 0) {
                attribute_words[DrawInfo::ATTRIBUTE_OFFSET] = 0;
                attribute_words[DrawInfo::ATTRIBUTE_STRIDE] = 0;
                attribute_words[DrawInfo::ATTRIBUTE_INSTANCED] = 0;
                attribute_words[DrawInfo::ATTRIBUTE_DIVISOR] = 0;
            } else {
                const u32 stream = attribute.buffer;
                const u32 frequency = regs.vertex_streams[stream].frequency;
                const bool instanced =
                    regs.vertex_stream_instances.IsInstancingEnabled(stream) && frequency != 0;
                attribute_words[DrawInfo::ATTRIBUTE_OFFSET] =
                    misalignments[stream] + attribute.offset;
                attribute_words[DrawInfo::ATTRIBUTE_STRIDE] = regs.vertex_streams[stream].stride;
                attribute_words[DrawInfo::ATTRIBUTE_INSTANCED] = instanced ? 1 : 0;
                attribute_words[DrawInfo::ATTRIBUTE_DIVISOR] = instanced ? frequency : 0;
            }
        }
        std::memcpy(info_ref.mapped_span.data(), words.data(), sizeof(words));
        MemoryAllocator::FlushRange(info_ref.buffer, info_ref.offset, sizeof(words));

        if (emits_points) {
            // Unwritten point indices reference the NaN sentinel vertex so they clip away.
            auto* const idx_dst = reinterpret_cast<u32*>(index_out.mapped_span.data());
            std::fill_n(idx_dst, output_indices, output_vertices);
            MemoryAllocator::FlushRange(index_out.buffer, index_out.offset, index_size);

            constexpr u32 QUIET_NAN = 0x7fc00000U;
            const u64 sentinel_offset = u64{output_vertices} * vtg->geometry_stride * sizeof(u32);
            const u64 sentinel_size = u64{vtg->geometry_stride} * sizeof(u32);
            auto* const sent_dst =
                reinterpret_cast<u32*>(geometry_out.mapped_span.data() + sentinel_offset);
            std::fill_n(sent_dst, vtg->geometry_stride, QUIET_NAN);
            MemoryAllocator::FlushRange(geometry_out.buffer,
                                        geometry_out.offset + sentinel_offset, sentinel_size);
        }

        vtg_geometry_ssbos = {
            info_ref.gpu_address,
            vertex_out.gpu_address,
            geometry_out.gpu_address,
            index_out.gpu_address,
        };

        vtg_bindings->runs = true;
        vtg_bindings->vertex_invocations = vertex_invocations;
        vtg_bindings->geometry_invocations = geometry_invocations;
        vtg_bindings->output_vertices = output_vertices;
        vtg_bindings->output_indices = output_indices;
        vtg_bindings->total_primitives = static_cast<u32>(total_primitives);
        vtg_bindings->info_buffer = info_ref;
        vtg_bindings->vertex_out = vertex_out;
        vtg_bindings->geometry_out = geometry_out;
        vtg_bindings->index_out = index_out;
        vtg_bindings->index_size = index_size;
        vtg_bindings->emits_points = emits_points;
    }

    const bool is_rescaling = m_texture_cache.IsRescaling();
    RescalingMaskBuilder shared_rescaling{};
    bool any_non_vtg_rescaling = false;

    size_t view_it = 0;
    size_t sampler_it = 0;
    size_t large_rec_it = 0;
    for_each_stage([&](size_t metal_index, size_t stage, const Shader::Info& info) {
        out[metal_index].uses_rescaling = info.uses_rescaling_uniform;
        out[metal_index].rescaling_masks.fill(0);
        out[metal_index].reads_gpu_texture = false;
        const bool is_vertex_stage = metal_index == static_cast<size_t>(PipelineStage::Vertex);
        // Non-VTG Vertex and Fragment share one EmitSPIRV Bindings counter; VTG
        // stages each compile with their own zero-based Bindings counter.
        RescalingMaskBuilder stage_rescaling{};
        RescalingMaskBuilder& active_rescaling =
            vtg == nullptr ? shared_rescaling : stage_rescaling;
        const bool track_stage_rescaling =
            is_rescaling && (vtg == nullptr || info.uses_rescaling_uniform);
        if (vtg == nullptr && info.uses_rescaling_uniform) {
            any_non_vtg_rescaling = true;
        }

        m_argument_buffer.Reset();
        m_argument_buffer.Reserve(
            pipeline.ExpectedArgumentSlots(static_cast<PipelineStage>(metal_index)));
        m_buffer_cache_runtime.ClearPendingBindings();

        m_buffer_cache.BindHostStageBuffers(stage);
        const std::span<const BufferBinding> pending = m_buffer_cache_runtime.GetPendingBindings();

        for (const BufferBinding& bound : pending) {
            if (bound.gpu_address != 0) {
                m_argument_buffer.AddBufferAddress(bound.gpu_address + bound.offset);
            } else if (bound.is_written) {
                m_argument_buffer.AddStorageBuffer(bound.buffer, bound.offset, bound.size, true);
            } else {
                m_argument_buffer.AddUniformBuffer(bound.buffer, bound.offset, bound.size);
            }
        }

        if (vtg != nullptr) {
            if (metal_index == static_cast<size_t>(PipelineStage::Vertex)) {
                for (const u64 addr : vtg_vertex_ssbos) {
                    m_argument_buffer.AddBufferAddress(addr);
                }
            } else if (metal_index == static_cast<size_t>(PipelineStage::Geometry)) {
                for (const u64 addr : vtg_geometry_ssbos) {
                    m_argument_buffer.AddBufferAddress(addr);
                }
            }
        }

        const auto texture_of = [&](size_t view_index,
                                    Shader::TextureType type) -> MTL::Texture* {
            const VideoCommon::ImageViewId id = views[view_index].id;
            if (!id) {
                return nullptr;
            }
            return m_texture_cache.GetImageView(id).Handle(type);
        };

        const std::span<MTL::Texture* const> texel_views =
            m_buffer_cache_runtime.GetTexelViews();
        MTL::Texture* const null_texel = m_buffer_cache_runtime.GetNullTexelView();
        size_t texel_it = 0;
        const auto next_texel = [&]() -> MTL::Texture* {
            MTL::Texture* const view =
                texel_it < texel_views.size() ? texel_views[texel_it++] : nullptr;
            return view != nullptr ? view : null_texel;
        };
        for (const auto& desc : info.texture_buffer_descriptors) {
            for (u32 element = 0; element < desc.count; ++element) {
                ++view_it;
                // Read view, then the view with the guest's size.
                m_argument_buffer.AddImage(next_texel(), false);
                m_argument_buffer.AddImage(next_texel(), false);
            }
        }
        for (const auto& desc : info.image_buffer_descriptors) {
            for (u32 element = 0; element < desc.count; ++element) {
                ++view_it;
                m_argument_buffer.AddImage(next_texel(), desc.is_written);
                m_argument_buffer.AddImage(next_texel(), desc.is_written);
            }
        }
        const ImageView& null_view = m_texture_cache.GetImageView(VideoCommon::NULL_IMAGE_VIEW_ID);
        struct DirectTextureEntry {
            const Shader::TextureDescriptor* desc;
            size_t start_view;
        };
        boost::container::small_vector<DirectTextureEntry, 4> direct_textures;
        // Pre-scan direct table descriptors so a cache hit skips both the
        // rescaling loop and the Set 1 argument buffer build/hash.
        const DirectTableCacheEntry* cached_table = nullptr;
        u64 table_signature = 0x9E3779B97F4A7C15ull ^ info.runtime_sampler_count;
        u32 total_direct_views = 0;
        {
            size_t scan_view = view_it;
            size_t scan_rec = large_rec_it;
            for (const auto& desc : info.texture_descriptors) {
                if (desc.is_direct) {
                    direct_textures.push_back(DirectTextureEntry{&desc, scan_view});
                    total_direct_views += desc.count;
                    table_signature ^=
                        (static_cast<u64>(desc.is_multisample) << 40) |
                        (static_cast<u64>(desc.type) << 32) | static_cast<u64>(desc.count);
                    table_signature *= 0xBF58476D1CE4E5B9ull;
                }
                scan_view += large_desc_records[scan_rec++].pushed_views;
            }
        }
        const u64 view_gen = m_texture_cache_runtime.ViewGeneration();
        if (!direct_textures.empty() || info.runtime_sampler_count != 0) {
            for (const DirectTableCacheEntry& entry : m_direct_table_cache) {
                if (entry.is_compute || entry.tick != current_tick ||
                    entry.view_generation != view_gen ||
                    entry.signature != table_signature ||
                    entry.is_rescaling != track_stage_rescaling ||
                    entry.total_views != total_direct_views ||
                    entry.runtime_sampler_count != info.runtime_sampler_count ||
                    entry.table_buffer.buffer == nullptr) {
                    continue;
                }
                bool match = true;
                size_t check_idx = 0;
                for (const auto& [desc_ptr, start_view] : direct_textures) {
                    const u32 count = desc_ptr->count;
                    for (u32 e = 0; e < count; ++e) {
                        if (entry.view_ids[check_idx++] != views[start_view + e].id) {
                            match = false;
                            break;
                        }
                    }
                    if (!match) {
                        break;
                    }
                }
                if (match) {
                    for (u32 s = 0; s < info.runtime_sampler_count; ++s) {
                        if (entry.sampler_ids[s] != m_texture_cache.GetGraphicsSamplerId(s)) {
                            match = false;
                            break;
                        }
                    }
                }
                if (match) {
                    cached_table = &entry;
                    break;
                }
            }
        }

        u32 direct_rescaled_mask = 0;
        u32 direct_idx = 0;
        VideoCommon::SamplerId last_sampler_id{UINT32_MAX};
        const Sampler* last_sampler_ptr = nullptr;
        u64 last_default_sampler_res_id = 0;
        boost::container::small_vector<u64, 64> array_samplers;
        for (const auto& desc : info.texture_descriptors) {
            const LargeDescRecord& large_rec = large_desc_records[large_rec_it++];
            bool desc_rescaled = false;
            if (desc.is_direct) {
                if (cached_table != nullptr && !is_vertex_stage) {
                    view_it += desc.count;
                    if (track_stage_rescaling) {
                        const bool bit =
                            direct_idx < 32 && ((cached_table->rescaled_mask >> direct_idx) & 1u) != 0;
                        active_rescaling.PushTexture(bit);
                    }
                    ++direct_idx;
                    continue;
                }
                for (u32 element = 0; element < desc.count; ++element) {
                    const VideoCommon::ImageViewId view_id = views[view_it++].id;
                    if (view_id) {
                        const ImageView& view = m_texture_cache.GetImageView(view_id);
                        if (is_vertex_stage && m_texture_cache.IsImageGpuModified(view.image_id)) {
                            out[metal_index].reads_gpu_texture = true;
                        }
                        if (track_stage_rescaling && !view.IsIntegerFormat()) {
                            desc_rescaled |= m_texture_cache.IsRescaling(view);
                        }
                    }
                }
                if (desc_rescaled && direct_idx < 32) {
                    direct_rescaled_mask |= 1u << direct_idx;
                }
                ++direct_idx;
                if (track_stage_rescaling) {
                    active_rescaling.PushTexture(desc_rescaled);
                }
                continue;
            }
            if (large_rec.hit_entry != nullptr) {
                LargeSet0ArrayCacheEntry& entry = *large_rec.hit_entry;
                const size_t u_tic = entry.unique_tic_indices.size();
                const size_t u_tsc = entry.unique_tsc_indices.size();
                bool match = entry.view_generation == view_gen;
                if (match) {
                    for (size_t u = 0; u < u_tic; ++u) {
                        if (views[view_it + u].id != entry.unique_view_ids[u]) {
                            match = false;
                            break;
                        }
                    }
                }
                if (match) {
                    for (size_t u = 0; u < u_tsc; ++u) {
                        if (samplers[sampler_it + u] != entry.unique_sampler_ids[u]) {
                            match = false;
                            break;
                        }
                    }
                }
                if (match) {
                    m_argument_buffer.AddRawSlots(entry.slots, entry.slots_hash);
                    view_it += u_tic;
                    sampler_it += u_tsc;
                    if (track_stage_rescaling) {
                        active_rescaling.PushTexture(entry.desc_rescaled);
                    }
                    continue;
                }
                // Silent guest TIC/TSC edit or view generation bump: rebuild entry.slots
                // directly from the freshly-resolved unique views & samplers without
                // re-reading cbuf memory or re-running FillGraphicsImageViews.
                const u64 null_tex_id = null_view.ResourceId(desc.type, desc.is_multisample);
                bool any_volume_copy = false;
                for (size_t u = 0; u < u_tic; ++u) {
                    entry.unique_view_ids[u] = views[view_it + u].id;
                }
                for (size_t u = 0; u < u_tsc; ++u) {
                    entry.unique_sampler_ids[u] = samplers[sampler_it + u];
                }
                view_it += u_tic;
                sampler_it += u_tsc;
                entry.slots.resize(static_cast<size_t>(desc.count) * 2u);
                for (u32 element = 0; element < desc.count; ++element) {
                    const VideoCommon::ImageViewId view_id =
                        entry.unique_view_ids[entry.elem_to_unique_tic[element]];
                    const VideoCommon::SamplerId sampler_id =
                        entry.unique_sampler_ids[entry.elem_to_unique_tsc[element]];
                    if (sampler_id != last_sampler_id) {
                        last_sampler_id = sampler_id;
                        last_sampler_ptr = &m_texture_cache.GetSampler(sampler_id);
                        last_default_sampler_res_id =
                            last_sampler_ptr->ResourceId(last_sampler_ptr->Handle());
                    }
                    u64 texture_id = null_tex_id;
                    u64 sampler_id_value = last_default_sampler_res_id;
                    if (view_id) {
                        const ImageView& view = m_texture_cache.GetImageView(view_id);
                        if (track_stage_rescaling) {
                            desc_rescaled |= m_texture_cache.IsRescaling(view);
                        }
                        any_volume_copy |= view.HasVolumeCopy();
                        const u64 resolved_id = view.ResourceId(desc.type, desc.is_multisample);
                        if (resolved_id != 0) {
                            texture_id = resolved_id;
                            sampler_id_value =
                                last_sampler_ptr->ResourceId(SamplerFor(*last_sampler_ptr, view));
                        }
                    }
                    entry.slots[element] = texture_id;
                    entry.slots[static_cast<size_t>(desc.count) + element] =
                        texture_id != 0 ? sampler_id_value : 0;
                }
                entry.slots_hash = Common::CityHash64(
                    reinterpret_cast<const char*>(entry.slots.data()),
                    entry.slots.size() * sizeof(u64));
                entry.desc_rescaled = desc_rescaled;
                m_argument_buffer.AddRawSlots(entry.slots, entry.slots_hash);
                if (!any_volume_copy) {
                    entry.view_generation = m_texture_cache_runtime.ViewGeneration();
                } else {
                    entry.tick = 0;
                    entry.count = 0;
                }
                if (track_stage_rescaling) {
                    active_rescaling.PushTexture(desc_rescaled);
                }
                continue;
            }
            const size_t array_start_view = view_it;
            const size_t array_start_sampler = sampler_it;
            const u64 null_tex_id = null_view.ResourceId(desc.type, desc.is_multisample);
            bool any_volume_copy = false;
            LargeSet0ArrayCacheEntry* fill_slot = nullptr;
            // SPIRV-Cross lays out combined sampled-image arrays in Tier 2 argument
            // buffers as two contiguous arrays: `array<texture, N> [[id(b)]]` followed
            // by `array<sampler, N> [[id(b + N)]]`, NOT interleaved (tex_i, smplr_i).
            const bool is_array = desc.count > 1;
            if (large_rec.can_cache) {
                for (size_t tries = 0; tries < m_large_set0_cache.size(); ++tries) {
                    LargeSet0ArrayCacheEntry* candidate =
                        &m_large_set0_cache[m_large_set0_cache_next++ &
                                            (m_large_set0_cache.size() - 1)];
                    bool in_use = false;
                    for (size_t r = large_rec_it; r < large_desc_records.size(); ++r) {
                        if (large_desc_records[r].hit_entry == candidate) {
                            in_use = true;
                            break;
                        }
                    }
                    if (!in_use) {
                        fill_slot = candidate;
                        break;
                    }
                }
                if (fill_slot != nullptr) {
                    fill_slot->slots.resize(static_cast<size_t>(desc.count) * 2u);
                }
            }
            if (fill_slot == nullptr && is_array) {
                array_samplers.clear();
                array_samplers.reserve(desc.count);
            }
            for (u32 element = 0; element < desc.count; ++element) {
                const VideoCommon::ImageViewId view_id = views[view_it++].id;
                const VideoCommon::SamplerId sampler_id = samplers[sampler_it++];
                // Resource IDs come cached from the view and the sampler: asking the
                // Metal objects is an Objective-C call per bound texture.
                // NULL_SAMPLER_ID (slot 0) is a valid default sampler; never write 0
                // into a Metal 4 argument buffer, or sampling through it hangs the GPU.
                if (sampler_id != last_sampler_id) {
                    last_sampler_id = sampler_id;
                    last_sampler_ptr = &m_texture_cache.GetSampler(sampler_id);
                    last_default_sampler_res_id =
                        last_sampler_ptr->ResourceId(last_sampler_ptr->Handle());
                }
                u64 texture_id = null_tex_id;
                u64 sampler_id_value = last_default_sampler_res_id;
                if (view_id) {
                    const ImageView& view = m_texture_cache.GetImageView(view_id);
                    if (is_vertex_stage && m_texture_cache.IsImageGpuModified(view.image_id)) {
                        out[metal_index].reads_gpu_texture = true;
                    }
                    if (track_stage_rescaling) {
                        desc_rescaled |= m_texture_cache.IsRescaling(view);
                    }
                    if (fill_slot != nullptr) {
                        any_volume_copy |= view.HasVolumeCopy();
                    }
                    const u64 resolved_id = view.ResourceId(desc.type, desc.is_multisample);
                    if (resolved_id != 0) {
                        texture_id = resolved_id;
                        sampler_id_value =
                            last_sampler_ptr->ResourceId(SamplerFor(*last_sampler_ptr, view));
                    }
                }
                if (fill_slot != nullptr) {
                    fill_slot->slots[element] = texture_id;
                    fill_slot->slots[static_cast<size_t>(desc.count) + element] =
                        texture_id != 0 ? sampler_id_value : 0;
                } else if (is_array) {
                    m_argument_buffer.AddSlot(texture_id);
                    array_samplers.push_back(texture_id != 0 ? sampler_id_value : 0);
                } else {
                    m_argument_buffer.AddTextureIds(texture_id, sampler_id_value);
                }
            }
            if (fill_slot != nullptr) {
                fill_slot->slots_hash = Common::CityHash64(
                    reinterpret_cast<const char*>(fill_slot->slots.data()),
                    fill_slot->slots.size() * sizeof(u64));
                m_argument_buffer.AddRawSlots(fill_slot->slots, fill_slot->slots_hash);
                if (!any_volume_copy) {
                    fill_slot->tick = current_tick;
                    fill_slot->view_generation = m_texture_cache_runtime.ViewGeneration();
                    fill_slot->cbuf_addr = large_rec.cbuf_addr;
                    fill_slot->cbuf_offset = desc.cbuf_offset;
                    fill_slot->stride = desc.stride;
                    fill_slot->count = desc.count;
                    fill_slot->shift_left = desc.shift_left;
                    fill_slot->mask = desc.mask;
                    fill_slot->handle_or_const = large_rec.handle_or_const;
                    fill_slot->type = desc.type;
                    fill_slot->is_multisample = desc.is_multisample;
                    fill_slot->via_header_index = via_header_index;
                    fill_slot->is_rescaling = track_stage_rescaling;
                    fill_slot->desc_rescaled = desc_rescaled;
                    // Extract only the strided u32 handle words (not unrelated struct
                    // padding between elements) and build deduplicated unique TIC/TSC
                    // tables for O(U) re-verification on subsequent draws.
                    fill_slot->raw_handles.resize(desc.count);
                    fill_slot->unique_tic_indices.clear();
                    fill_slot->unique_tsc_indices.clear();
                    fill_slot->unique_view_ids.clear();
                    fill_slot->unique_sampler_ids.clear();
                    fill_slot->elem_to_unique_tic.resize(desc.count);
                    fill_slot->elem_to_unique_tsc.resize(desc.count);
                    std::unordered_map<u32, u16> tic_map;
                    std::unordered_map<u32, u16> tsc_map;
                    const u8* p = large_rec.raw_cbuf_ptr;
                    const size_t stride = desc.stride;
                    for (u32 e = 0; e < desc.count; ++e, p += stride) {
                        u32 raw_val = 0;
                        std::memcpy(&raw_val, p, sizeof(u32));
                        fill_slot->raw_handles[e] = raw_val;
                        const u32 composed =
                            ((raw_val << desc.shift_left) & desc.mask) | large_rec.handle_or_const;
                        const auto handle =
                            Tegra::Texture::TexturePair(composed, via_header_index);
                        auto [tit, tinserted] =
                            tic_map.emplace(handle.first,
                                            static_cast<u16>(fill_slot->unique_tic_indices.size()));
                        if (tinserted) {
                            fill_slot->unique_tic_indices.push_back(handle.first);
                            fill_slot->unique_view_ids.push_back(views[array_start_view + e].id);
                        }
                        fill_slot->elem_to_unique_tic[e] = tit->second;

                        auto [sit, sinserted] =
                            tsc_map.emplace(handle.second,
                                            static_cast<u16>(fill_slot->unique_tsc_indices.size()));
                        if (sinserted) {
                            fill_slot->unique_tsc_indices.push_back(handle.second);
                            fill_slot->unique_sampler_ids.push_back(
                                samplers[array_start_sampler + e]);
                        }
                        fill_slot->elem_to_unique_tsc[e] = sit->second;
                    }
                } else {
                    fill_slot->tick = 0;
                    fill_slot->count = 0;
                }
            } else if (is_array) {
                m_argument_buffer.AddRawSlots(array_samplers);
            }
            if (track_stage_rescaling) {
                active_rescaling.PushTexture(desc_rescaled);
            }
        }
        for (const auto& desc : info.image_descriptors) {
            bool desc_rescaled = false;
            MTL::Texture* const null_img = null_view.Handle(desc.type);
            for (u32 element = 0; element < desc.count; ++element) {
                const VideoCommon::ImageViewId id = views[view_it++].id;
                MTL::Texture* image = nullptr;
                if (id) {
                    ImageView& view = m_texture_cache.GetImageView(id);
                    // Or a later sync from guest memory overwrites the result.
                    if (desc.is_written && !desc.is_direct) {
                        m_texture_cache.MarkModification(view.image_id);
                    }
                    if (is_vertex_stage && m_texture_cache.IsImageGpuModified(view.image_id)) {
                        out[metal_index].reads_gpu_texture = true;
                    }
                    if (track_stage_rescaling) {
                        desc_rescaled |= m_texture_cache.IsRescaling(view);
                    }
                    image = view.StorageView(desc.type, desc.format);
                }
                m_argument_buffer.AddImage(image != nullptr ? image : null_img, desc.is_written);
            }
            if (track_stage_rescaling) {
                active_rescaling.PushImage(desc_rescaled);
            }
        }

        if (vtg != nullptr && track_stage_rescaling) {
            out[metal_index].rescaling_masks = stage_rescaling.Data();
        }

        // Nothing in Metal checks that the slots written line up with the ones
        // the shader declares, a mismatch silently samples the wrong resource.
        const u32 expected = pipeline.ExpectedArgumentSlots(static_cast<PipelineStage>(metal_index));
        const u32 written = m_argument_buffer.SlotCount();
        if (expected != written) {
            static std::atomic<u32> warned{0};
            if (warned++ < 8) {
                static constexpr std::array<const char*, NUM_PIPELINE_STAGES> NAMES{
                    "vertex", "fragment", "geometry"};
                LOG_WARNING(Render_Metal,
                            "Argument buffer slot mismatch on {} stage: shader declares {}, "
                            "wrote {}",
                            NAMES[metal_index], expected, written);
            }
        }

        out[metal_index].argument_buffer = m_argument_buffer.Build();
        // A failed Build leaves no buffer, and the draw path only rebinds when
        // there IS one - so the stage would keep the previous draw's.
        if (m_argument_buffer.SlotCount() != 0 &&
            out[metal_index].argument_buffer.buffer == nullptr) {
            out[metal_index].resources_failed = true;
        }

        if (!direct_textures.empty() || info.runtime_sampler_count != 0) {
            if (cached_table != nullptr) {
                out[metal_index].table_argument_buffer = cached_table->table_buffer;
            } else {
                m_argument_buffer.Reset();
                m_argument_buffer.Reserve(
                    pipeline.ExpectedTableArgumentSlots(static_cast<PipelineStage>(metal_index)));
                bool any_volume_copy = false;
                for (const auto& [desc_ptr, start_view] : direct_textures) {
                    const Shader::TextureDescriptor& desc = *desc_ptr;
                    const u64 null_tex_id = null_view.ResourceId(desc.type, desc.is_multisample);
                    for (u32 element = 0; element < desc.count; ++element) {
                        const VideoCommon::ImageViewId view_id = views[start_view + element].id;
                        u64 texture_id = 0;
                        if (view_id) {
                            const ImageView& view = m_texture_cache.GetImageView(view_id);
                            any_volume_copy |= view.HasVolumeCopy();
                            if (!view.IsIntegerFormat()) {
                                texture_id = view.ResourceId(desc.type, desc.is_multisample);
                            }
                        }
                        m_argument_buffer.AddBufferAddress(texture_id != 0 ? texture_id
                                                                           : null_tex_id);
                    }
                }
                for (u32 index = 0; index < info.runtime_sampler_count; ++index) {
                    const VideoCommon::SamplerId sampler_id =
                        m_texture_cache.GetGraphicsSamplerId(index);
                    // NULL_SAMPLER_ID (slot 0) is a valid default sampler; always resolve it
                    // so a shader indexing an unused sampler slot never dereferences ID 0.
                    const Sampler& state = m_texture_cache.GetSampler(sampler_id);
                    MTL::SamplerState* const handle = state.HasAddedAnisotropy()
                                                          ? state.HandleWithDefaultAnisotropy()
                                                          : state.Handle();
                    m_argument_buffer.AddBufferAddress(state.ResourceId(handle));
                }
                const u32 expected_table =
                    pipeline.ExpectedTableArgumentSlots(static_cast<PipelineStage>(metal_index));
                const u32 written_table = m_argument_buffer.SlotCount();
                if (expected_table != written_table) {
                    static std::atomic<u32> warned_table{0};
                    if (warned_table++ < 8) {
                        static constexpr std::array<const char*, NUM_PIPELINE_STAGES> NAMES{
                            "vertex", "fragment", "geometry"};
                        LOG_WARNING(
                            Render_Metal,
                            "Table argument buffer slot mismatch on {} stage: shader declares "
                            "{}, wrote {}",
                            NAMES[metal_index], expected_table, written_table);
                    }
                }
                out[metal_index].table_argument_buffer = m_argument_buffer.Build();
                if (written_table != 0 &&
                    out[metal_index].table_argument_buffer.buffer == nullptr) {
                    out[metal_index].resources_failed = true;
                } else if (!any_volume_copy &&
                           out[metal_index].table_argument_buffer.buffer != nullptr &&
                           direct_textures.size() <= 32) {
                    DirectTableCacheEntry& slot =
                        m_direct_table_cache[m_direct_table_cache_next++ &
                                             (m_direct_table_cache.size() - 1)];
                    slot.tick = current_tick;
                    slot.view_generation = m_texture_cache_runtime.ViewGeneration();
                    slot.signature = table_signature;
                    slot.total_views = total_direct_views;
                    slot.runtime_sampler_count = info.runtime_sampler_count;
                    slot.rescaled_mask = direct_rescaled_mask;
                    slot.is_rescaling = track_stage_rescaling;
                    slot.is_compute = false;
                    slot.table_buffer = out[metal_index].table_argument_buffer;
                    slot.view_ids.clear();
                    slot.view_ids.reserve(total_direct_views);
                    for (const auto& [desc_ptr, start_view] : direct_textures) {
                        const u32 count = desc_ptr->count;
                        for (u32 e = 0; e < count; ++e) {
                            slot.view_ids.push_back(views[start_view + e].id);
                        }
                    }
                    slot.sampler_ids.clear();
                    slot.sampler_ids.reserve(info.runtime_sampler_count);
                    for (u32 s = 0; s < info.runtime_sampler_count; ++s) {
                        slot.sampler_ids.push_back(m_texture_cache.GetGraphicsSamplerId(s));
                    }
                }
            }
        }
    });

    if (vtg == nullptr && is_rescaling && any_non_vtg_rescaling) {
        const auto& masks = shared_rescaling.Data();
        for (auto& stage_res : out) {
            if (stage_res.uses_rescaling) {
                stage_res.rescaling_masks = masks;
            }
        }
    }

    if (vtg != nullptr && vtg_bindings != nullptr && vtg_bindings->runs) {
        m_argument_buffer.Reset();
        m_argument_buffer.AddBufferAddress(vtg_bindings->geometry_out.gpu_address);
        vtg_bindings->passthrough_arg_buffer = m_argument_buffer.Build();
        (void)vtg_geometry_size;
    }
}

void RasterizerMetal::UpdatePipelineState() {
    if (maxwell3d == nullptr) {
        return;
    }
    using Maxwell = Tegra::Engines::Maxwell3D::Regs;

    const auto& regs = maxwell3d->regs;
    auto& dirty = maxwell3d->dirty.flags;
    const u32 topology = static_cast<u32>(maxwell3d->draw_manager->GetDrawState().topology);

    // Fast path: when neither pipeline_key_changed nor PIPELINE_STATE_DIRTY is set (or
    // Dirty::VertexBuffers is clear), and engine/framebuffer/topology/RT dirty generation are
    // unchanged, neither stream_words nor rt_formats can have changed.
    if (!dirty[PIPELINE_STATE_DIRTY] &&
        (!maxwell3d->dirty.pipeline_key_changed || !dirty[VideoCommon::Dirty::VertexBuffers]) &&
        m_pipeline_state_inputs.engine == maxwell3d &&
        m_pipeline_state_inputs.framebuffer == m_current_framebuffer &&
        m_pipeline_state_inputs.topology == topology &&
        m_pipeline_state_inputs.rt_dirty_generation == m_rt_dirty_generation) {
        return;
    }

    // Everything the state reads outside the dirty flag: the draw's topology, the
    // attachments, and the registers whose dirty entries other flags hold.
    PipelineStateInputs inputs{};
    inputs.engine = maxwell3d;
    inputs.framebuffer = m_current_framebuffer;
    inputs.rt_dirty_generation = m_rt_dirty_generation;
    inputs.topology = topology;
    for (size_t index = 0; index < Maxwell::NumVertexArrays; ++index) {
        inputs.stream_words[index] = regs.reg_array[MAXWELL3D_REG_INDEX(vertex_streams) +
                                                    index * (sizeof(Maxwell::VertexStream) /
                                                             sizeof(u32))];
    }
    for (size_t index = 0; index < Maxwell::NumRenderTargets; ++index) {
        inputs.rt_formats[index] = static_cast<u32>(regs.rt[index].format);
    }
    if (!dirty[PIPELINE_STATE_DIRTY] &&
        m_pipeline_state_inputs.engine == inputs.engine &&
        m_pipeline_state_inputs.framebuffer == inputs.framebuffer &&
        m_pipeline_state_inputs.topology == inputs.topology &&
        m_pipeline_state_inputs.stream_words == inputs.stream_words &&
        m_pipeline_state_inputs.rt_formats == inputs.rt_formats) {
        m_pipeline_state_inputs.rt_dirty_generation = m_rt_dirty_generation;
        return;
    }
    dirty[PIPELINE_STATE_DIRTY] = false;
    m_pipeline_state_inputs = inputs;

    FixedPipelineState state{};
    // The draw manager's topology, not regs.draw's, re-classed to triangles
    // when the index pass expands it. Metal validates the pairing: a point-class
    // pipeline used with a non-point primitive is a hard error.
    const auto draw_topology = maxwell3d->draw_manager->GetDrawState().topology;
    state.topology_class = NeedsIndexConversion(draw_topology)
                               ? (draw_topology == Maxwell::PrimitiveTopology::LineLoop
                                      ? MTL::PrimitiveTopologyClassLine
                                      : MTL::PrimitiveTopologyClassTriangle)
                               : TopologyClass(draw_topology);
    // The unclassified topology too: a geometry shader's input topology is
    // derived from it and the class throws away Triangles vs TrianglesAdjacency.
    state.topology = static_cast<u8>(draw_topology);
    // What the guest's shaders read as YDirection. Left off - setting it from
    // the register correlates with flicker, and the viewport code already
    // handles orientation.
    state.y_direction_negate = false;
    // NOT set from the guest register, see MakeRuntimeInfo: Apple Silicon TBDR
    // applies early-Z automatically when safe, whereas forcing [[early_fragment_tests]]
    // from regs.mandated_early_z breaks Tears of the Kingdom's deferred mural pass.
    state.force_early_z = false;
    state.ndc_minus_one_to_one = regs.depth_mode == Maxwell::DepthMode::MinusOneToOne;
    state.xfb_enabled = regs.transform_feedback_enabled != 0;
    // ONLY for a draw that submits patches. These registers are live whether or
    // not a title tessellates (`patch_vertices` moves freely), so recording them
    // always mints a new key, and a recompile, for nothing.
    if (draw_topology == Maxwell::PrimitiveTopology::Patches) {
        state.tessellation_domain = static_cast<u8>(regs.tessellation.params.domain_type.Value());
        state.tessellation_spacing = static_cast<u8>(regs.tessellation.params.spacing.Value());
        state.tessellation_clockwise =
            regs.tessellation.params.output_primitives.Value() ==
                    Maxwell::Tessellation::OutputPrimitives::Triangles_CW
                ? 1u
                : 0u;
        state.patch_control_points = static_cast<u8>(regs.patch_vertices);
    }
    // The recompiler bakes the alpha test into the fragment shader. Fold D3D/GL
    // aliases and normalize ref when disabled or Always/Never so stale register
    // values do not mint duplicate pipelines.
    Maxwell::ComparisonOp alpha_func =
        regs.alpha_test_enabled != 0 ? regs.alpha_test_func : Maxwell::ComparisonOp::Always_GL;
    if (alpha_func == Maxwell::ComparisonOp::Always_D3D) {
        alpha_func = Maxwell::ComparisonOp::Always_GL;
    } else if (alpha_func == Maxwell::ComparisonOp::Never_D3D) {
        alpha_func = Maxwell::ComparisonOp::Never_GL;
    }
    state.alpha_test_func = static_cast<u32>(alpha_func);
    const bool alpha_uses_ref = alpha_func != Maxwell::ComparisonOp::Always_GL &&
                                alpha_func != Maxwell::ComparisonOp::Never_GL;
    state.alpha_test_ref = alpha_uses_ref ? std::bit_cast<u32>(regs.alpha_test_ref) : 0u;
    state.dual_source_blend = ComputeAttachment0DualSourceBlend(regs) ? 1u : 0u;
    state.guest_depth_d24 =
        !m_device.SupportsDepth24Stencil8() && regs.zeta_enable != 0 &&
        (regs.zeta.format == Tegra::DepthFormat::Z24_UNORM_S8_UINT ||
         regs.zeta.format == Tegra::DepthFormat::X8Z24_UNORM ||
         regs.zeta.format == Tegra::DepthFormat::S8Z24_UNORM ||
         regs.zeta.format == Tegra::DepthFormat::V8Z24_UNORM);
    const bool depth_equal_readonly =
        regs.depth_test_enable != 0 && regs.depth_write_enabled == 0 &&
        (regs.depth_test_func == Maxwell::ComparisonOp::Equal_D3D ||
         regs.depth_test_func == Maxwell::ComparisonOp::Equal_GL);
    state.depth_equal_readonly = depth_equal_readonly;
    state.d24_frag_depth_equal =
        state.guest_depth_d24 && depth_equal_readonly &&
        m_render_pass.depth_stencil.texture != nullptr &&
        m_render_pass.depth_stencil.texture->textureType() == MTL::TextureType2D &&
        m_render_pass.depth_stencil.texture->sampleCount() == 1 &&
        (m_render_pass.depth_stencil.texture->usage() & MTL::TextureUsageShaderRead) != 0;
    // Unconditional: gating on a point topology would miss a geometry shader
    // emitting points from triangle input, only known after translation.
    state.point_size = std::bit_cast<u32>(regs.point_size);
    state.alpha_to_coverage = regs.anti_alias_alpha_control.alpha_to_coverage != 0;
    state.alpha_to_coverage_dither =
        state.alpha_to_coverage &&
        regs.alpha_to_coverage_dither == Maxwell::AlphaToCoverageDither::Footprint_2x2;
    state.alpha_to_one = regs.anti_alias_alpha_control.alpha_to_one != 0;
    // Two ways the guest asks for no rasterization: rasterize_enable, and a
    // front-and-back cull, which Metal has no cull mode for. Face culling only
    // applies to polygons/triangles, never points or lines.
    const bool has_geometry_stage =
        regs.IsShaderConfigEnabled(static_cast<size_t>(Maxwell::ShaderType::Geometry));
    const bool cull_applies =
        has_geometry_stage || state.topology_class == MTL::PrimitiveTopologyClassTriangle;
    state.rasterizer_discard =
        regs.rasterize_enable == 0 ||
        (cull_applies && regs.gl_cull_test_enabled != 0 &&
         regs.gl_cull_face == Tegra::Engines::Maxwell3D::Regs::CullFace::FrontAndBack);

    {
        using ProvokingVertex = Tegra::Engines::Maxwell3D::Regs::ProvokingVertex;
        static std::atomic<u32> reported{0};
        const auto report = [&](const char* what) {
            if ((reported.fetch_or(1u, std::memory_order_relaxed) & 1u) == 0) {
                LOG_WARNING(Render_Metal, "{}", what);
            }
        };
        static std::atomic<bool> said_logic{false};
        static std::atomic<bool> said_provoking{false};
        static std::atomic<bool> said_bounds{false};
        static std::atomic<bool> said_line{false};
        (void)report;
        if (regs.logic_op.enable != 0 && !said_logic.exchange(true)) {
            // Apple GPUs can read the attachment in a fragment shader, so this
            // is emulatable - but only in the shared recompiler.
            LOG_WARNING(Render_Metal, "Logic op {} is ignored; Metal has no equivalent",
                        static_cast<u32>(regs.logic_op.op));
        }
        if (regs.provoking_vertex == ProvokingVertex::Last && !said_provoking.exchange(true)) {
            LOG_WARNING(Render_Metal, "Last-vertex flat shading is ignored; Metal provokes first");
        }
        if (regs.depth_bounds_enable != 0 && !said_bounds.exchange(true)) {
            LOG_WARNING(Render_Metal, "Depth bounds test is ignored; Metal has no equivalent");
        }
        if (regs.line_width_aliased > 1.0f && !said_line.exchange(true)) {
            LOG_WARNING(Render_Metal, "Line width {} is ignored; Metal draws one pixel wide",
                        regs.line_width_aliased);
        }
    }

    // Where the constant attributes will live - a slot no real stream wants.
    // CONSTANT_ATTRIBUTE_BINDING is only the preferred one.
    std::bitset<Maxwell::NumVertexArrays> streams_in_use;
    for (size_t index = 0; index < Maxwell::NumVertexAttributes; ++index) {
        const auto& attrib = regs.vertex_attrib_format[index];
        if (attrib.constant != 0) {
            continue;
        }
        if (VertexFormat(attrib.size, attrib.type) == MTL::VertexFormatInvalid) {
            continue;
        }
        const u32 stream = static_cast<u32>(attrib.buffer);
        if (stream < streams_in_use.size()) {
            streams_in_use.set(stream);
        }
    }
    // Downwards from the preferred slot, since guests use the low ones. Capture
    // slots get skipped as well as live streams: the preferred slot lands on
    // Metal buffer 30, CAPTURE_LIMITS_BUFFER_INDEX, so a draw with constant
    // attributes during a capture put both there and one overwrote the other.
    const auto slot_taken = [&](u32 binding) {
        return streams_in_use.test(binding) ||
               IsCaptureReservedBufferSlot(BindingMap::VERTEX_BUFFER_BASE + binding);
    };
    u32 constant_binding = CONSTANT_ATTRIBUTE_BINDING;
    while (constant_binding > 0 && slot_taken(constant_binding)) {
        --constant_binding;
    }
    if (slot_taken(constant_binding)) {
        // Needs 30 simultaneous streams to reach.
        static std::atomic<u32> reported{0};
        if (reported++ < 4) {
            LOG_WARNING(Render_Metal,
                        "All {} vertex slots are in use; constant attributes must share slot {}",
                        streams_in_use.count(), constant_binding);
        }
    }
    state.constant_binding = static_cast<u8>(constant_binding);

    // The shader declares in_attrN at location N, so every attribute the guest
    // has enabled must appear in the descriptor or Metal rejects the pipeline.
    for (size_t index = 0; index < Maxwell::NumVertexAttributes; ++index) {
        const auto& attrib = regs.vertex_attrib_format[index];

        state.attribute_types[index] = static_cast<u8>(AttributeTypeOf(regs, index));

        // Constant attributes have no vertex stream, but the descriptor cannot
        // omit them, so they get an entry on the zero-filled null buffer.
        const bool is_constant = attrib.constant != 0;
        const MTL::VertexFormat format =
            is_constant ? MTL::VertexFormatFloat4 : VertexFormat(attrib.size, attrib.type);
        if (format == MTL::VertexFormatInvalid) {
            continue;
        }

        const u32 buffer_index =
            is_constant ? constant_binding : static_cast<u32>(attrib.buffer);
        if (buffer_index > BindingMap::MAX_VERTEX_BUFFER_SLOT - BindingMap::VERTEX_BUFFER_BASE) {
            // Dropping loses geometry, but binding out of range aborts.
            static std::atomic<u32> reported{0};
            if (reported++ < 8) {
                LOG_WARNING(Render_Metal,
                            "Vertex stream {} exceeds the Metal slot limit, dropping attribute {}",
                            buffer_index, index);
            }
            continue;
        }

        state.attributes[index].format = format;
        state.attributes[index].offset = is_constant ? 0 : attrib.offset;
        state.attributes[index].buffer_index = buffer_index;
        state.bindings[buffer_index].enabled = true;
        if (is_constant) {
            state.bindings[buffer_index].stride = 0;
            state.uses_constant_attributes = true;
        }
    }

    for (size_t index = 0; index < Maxwell::NumVertexArrays; ++index) {
        const auto& stream = regs.vertex_streams[index];
        if (!state.bindings[index].enabled) {
            continue;
        }
        if (state.uses_constant_attributes && index == constant_binding) {
            // The constant slot keeps its zero stride and no divisor.
            continue;
        }
        state.bindings[index].stride = stream.stride;
        state.bindings[index].instanced =
            regs.vertex_stream_instances.IsInstancingEnabled(static_cast<u32>(index));
        state.bindings[index].divisor = state.bindings[index].instanced ? stream.frequency : 0;
    }

    // Use the framebuffer already resolved by UpdateRenderTargets (with cached pixel formats)
    // to avoid re-locking m_texture_cache.mutex and calling ObjC pixelFormat() per draw.
    if (m_current_framebuffer != nullptr) {
        const RenderPassState& pass = m_current_framebuffer->GetRenderPassState();
        const auto& formats = m_current_framebuffer->ColorFormats();
        state.num_color_attachments = pass.num_color;
        state.sample_count = pass.sample_count;
        for (u32 index = 0; index < pass.num_color; ++index) {
            state.color_attachments[index].format = formats[index];
        }
    } else {
        std::scoped_lock lock{m_texture_cache.mutex};
        if (const Framebuffer* framebuffer = m_texture_cache.GetFramebuffer()) {
            const RenderPassState& pass = framebuffer->GetRenderPassState();
            const auto& formats = framebuffer->ColorFormats();
            state.num_color_attachments = pass.num_color;
            state.sample_count = pass.sample_count;
            for (u32 index = 0; index < pass.num_color; ++index) {
                state.color_attachments[index].format = formats[index];
            }
        }
    }

    // MSL will not compile a float4 written to an integer attachment, so the
    // output type is decided before translation. Taken from the guest formats,
    // the Metal ones above can be absent for a target nothing has drawn to yet.
    for (size_t index = 0; index < state.color_output_types.size(); ++index) {
        const auto format = regs.rt[index].format;
        if (static_cast<u32>(format) == 0) {
            continue;
        }
        const auto pixel_format = VideoCore::Surface::PixelFormatFromRenderTargetFormat(format);
        if (!VideoCore::Surface::IsPixelFormatInteger(pixel_format)) {
            continue;
        }
        state.color_output_types[index] =
            static_cast<u8>(VideoCore::Surface::IsPixelFormatSignedInteger(pixel_format)
                                ? Shader::AttributeType::SignedInt
                                : Shader::AttributeType::UnsignedInt);
    }

    for (u32 index = 0; index < state.num_color_attachments; ++index) {
        auto& target = state.color_attachments[index];
        const auto out_type = static_cast<Shader::AttributeType>(state.color_output_types[index]);
        const bool is_integer = out_type == Shader::AttributeType::SignedInt ||
                                out_type == Shader::AttributeType::UnsignedInt;
        // Metal rejects blending on integer pixel formats (matches Vulkan).
        target.blend_enable = !is_integer && regs.blend.enable[index] != 0;
        if (target.blend_enable) {
            const auto setup = [&target](const auto& blend) {
                target.src_rgb = BlendFactor(blend.color_source);
                target.dst_rgb = BlendFactor(blend.color_dest);
                target.rgb_op = BlendEquation(blend.color_op);
                target.src_alpha = BlendFactor(blend.alpha_source);
                target.dst_alpha = BlendFactor(blend.alpha_dest);
                target.alpha_op = BlendEquation(blend.alpha_op);
            };
            if (regs.blend_per_target_enabled != 0) {
                setup(regs.blend_per_target[index]);
            } else if (regs.iterated_blend.enable != 0 &&
                       false) {
                // Maxwell's programmable per-fragment blend unit, which Metal
                // cannot express. While it is on, regs.blend is stale, so the
                // other backends' fixed squashed equation is used instead.
                target.src_rgb = MTL::BlendFactorOne;
                target.dst_rgb = MTL::BlendFactorOne;
                target.rgb_op = MTL::BlendOperationAdd;
                target.src_alpha = MTL::BlendFactorOneMinusSourceColor;
                target.dst_alpha = MTL::BlendFactorZero;
                target.alpha_op = MTL::BlendOperationAdd;
                target.blend_enable = true;
            } else {
                setup(regs.blend);
            }
        }

        const auto mask = regs.color_mask[regs.color_mask_common != 0 ? 0 : index];
        MTL::ColorWriteMask write_mask = MTL::ColorWriteMaskNone;
        if (mask.R != 0) {
            write_mask |= MTL::ColorWriteMaskRed;
        }
        if (mask.G != 0) {
            write_mask |= MTL::ColorWriteMaskGreen;
        }
        if (mask.B != 0) {
            write_mask |= MTL::ColorWriteMaskBlue;
        }
        if (mask.A != 0) {
            write_mask |= MTL::ColorWriteMaskAlpha;
        }
        target.write_mask = write_mask;
    }

    m_pipeline_cache.SetGraphicsState(state);
}

void RasterizerMetal::DrawTexture() {
    CommitPendingReadbacks();
    if (maxwell3d == nullptr) {
        return;
    }
    SCOPE_EXIT {
        m_gpu.TickWork();
    };

    std::scoped_lock lock{m_texture_cache.mutex};
    m_texture_cache.SynchronizeGraphicsDescriptors();
    m_texture_cache.UpdateRenderTargets(false);

    const Framebuffer* const framebuffer = m_texture_cache.GetFramebuffer();
    if (framebuffer == nullptr) {
        return;
    }
    const auto& state = maxwell3d->draw_manager->GetDrawTextureState();
    const ImageView& texture = m_texture_cache.GetImageView(state.src_texture);

    const bool src_rescaling = m_texture_cache.IsRescaling() && texture.IsRescaled();
    const bool dst_rescaling = m_texture_cache.IsRescaling() && framebuffer->IsRescaled();
    const auto scale = [](bool rescaling, auto value) {
        const auto dim = static_cast<s32>(value);
        return rescaling ? videoSettings.resolution_info.ScaleUp(dim) : dim;
    };

    const VideoCommon::Region2D dst_region{
        VideoCommon::Offset2D{scale(dst_rescaling, state.dst_x0),
                              scale(dst_rescaling, state.dst_y0)},
        VideoCommon::Offset2D{scale(dst_rescaling, state.dst_x1),
                              scale(dst_rescaling, state.dst_y1)},
    };
    const VideoCommon::Region2D src_region{
        VideoCommon::Offset2D{scale(src_rescaling, state.src_x0),
                              scale(src_rescaling, state.src_y0)},
        VideoCommon::Offset2D{scale(src_rescaling, state.src_x1),
                              scale(src_rescaling, state.src_y1)},
    };

    MTL::Texture* const dst = framebuffer->GetRenderPassState().color[0].texture;
    MTL::Texture* const src = texture.Handle(Shader::TextureType::Color2D) != nullptr
                                  ? texture.Handle(Shader::TextureType::Color2D)
                                  : texture.RenderTarget();
    if (dst == nullptr || src == nullptr) {
        return;
    }
    MTL::SamplerState* override_sampler = nullptr;
    if (const Sampler* const guest_sampler =
            m_texture_cache.GetGraphicsSampler(state.src_sampler);
        guest_sampler != nullptr) {
        override_sampler = SamplerFor(*guest_sampler, texture);
    }

    // DrawTexture follows the same scissor selection and scaling as a regular draw
    // (matches Vulkan PR #553).
    const auto& regs = maxwell3d->regs;
    const f32 dst_scale = dst_rescaling ? videoSettings.resolution_info.up_factor : 1.0f;
    const bool lower_left =
        regs.window_origin.mode != Tegra::Engines::Maxwell3D::Regs::WindowOrigin::Mode::UpperLeft;
    std::optional<MTL::ScissorRect> scissor;
    if (regs.viewport_scale_offset_enabled == 0) {
        const s64 area_w = static_cast<s64>(dst->width());
        const s64 area_h = static_cast<s64>(dst->height());
        s64 left = static_cast<s64>(regs.surface_clip.x * dst_scale);
        s64 top = static_cast<s64>(regs.surface_clip.y * dst_scale);
        s64 right =
            left + static_cast<s64>((std::max)(1, static_cast<s32>(regs.surface_clip.width)) *
                                    dst_scale);
        s64 bottom =
            top + static_cast<s64>((std::max)(1, static_cast<s32>(regs.surface_clip.height)) *
                                   dst_scale);
        left = std::clamp<s64>(left, 0, area_w);
        top = std::clamp<s64>(top, 0, area_h);
        right = std::clamp<s64>(right, left, area_w);
        bottom = std::clamp<s64>(bottom, top, area_h);
        scissor = MTL::ScissorRect{
            .x = static_cast<NS::UInteger>(left),
            .y = static_cast<NS::UInteger>(top),
            .width = static_cast<NS::UInteger>(right - left),
            .height = static_cast<NS::UInteger>(bottom - top),
        };
    } else if (regs.scissor_test[0].enable != 0) {
        const auto& sc = regs.scissor_test[0];
        if (sc.max_x <= sc.min_x || sc.max_y <= sc.min_y) {
            return;
        }
        s64 min_y = static_cast<s64>(sc.min_y);
        s64 max_y = static_cast<s64>(sc.max_y);
        if (lower_left) {
            const s64 clip_h = static_cast<s64>(regs.surface_clip.height);
            const s64 flipped_min = clip_h - max_y;
            max_y = clip_h - min_y;
            min_y = flipped_min;
        }
        const s64 area_w = static_cast<s64>(dst->width());
        const s64 area_h = static_cast<s64>(dst->height());
        const s64 left = std::clamp<s64>(static_cast<s64>(sc.min_x * dst_scale), 0, area_w);
        const s64 top = std::clamp<s64>(static_cast<s64>(min_y * dst_scale), 0, area_h);
        const s64 right = std::clamp<s64>(static_cast<s64>(sc.max_x * dst_scale), left, area_w);
        const s64 bottom = std::clamp<s64>(static_cast<s64>(max_y * dst_scale), top, area_h);
        scissor = MTL::ScissorRect{
            .x = static_cast<NS::UInteger>(left),
            .y = static_cast<NS::UInteger>(top),
            .width = static_cast<NS::UInteger>(right - left),
            .height = static_cast<NS::UInteger>(bottom - top),
        };
    }

    m_blit_image.BlitColor(dst, src, dst_region, src_region,
                           Tegra::Engines::Fermi2D::Filter::Bilinear, scissor, override_sampler);
    m_texture_cache.MarkRenderTargetsWritten(~0U, false);
}

void RasterizerMetal::Clear(u32 layer_count) {
    CommitPendingReadbacks();
    if (!maxwell3d) {
        return;
    }
    MTL::Buffer* cond_indirect_buffer = nullptr;
    u32 cond_indirect_offset = 0;
    if (m_gpu_predicate.active && m_gpu_predicate.engine == maxwell3d) {
        if (m_gpu_predicate.cpu_resolved) {
            if (!m_gpu_predicate.cpu_result) {
                return;
            }
        } else if (!m_gpu_predicate.from_buffer && m_scheduler.IsFree(m_gpu_predicate.wait_tick)) {
            if (!EvaluateGpuPredicateOnCpu()) {
                return;
            }
        } else if (m_buffer_cache_runtime.GetIndexPass().HasConditionalDraw()) {
            // Evaluate the conditional clear on the GPU via an indirect 3-vertex
            // drawn clear instead of stalling the CPU with ReadBlock()/Scheduler::Wait().
            if (m_gpu_predicate.wait_tick >= m_scheduler.CurrentTick()) {
                m_scheduler.Flush();
            }
            u32 pflags = 0u;
            if (m_gpu_predicate.from_buffer) {
                pflags |= 32u;
            } else {
                if (m_gpu_predicate.a_from_gpu) {
                    pflags |= 1u | (m_gpu_predicate.a_is_64bit ? 2u : 0u);
                }
                if (m_gpu_predicate.b_from_gpu) {
                    pflags |= 4u | (m_gpu_predicate.b_is_64bit ? 8u : 0u);
                }
            }
            const ConditionalDrawParams cond_params{
                .a_cpu_value = m_gpu_predicate.a_cpu_value,
                .b_cpu_value = m_gpu_predicate.b_cpu_value,
                .mode = static_cast<u32>(m_gpu_predicate.mode),
                .flags = pflags,
                .a_first_slot = static_cast<u32>(m_gpu_predicate.a_first_slot % VISIBILITY_SLOTS),
                .a_slot_count =
                    static_cast<u32>(m_gpu_predicate.a_end_slot - m_gpu_predicate.a_first_slot),
                .b_first_slot = static_cast<u32>(m_gpu_predicate.b_first_slot % VISIBILITY_SLOTS),
                .b_slot_count =
                    static_cast<u32>(m_gpu_predicate.b_end_slot - m_gpu_predicate.b_first_slot),
                .vis_mask = VISIBILITY_SLOTS - 1u,
                .draw_count = 3u,
                .num_instances = 1u,
                .base_vertex = 0u,
                .base_instance = 0u,
            };
            MTL::Buffer* pred_src_buffer = m_visibility_buffer.get();
            u32 pred_src_offset = 0u;
            const bool resolved_src =
                !m_gpu_predicate.from_buffer ||
                ResolveBufferPredicate(pred_src_buffer, pred_src_offset);
            const PatchedIndirect cond_args =
                resolved_src
                    ? m_buffer_cache_runtime.GetIndexPass().MakeConditionalDrawArgs(
                          pred_src_buffer, cond_params, pred_src_offset)
                    : PatchedIndirect{};
            if (cond_args.valid) {
                if (!m_gpu_predicate.from_buffer) {
                    const u64 cur_tick = m_scheduler.CurrentTick();
                    if (m_gpu_predicate.a_from_gpu) {
                        for (u64 s = m_gpu_predicate.a_first_slot; s < m_gpu_predicate.a_end_slot;
                             ++s) {
                            const u32 idx = static_cast<u32>(s % VISIBILITY_SLOTS);
                            m_visibility_slot_tick[idx] =
                                std::max(m_visibility_slot_tick[idx], cur_tick);
                        }
                    }
                    if (m_gpu_predicate.b_from_gpu) {
                        for (u64 s = m_gpu_predicate.b_first_slot; s < m_gpu_predicate.b_end_slot;
                             ++s) {
                            const u32 idx = static_cast<u32>(s % VISIBILITY_SLOTS);
                            m_visibility_slot_tick[idx] =
                                std::max(m_visibility_slot_tick[idx], cur_tick);
                        }
                    }
                }
                cond_indirect_buffer = cond_args.buffer;
                cond_indirect_offset = cond_args.offset;
            } else if (!EvaluateGpuPredicateOnCpu()) {
                return;
            }
        } else if (!EvaluateGpuPredicateOnCpu()) {
            return;
        }
    }
    // FlushWork()/++m_draw_counter belong here, a clear-dominated frame
    // otherwise never hits the mid-frame commit cadence and FlushCommands
    // early-returns on m_draw_counter == 0. Left out while we bisect a rendering
    // artifact. Tried putting it back and reverted it, Z-A came back with
    // full-screen vertical striping. Do not restore without a Z-A run.

    m_gpu.TickWork();
    if (gpu_memory) {
        gpu_memory->FlushCaching();
    }
    // The clear is expressed entirely by the load actions UpdateRenderTargets
    // sets. It must clear no more than `clear_surface` names, or a depth-only
    // clear also blanks colour.
    const auto& clear = maxwell3d->regs.clear_surface;
    const u32 color_mask = (clear.R != 0 ? MTL::ColorWriteMaskRed : 0u) |
                           (clear.G != 0 ? MTL::ColorWriteMaskGreen : 0u) |
                           (clear.B != 0 ? MTL::ColorWriteMaskBlue : 0u) |
                           (clear.A != 0 ? MTL::ColorWriteMaskAlpha : 0u);
    const ClearAspects aspects{
        .color = color_mask != 0,
        .depth = clear.Z != 0,
        .stencil = clear.S != 0,
        .color_target = clear.RT.Value(),
        .layer = clear.layer.Value(),
        .layer_count = layer_count,
        .color_mask = color_mask,
        .stencil_mask = maxwell3d->regs.stencil_front_mask & 0xffu,
        .indirect_buffer = cond_indirect_buffer,
        .indirect_offset = cond_indirect_offset,
    };
    if (!aspects.color && !aspects.depth && !aspects.stencil) {
        return;
    }

    UpdateRenderTargets(true, aspects);
    std::scoped_lock lock{m_texture_cache.mutex};
    m_texture_cache.MarkRenderTargetsWritten(
        aspects.color ? 1u << aspects.color_target : 0u, aspects.depth || aspects.stencil);
}

void RasterizerMetal::DispatchCompute() {
    CommitPendingReadbacks();
    // Drains the guest's pending sync requests, see Clear().
    m_gpu.TickWork();
    if (gpu_memory) {
        gpu_memory->FlushCaching();
    }
    // See the note in Clear(): the same FlushWork()/++m_draw_counter pair is
    // out here for the same bisect, and for the same Z-A striping.

    ComputePipeline* const pipeline = m_pipeline_cache.CurrentComputePipeline();
    if (pipeline == nullptr) {
        static std::atomic<u32> skipped{0};
        const u32 n = ++skipped;
        if (n == 1 || n % 10000 == 0) {
            LOG_WARNING(Render_Metal, "Compute dispatch skipped, no pipeline ({} so far)", n);
        }
        return;
    }
    if (kepler_compute == nullptr) {
        return;
    }

    const auto& qmd = kepler_compute->launch_description;

    // Close any open render pass (and fire any pending pass-boundary flush) BEFORE
    // ConfigureComputeResources binds buffers and stamps MarkUsage()/argument buffers,
    // so the compute dispatch is guaranteed to land in the same submission tick.
    m_scheduler.RequestOutsideRenderPassOperationContext();
    m_scheduler.DeferBoundaryFlush(true);
    SCOPE_EXIT {
        m_scheduler.DeferBoundaryFlush(false);
    };
    const u64 bind_tick = m_scheduler.CurrentTick();

    StagingBufferRef argument_buffer{};
    StagingBufferRef table_argument_buffer{};
    std::array<u32, 6> compute_rescaling_masks{};
    std::array<u32, BufferCache::MAX_WRITE_TRACK_SLOTS * 2> compute_write_tracking{};
    u32 compute_write_tracking_bytes = 0;
    RuntimeImageWriteState runtime_image_writes{};
    ConfigureComputeResources(*pipeline, argument_buffer, table_argument_buffer,
                              compute_rescaling_masks, compute_write_tracking,
                              compute_write_tracking_bytes, runtime_image_writes);

    const auto finish_runtime_image_writes = [&]() {
        if (runtime_image_writes.mask_words == 0 || !runtime_image_writes.has_candidates ||
            runtime_image_writes.mask.buffer == nullptr) {
            return;
        }
        // Only stall the GPU thread to read back the exact write mask when at least one
        // candidate 3D image has guest aliases that must observe exact write ordering.
        // Non-aliased candidates can be marked directly with zero synchronization stall.
        bool any_aliased = false;
        {
            std::scoped_lock lock{m_texture_cache.mutex};
            for (const VideoCommon::ImageViewId id : runtime_image_writes.views) {
                if (id != VideoCommon::NULL_IMAGE_VIEW_ID &&
                    m_texture_cache.HasAliasedImages(m_texture_cache.GetImageView(id).image_id)) {
                    any_aliased = true;
                    break;
                }
            }
            if (!any_aliased) {
                for (const VideoCommon::ImageViewId id : runtime_image_writes.views) {
                    if (id != VideoCommon::NULL_IMAGE_VIEW_ID) {
                        m_texture_cache.MarkModification(m_texture_cache.GetImageView(id).image_id);
                    }
                }
                return;
            }
        }
        // Guest image aliases must see precisely the images written before their next use.
        m_scheduler.Finish();
        std::scoped_lock lock{m_texture_cache.mutex};
        for (u32 word = 0; word < runtime_image_writes.mask_words; ++word) {
            u32 bits = 0;
            std::memcpy(&bits,
                        runtime_image_writes.mask.mapped_span.data() + word * sizeof(u32),
                        sizeof(bits));
            while (bits != 0) {
                const u32 bit = static_cast<u32>(std::countr_zero(bits));
                bits &= bits - 1;
                const VideoCommon::ImageViewId id = runtime_image_writes.views[word * 32u + bit];
                if (id != VideoCommon::NULL_IMAGE_VIEW_ID) {
                    m_texture_cache.MarkModification(m_texture_cache.GetImageView(id).image_id);
                }
            }
        }
    };

    struct ComputePushConstants {
        Shader::Backend::SPIRV::RescalingLayout scaling{};
        std::array<u32, BufferCache::MAX_WRITE_TRACK_SLOTS * 2> write_track{};
    };
    static_assert(offsetof(ComputePushConstants, write_track) ==
                  Shader::Backend::SPIRV::WRITE_TRACK_OFFSET);

    const bool compute_uses_rescaling = pipeline->GetInfo().uses_rescaling_uniform;
    const bool compute_uses_push = compute_uses_rescaling || compute_write_tracking_bytes != 0;
    const f32 compute_down_factor =
        m_texture_cache.IsRescaling() ? videoSettings.resolution_info.down_factor : 1.0f;
    ComputePushConstants compute_push{};
    compute_push.scaling = Shader::Backend::SPIRV::MakeGraphicsPushConstants(
                               compute_rescaling_masks, compute_down_factor,
                               Shader::Backend::SPIRV::NO_ATTACHMENT_READS, {})
                               .scaling;
    compute_push.write_track = compute_write_tracking;
    const u32 compute_push_size =
        compute_write_tracking_bytes != 0
            ? (Shader::Backend::SPIRV::WRITE_TRACK_OFFSET + compute_write_tracking_bytes)
            : static_cast<u32>(sizeof(Shader::Backend::SPIRV::RescalingLayout));

    MTL::Size threadgroup = pipeline->GetThreadgroupSize();
    const u32 declared = static_cast<u32>(threadgroup.width * threadgroup.height *
                                          threadgroup.depth);
    if (declared == 0) {
        return;
    }

    // A GPU-driven dispatch reads its grid from a buffer the GPU itself wrote,
    // the QMD's grid_dim fields are stale for one.
    const std::optional<GPUVAddr> indirect =
        Metal::Options::IndirectCompute()
            ? kepler_compute->GetIndirectComputeAddress()
            : std::nullopt;
    if (indirect) {
        static constexpr auto sync_info = VideoCommon::ObtainBufferSynchronize::FullSynchronize;
        // The arguments are only read; what the GPU wrote there must still reach the guest.
        static constexpr auto post_op = VideoCommon::ObtainBufferOperation::DoNothing;
        static constexpr u32 DISPATCH_ARGS_SIZE = 12; // three u32 group counts
        std::scoped_lock lock{m_buffer_cache.mutex};
        const auto [indirect_buffer, indirect_offset] =
            m_buffer_cache.ObtainBuffer(*indirect, DISPATCH_ARGS_SIZE, sync_info, post_op);
        if (indirect_buffer == nullptr) {
            return;
        }
        // Read later in this command buffer: an upload reordered before the dispatch
        // would replace arguments an earlier shader wrote.
        indirect_buffer->MarkUsage(indirect_offset, DISPATCH_ARGS_SIZE);
        MTL::Buffer* const handle = indirect_buffer->Handle();
        m_scheduler.RequestOutsideRenderPassOperationContext();
        m_scheduler.Record([pipeline, threadgroup, declared, handle, indirect_offset,
                            argument_buffer, table_argument_buffer, compute_uses_push,
                            compute_push, compute_push_size](CommandContext& context) {
            pipeline->WaitBuilt();
            MTL::ComputePipelineState* const state = pipeline->Handle();
            if (state == nullptr) {
                return;
            }
            const u32 max_threads = pipeline->GetMaxTotalThreadsPerThreadgroup();
            if (declared > max_threads) {
                LOG_WARNING(Render_Metal, "Compute kernel wants {} threads per group, max is {}",
                            declared, max_threads);
                return;
            }
            ComputeEncoder encoder = context.RequestCompute();
            if (encoder == nullptr) {
                return;
            }
            encoder->setComputePipelineState(state);
            if (argument_buffer.buffer != nullptr) {
                encoder->setBuffer(argument_buffer.buffer, argument_buffer.offset,
                                   BindingMap::ARGUMENT_BUFFER_INDEX);
            }
            if (table_argument_buffer.buffer != nullptr) {
                encoder->setBuffer(table_argument_buffer.buffer, table_argument_buffer.offset,
                                   BindingMap::COMPUTE_TABLE_ARGUMENT_BUFFER_INDEX);
            }
            if (compute_uses_push) {
                encoder->setBytes(&compute_push, compute_push_size,
                                  BindingMap::COMPUTE_PUSH_CONSTANT_INDEX);
            }
            encoder->dispatchThreadgroups(handle, indirect_offset, threadgroup);
        });
        m_buffer_cache.CopySparseWrites();
        finish_runtime_image_writes();
        if (m_scheduler.CurrentTick() != bind_tick) [[unlikely]] {
            m_scheduler.Flush();
        }
        return;
    }

    const MTL::Size grid =
        MTL::Size::Make(qmd.grid_dim_x, qmd.grid_dim_y, qmd.grid_dim_z);
    if (grid.width == 0 || grid.height == 0 || grid.depth == 0) {
        return;
    }

    m_scheduler.RequestOutsideRenderPassOperationContext();

    m_scheduler.Record([pipeline, grid, threadgroup, declared, argument_buffer,
                        table_argument_buffer, compute_uses_push, compute_push,
                        compute_push_size](CommandContext& context) {
        pipeline->WaitBuilt();
        MTL::ComputePipelineState* const state = pipeline->Handle();
        if (state == nullptr) {
            return;
        }
        const u32 max_threads = pipeline->GetMaxTotalThreadsPerThreadgroup();
        if (declared > max_threads) {
            LOG_WARNING(Render_Metal, "Compute kernel wants {} threads per group, max is {}",
                        declared, max_threads);
            return;
        }
        ComputeEncoder encoder = context.RequestCompute();
        if (encoder == nullptr) {
            return;
        }

        encoder->setComputePipelineState(state);
        if (argument_buffer.buffer != nullptr) {
            encoder->setBuffer(argument_buffer.buffer, argument_buffer.offset,
                               BindingMap::ARGUMENT_BUFFER_INDEX);
        }
        if (table_argument_buffer.buffer != nullptr) {
            encoder->setBuffer(table_argument_buffer.buffer, table_argument_buffer.offset,
                               BindingMap::COMPUTE_TABLE_ARGUMENT_BUFFER_INDEX);
        }
        if (compute_uses_push) {
            encoder->setBytes(&compute_push, compute_push_size,
                              BindingMap::COMPUTE_PUSH_CONSTANT_INDEX);
        }

        encoder->dispatchThreadgroups(grid, threadgroup);
    });
    if (m_buffer_cache.HasSparseWrites()) {
        std::scoped_lock lock{m_buffer_cache.mutex};
        m_buffer_cache.CopySparseWrites();
    }
    finish_runtime_image_writes();
    if (m_scheduler.CurrentTick() != bind_tick) [[unlikely]] {
        m_scheduler.Flush();
    }
}

void RasterizerMetal::ResetCounter(VideoCommon::QueryType type) {
    switch (type) {
    case VideoCommon::QueryType::ZPassPixelCount64:
        // The sum restarts from the next slot a counting draw claims.
        m_visibility_first = m_visibility_next;
        m_visibility_current = ~0ull;
        break;
    case VideoCommon::QueryType::StreamingByteCount:
        m_xfb_bytes.fill(0);
        m_xfb_vtg_pending.fill(false);
        break;
    case VideoCommon::QueryType::StreamingPrimitivesSucceeded:
    case VideoCommon::QueryType::StreamingPrimitivesNeeded:
        // Both, or Needed - Succeeded goes non-zero, which guests read as
        // capture overflow.
        m_xfb_primitives.fill(0);
        break;
    default:
        break;
    }
}

void RasterizerMetal::BeginOrSyncXfbSession() {
    if (!maxwell3d) {
        return;
    }
    const auto& tfb = maxwell3d->regs.transform_feedback;
    const bool toggled = maxwell3d->dirty.flags[XFB_SESSION_DIRTY];
    maxwell3d->dirty.flags[XFB_SESSION_DIRTY] = false;
    if (!m_xfb_session_active || toggled) {
        m_xfb_session_active = true;
        m_xfb_bytes.fill(0);
        m_xfb_vtg_pending.fill(false);
        for (u32 i = 0; i < 4; ++i) {
            m_xfb_session_addr[i] = tfb.buffers[i].Address();
            m_xfb_session_start[i] = tfb.buffers[i].start_offset;
        }
        return;
    }
    for (u32 i = 0; i < 4; ++i) {
        const u64 addr = tfb.buffers[i].Address();
        const u32 start = tfb.buffers[i].start_offset;
        if (addr != m_xfb_session_addr[i] || start != m_xfb_session_start[i]) {
            m_xfb_session_addr[i] = addr;
            m_xfb_session_start[i] = start;
            m_xfb_bytes[i] = 0;
            m_xfb_vtg_pending[i] = false;
        }
    }
}

void RasterizerMetal::ResolvePendingVtgXfbCounters(u32 stream) {
    if (stream >= 4 || !m_xfb_vtg_pending[stream] || !m_xfb_emitted_buffer) {
        return;
    }
    const u64 tick = m_xfb_vtg_tick[stream];
    if (!m_scheduler.IsFree(tick)) {
        m_scheduler.Flush();
        m_scheduler.Wait(tick);
    }
    const u32 emitted_verts = static_cast<const u32*>(m_xfb_emitted_buffer->contents())[0];
    const u32 vpp = std::max(m_xfb_vtg_verts_per_prim[stream], 1u);
    m_xfb_primitives[stream] += emitted_verts / vpp;
    m_xfb_bytes[stream] += u64{emitted_verts} * m_xfb_vtg_stride[stream];
    m_xfb_vtg_pending[stream] = false;
}

void RasterizerMetal::EnsureVisibilityBuffer() {
    if (m_visibility_buffer) {
        return;
    }
    m_visibility_buffer = Adopt(m_device.GetDevice()->newBuffer(
        VISIBILITY_SLOTS * sizeof(u64), MTL::ResourceStorageModeShared));
    if (m_visibility_buffer) {
        m_visibility_buffer->setLabel(MTLSTR("eden occlusion counts"));
        std::memset(m_visibility_buffer->contents(), 0, VISIBILITY_SLOTS * sizeof(u64));
        m_visibility_slot_tick.assign(VISIBILITY_SLOTS, 0);
        // Nothing is implicitly resident on Metal 4, and this is created here
        // rather than through MemoryAllocator, which handles residency itself.
        m_device.AddResidentAllocation(m_visibility_buffer.get());
        m_device.CommitResidency();
    }
}

void RasterizerMetal::BeginPassVisibility(RenderPassState& pass) {
    // Named by every pass once the title has counted, not only while counting is on: a pass
    // with a visibility buffer can't continue one without it, so following the guest's
    // zpass_pixel_count_enable toggles ended a render pass at each one.
    if (!m_occlusion_used) {
        return;
    }
    EnsureVisibilityBuffer();
    pass.visibility_buffer = m_visibility_buffer.get();
}

u32 RasterizerMetal::CountingSlot() {
    const u64 pass = m_scheduler.PassSerial();
    if (m_visibility_current != ~0ull && m_visibility_current_pass == pass) {
        return static_cast<u32>(m_visibility_current % VISIBILITY_SLOTS);
    }
    const u64 slot = m_visibility_next++;
    const u32 index = static_cast<u32>(slot % VISIBILITY_SLOTS);
    // The ring came round: the pass that last wrote this slot, and the query reading it,
    // must be done first. 16k ranges is several frames even in heavy titles.
    if (slot >= VISIBILITY_SLOTS) {
        ResolveOcclusionQueries(false);
        bool pending;
        {
            std::scoped_lock lock{m_occlusion_mutex};
            pending = !m_pending_occlusion.empty() &&
                      m_pending_occlusion.front().first_slot + VISIBILITY_SLOTS <= slot;
        }
        if (pending) {
            ResolveOcclusionQueries(true);
        }
        const u64 slot_tick = m_visibility_slot_tick[index];
        if (!m_scheduler.IsFree(slot_tick)) {
            // Wait explicitly even when m_pending_occlusion is empty: a counting pass
            // whose query was never issued may still be writing this slot on the GPU.
            m_scheduler.Wait(slot_tick);
            ResolveOcclusionQueries(false);
        }
    }
    m_visibility_slot_tick[index] = m_scheduler.CurrentTick();
    // Metal only writes the slot if a draw counts into it, and overwrites it at the end
    // of the pass, so it starts from zero for this range.
    static_cast<u64*>(m_visibility_buffer->contents())[index] = 0;
    m_visibility_current = slot;
    m_visibility_current_pass = pass;
    return index;
}

void RasterizerMetal::ResolveOcclusionQueries(bool wait) {
    if (m_pending_occlusion_count.load(std::memory_order_acquire) == 0) {
        return;
    }
    if (wait) {
        u64 last = 0;
        {
            std::scoped_lock lock{m_occlusion_mutex};
            if (!m_pending_occlusion.empty()) {
                last = m_pending_occlusion.back().tick;
            }
        }
        if (last != 0 && !m_scheduler.IsFree(last)) {
            // Never hold m_occlusion_mutex across a blocking GPU wait.
            m_scheduler.Wait(last);
        }
    }

    // Serialize ordered retirement without holding m_occlusion_mutex across guest writes
    // or ProtectResult, while keeping entries visible in m_pending_occlusion until
    // their writes have landed in guest RAM. Release m_occlusion_resolve_mutex BEFORE
    // acquiring m_buffer_cache.mutex so ResolveOcclusionQueries never inverts lock order
    // against DrawImpl (which holds m_buffer_cache.mutex when calling CountingSlot).
    std::unique_lock resolve_lock{m_occlusion_resolve_mutex};
    boost::container::small_vector<PendingOcclusion, 16> ready;
    {
        std::scoped_lock lock{m_occlusion_mutex};
        for (const PendingOcclusion& query : m_pending_occlusion) {
            // In order: a later answer must not land before an earlier one.
            if (!m_scheduler.IsFree(query.tick)) {
                break;
            }
            ready.push_back(query);
        }
    }
    if (ready.empty()) {
        return;
    }

    const u64* const slots = static_cast<const u64*>(m_visibility_buffer->contents());
    for (const PendingOcclusion& query : ready) {
        if (query.invalidated) {
            continue;
        }
        u64 total = 0;
        for (u64 slot = query.first_slot; slot < query.end_slot; ++slot) {
            total += slots[slot % VISIBILITY_SLOTS];
        }
        if (query.has_timeout) {
            query.memory->Write<u64>(query.gpu_addr + 8, m_gpu.GetTicks());
            query.memory->Write<u64>(query.gpu_addr, total);
        } else {
            query.memory->Write<u32>(query.gpu_addr, static_cast<u32>(total));
        }
    }
    {
        std::scoped_lock lock{m_occlusion_mutex};
        const size_t erase_count = std::min(ready.size(), m_pending_occlusion.size());
        m_pending_occlusion.erase(m_pending_occlusion.begin(),
                                  m_pending_occlusion.begin() + erase_count);
        m_pending_occlusion_count.store(static_cast<u32>(m_pending_occlusion.size()),
                                        std::memory_order_release);
    }
    resolve_lock.unlock();
    {
        // Notify BufferCache of newly published occlusion results ONLY on 4 KiB pages that
        // have no live GPU writes (marking a GPU-modified page CPU-modified would clobber
        // neighboring GPU-written vertex/indirect data sharing that page).
        bool any_cpu_sync = false;
        for (const PendingOcclusion& query : ready) {
            if (!query.invalidated && query.device_addr != 0) {
                any_cpu_sync = true;
                break;
            }
        }
        if (any_cpu_sync) {
            std::scoped_lock lock{m_buffer_cache.mutex};
            for (const PendingOcclusion& query : ready) {
                if (query.invalidated || query.device_addr == 0) {
                    continue;
                }
                const u64 bytes = query.has_timeout ? 16u : 4u;
                if (!m_buffer_cache.IsRegionRegistered(query.device_addr, bytes)) {
                    continue;
                }
                const DAddr aligned_start =
                    Common::AlignDown(query.device_addr, Core::DEVICE_PAGESIZE);
                const DAddr aligned_end =
                    Common::AlignUp(query.device_addr + bytes, Core::DEVICE_PAGESIZE);
                if (!m_buffer_cache.IsRegionGpuModified(aligned_start,
                                                        aligned_end - aligned_start)) {
                    m_buffer_cache.WriteMemory(query.device_addr, bytes);
                }
            }
        }
    }
    for (const PendingOcclusion& query : ready) {
        if (query.protected_result) {
            ProtectResult(query.device_addr, query.has_timeout ? 16 : 4, -1);
        }
    }
}

void RasterizerMetal::InvalidatePendingOcclusion(DAddr addr, u64 size) {
    if (addr == 0 || size == 0 ||
        m_pending_occlusion_count.load(std::memory_order_acquire) == 0) {
        return;
    }
    std::scoped_lock lock{m_occlusion_mutex};
    for (PendingOcclusion& query : m_pending_occlusion) {
        if (query.device_addr == 0) {
            continue;
        }
        const u64 query_size = query.has_timeout ? 16u : 4u;
        if (query.device_addr < addr + size && addr < query.device_addr + query_size) {
            query.invalidated = true;
        }
    }
}

void RasterizerMetal::ResolveOcclusionQueriesAt(GPUVAddr gpu_addr, u64 size) {
    u64 tick = 0;
    {
        std::scoped_lock lock{m_occlusion_mutex};
        for (const PendingOcclusion& query : m_pending_occlusion) {
            if (query.invalidated) {
                continue;
            }
            const u64 query_size = query.has_timeout ? 16 : 4;
            if (query.gpu_addr < gpu_addr + size && gpu_addr < query.gpu_addr + query_size) {
                tick = std::max(tick, query.tick);
            }
        }
    }
    if (tick == 0) {
        return;
    }
    m_scheduler.Wait(tick);
    ResolveOcclusionQueries(false);
}

void RasterizerMetal::Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
                            VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) {
    if (!gpu_memory) {
        return;
    }
    DrainPendingXfb(XfbDrain::ForGuestRead);
    // An answer must always be written or the guest deadlocks on the semaphore.
    // With no query cache it cannot be truthful, so it errs large: zero makes a
    // title conclude an object is hidden and stop submitting it.
    if (maxwell3d != nullptr && maxwell3d->regs.transform_feedback_enabled == 0) {
        m_xfb_session_active = false;
    }
    u64 answer;
    switch (type) {
    case VideoCommon::QueryType::Payload:
        answer = static_cast<u64>(payload);
        break;
    case VideoCommon::QueryType::StreamingPrimitivesNeededMinusSucceeded:
    case VideoCommon::QueryType::TotalStreamingPrimitivesNeededMinusSucceeded:
        // The XFB overflow check, and zero is the truthful answer. The generic
        // max() below reads as "everything overflowed", after which the guest
        // stops issuing capture batches entirely.
        answer = 0;
        break;
    case VideoCommon::QueryType::StreamingPrimitivesSucceeded:
    case VideoCommon::QueryType::StreamingPrimitivesNeeded:
        ResolvePendingVtgXfbCounters(subreport & 3);
        answer = m_xfb_primitives[subreport & 3];
        break;
    case VideoCommon::QueryType::StreamingByteCount:
        ResolvePendingVtgXfbCounters(subreport & 3);
        answer = m_xfb_bytes[subreport & 3];
        break;
    case VideoCommon::QueryType::ZPassPixelCount64:
        if (m_occlusion_used) {
            // Answered once the submission holding its passes completes, never with a
            // wait: the next fence that retires or the next query publishes it.
            ResolveOcclusionQueries(false);
            const u64 first = m_visibility_first;
            const u64 end = m_visibility_next;
            // Later draws count into a new range, this one is closed.
            m_visibility_current = ~0ull;
            if (first == end) {
                answer = 0;
                break;
            }
            const bool timeout = True(flags & VideoCommon::QueryPropertiesFlags::HasTimeout);
            const DAddr device_addr = gpu_memory->GpuToCpuAddress(gpu_addr).value_or(0);
            const u64 full_query_bytes = timeout ? 16u : 4u;
            if (device_addr != 0) {
                InvalidatePendingOcclusion(device_addr, full_query_bytes);
            }
            const u64 last_slot_tick = m_visibility_slot_tick[(end - 1) % VISIBILITY_SLOTS];
            const u64 current_tick = m_scheduler.CurrentTick();
            const u64 query_tick = (last_slot_tick != 0 && last_slot_tick <= current_tick)
                                       ? last_slot_tick
                                       : current_tick;
            // Fast path: if the counting submission already finished on the GPU, publish
            // immediately on the CPU + inline into BufferCache without queueing a pending entry.
            if (m_visibility_buffer && m_scheduler.IsFree(query_tick)) {
                const u64* const slots = static_cast<const u64*>(m_visibility_buffer->contents());
                u64 total = 0;
                for (u64 slot = first; slot < end; ++slot) {
                    total += slots[slot % VISIBILITY_SLOTS];
                }
                answer = total;
                break;
            }
            const bool protect_result =
                device_addr != 0 && m_device_memory.ProtectsReadCachedPages();
            // Serialize with ResolveOcclusionQueries (same lock order: resolve_mutex ->
            // occlusion_mutex -> ProtectResult) so a fast GPU completion cannot retire the
            // query and call ProtectResult(-1) before ProtectResult(+1) runs.
            {
                std::scoped_lock resolve_lock{m_occlusion_resolve_mutex};
                {
                    std::scoped_lock lock{m_occlusion_mutex};
                    m_pending_occlusion.push_back({query_tick, first, end, gpu_addr, gpu_memory,
                                                   timeout, device_addr, protect_result, false});
                    m_pending_occlusion_count.store(static_cast<u32>(m_pending_occlusion.size()),
                                                    std::memory_order_release);
                    m_last_occlusion_tick.store(query_tick, std::memory_order_release);
                    m_occlusion_needs_fence.store(true, std::memory_order_release);
                }
                if (protect_result) {
                    ProtectResult(device_addr, timeout ? 16 : 4, 1);
                }
            }
            if (device_addr != 0) {
                // So engine reads of guest memory (DMA, inline copies, macros) wait for it.
                m_device_memory.RegisterFlushCacheRegion(device_addr, timeout ? 16 : 4,
                                                         CacheType::QueryCache);
            }
            m_fence_manager.SyncOperation([this] { ResolveOcclusionQueries(false); });
            return;
        }
        // No counting draw has occurred yet: truthful pixel count is 0, not UINT32_MAX.
        answer = 0;
        break;
    default:
        answer = std::numeric_limits<u32>::max();
        break;
    }

    const bool has_timeout = True(flags & VideoCommon::QueryPropertiesFlags::HasTimeout);
    Tegra::MemoryManager* const memory = gpu_memory;
    const auto publish = [this, memory, gpu_addr, answer, has_timeout] {
        if (has_timeout) {
            const u64 ticks = m_gpu.GetTicks();
            memory->Write<u64>(gpu_addr + 8, ticks);
            memory->Write<u64>(gpu_addr, answer);
        } else {
            memory->Write<u32>(gpu_addr, static_cast<u32>(answer));
        }
    };
    if (type != VideoCommon::QueryType::Payload) {
        const DAddr device_addr = memory->GpuToCpuAddress(gpu_addr).value_or(0);
        const u64 full_bytes = has_timeout ? 16u : 4u;
        if (device_addr != 0) {
            InvalidatePendingOcclusion(device_addr, full_bytes);
        }
        publish();
        if (device_addr != 0) {
            const u32 sync_bytes = has_timeout ? 8u : 4u;
            std::array<u8, 8> raw{};
            if (has_timeout) {
                std::memcpy(raw.data(), &answer, sizeof(u64));
            } else {
                const u32 ans32 = static_cast<u32>(answer);
                std::memcpy(raw.data(), &ans32, sizeof(u32));
            }
            std::scoped_lock lock{m_buffer_cache.mutex};
            if (m_buffer_cache.IsRegionRegistered(device_addr, sync_bytes) &&
                !m_buffer_cache.InlineMemory(device_addr, sync_bytes,
                                             std::span<const u8>{raw.data(), sync_bytes})) {
                const DAddr aligned_start = Common::AlignDown(device_addr, Core::DEVICE_PAGESIZE);
                const DAddr aligned_end =
                    Common::AlignUp(device_addr + sync_bytes, Core::DEVICE_PAGESIZE);
                if (!m_buffer_cache.IsRegionGpuModified(aligned_start,
                                                        aligned_end - aligned_start)) {
                    m_buffer_cache.WriteMemory(device_addr, sync_bytes);
                }
            }
        }
        return;
    }
    // Like the query cache's fast payloads: the guest reads what the GPU wrote as soon as it
    // sees the semaphore, so pending readbacks are submitted first, and a payload is held
    // behind a fence only when they cannot be. Vulkan holds a semaphore neither behind
    // pending occlusion queries nor behind texture downloads: doing so here delayed the
    // guest's semaphores, and Tears of the Kingdom drew shadows, clouds and light wrong for
    // single frames.
    const DAddr payload_device_addr = memory->GpuToCpuAddress(gpu_addr).value_or(0);
    if (payload_device_addr != 0) {
        InvalidatePendingOcclusion(payload_device_addr, has_timeout ? 16u : 4u);
    }
    const bool is_fence = True(flags & VideoCommon::QueryPropertiesFlags::IsAFence);
    const bool behind_readback =
        Settings::IsGpuReadbackExact() || !CommitReadbacksForEarlySignal();
    // Semaphores only move forward: a later payload may not overtake a held one.
    const bool chained = m_payloads_held.load(std::memory_order_acquire) != 0;
    if (behind_readback || chained) {
        m_payloads_held.fetch_add(1, std::memory_order_relaxed);
        std::function<void()> held = [this, publish] {
            publish();
            m_payloads_held.fetch_sub(1, std::memory_order_release);
        };
        if (!m_fence_manager.AttachToLastFence(std::move(held))) {
            SignalFence(std::move(held));
        }
        return;
    }
    publish();
    if (is_fence) {
        SignalReference();
    }
}

void RasterizerMetal::BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr,
                                                u32 size) {
    // Only updates GPU-thread-private channel_state->uniform_buffers (matches RasterizerVulkan).
    m_buffer_cache.BindGraphicsUniformBuffer(stage, index, gpu_addr, size);
}

void RasterizerMetal::DisableGraphicsUniformBuffer(size_t stage, u32 index) {
    m_buffer_cache.DisableGraphicsUniformBuffer(stage, index);
}

bool RasterizerMetal::PrefetchUniform(size_t stage, u32 index, DAddr device_addr, u32 max_size,
                                      VideoCommon::UniformPrefetch& prefetch) {
    return m_buffer_cache.PrefetchUniform(stage, index, device_addr, max_size, prefetch);
}

u64 RasterizerMetal::UniformPrefetchPosition() {
    return m_buffer_cache.UniformPrefetchPosition();
}

void RasterizerMetal::ApplyUniformPrefetch(size_t stage, u32 index,
                                           const VideoCommon::UniformPrefetch& prefetch) {
    m_buffer_cache.ApplyUniformPrefetch(stage, index, prefetch);
}

void RasterizerMetal::ResetUniformPrefetch(u64 position) {
    m_buffer_cache.ResetUniformPrefetch(position);
}

void RasterizerMetal::FlushAll() {}

void RasterizerMetal::FlushRegion(DAddr addr, u64 size, CacheType which) {
    FlushRegionImpl(addr, size, which, false);
}

void RasterizerMetal::FlushNativeRegion(DAddr addr, u64 size) {
    FlushRegionImpl(addr, size, CacheType::All, true);
}

void RasterizerMetal::FlushRegionImpl(DAddr addr, u64 size, CacheType which, bool native_read) {
    if (addr == 0 || size == 0) {
        return;
    }
    // Pages no cache registered are skipped without taking any lock.
    which = m_device_memory.FilterFlushCaches(addr, size, which);
    if (True(which & CacheType::TextureCache)) {
        std::scoped_lock lock{m_texture_cache.mutex};
        m_texture_cache.DownloadMemory(addr, size, native_read);
    }
    if (True(which & CacheType::BufferCache)) {
        std::scoped_lock lock{m_buffer_cache.mutex};
        m_buffer_cache.DownloadMemory(addr, size);
    }
    // An engine reading a pending occlusion result from guest memory, in GPU order: Vulkan's
    // query cache flushes it the same way.
    if (True(which & CacheType::QueryCache)) {
        if (const u64 tick = PendingOcclusionTickAt(addr, size); tick != 0) {
            m_scheduler.Wait(tick);
            ResolveOcclusionQueries(false);
        }
    }
}

bool RasterizerMetal::MayFlushSharedPage(DAddr addr, u64 size) {
    return m_buffer_cache.MayHaveGpuWrites(addr, size) ||
           m_texture_cache.MayHaveUnpublishedReadImage(addr, size);
}

bool RasterizerMetal::MayHaveComputeWrites(DAddr addr, u64 size) {
    return m_buffer_cache.MayHaveComputeWrites(addr, size) ||
           m_texture_cache.MayHaveComputeWrites(addr, size);
}

u64 RasterizerMetal::ComputeWriteEpoch() {
    return m_buffer_cache.ComputeWriteEpoch() + m_texture_cache.ComputeWriteEpoch();
}

bool RasterizerMetal::AwaitsProtectedDownload(DAddr addr, u64 size) {
    return m_buffer_cache.AwaitsProtectedDownload(addr, size) ||
           m_protected_result_pages.Any(addr, size);
}

bool RasterizerMetal::AwaitsProtectedBufferDownload(DAddr addr, u64 size) {
    return m_buffer_cache.AwaitsProtectedDownload(addr, size) ||
           m_texture_cache.AwaitsProtectedDownload(addr, size);
}

void RasterizerMetal::WaitProtectedBufferDownloads(DAddr addr, u64 size) {
    m_buffer_cache.WaitProtectedDownload(addr, size);
    m_texture_cache.WaitProtectedDownload(addr, size);
}

u64 RasterizerMetal::PendingOcclusionTickAt(DAddr addr, u64 size) {
    if (m_pending_occlusion_count.load(std::memory_order_acquire) == 0) {
        return 0;
    }
    if (m_device_memory.ProtectsReadCachedPages() && !m_protected_result_pages.Any(addr, size)) {
        return 0;
    }
    u64 tick = 0;
    std::scoped_lock lock{m_occlusion_mutex};
    for (const PendingOcclusion& query : m_pending_occlusion) {
        if (query.invalidated) {
            continue;
        }
        const u64 query_size = query.has_timeout ? 16 : 4;
        if (query.device_addr != 0 && query.device_addr < addr + size &&
            addr < query.device_addr + query_size) {
            tick = std::max(tick, query.tick);
        }
    }
    return tick;
}

bool RasterizerMetal::HasPendingResult(DAddr addr, u64 size) {
    if (!m_protected_result_pages.Any(addr, size)) {
        return false;
    }
    return PendingOcclusionTickAt(addr, size) != 0;
}

void RasterizerMetal::ProtectResult(DAddr addr, u64 size, s32 delta) {
    // As Vulkan's query cache: the guest may poll a result without waiting on a semaphore,
    // and native code reads guest memory directly. Read before the answer lands, Tears of
    // the Kingdom saw the previous value and culled grass for a frame. A CPU access to a
    // neighbouring guest page of the same host page must not open it early either.
    using PageBits = decltype(m_protected_result_pages);
    // For delta > 0, set m_protected_result_pages BEFORE applying PROT_NONE so a faulting
    // guest CPU never observes PROT_NONE while HasPendingResult/PendingOcclusionTickAt is false.
    // For delta < 0, clear m_protected_result_pages before releasing PROT_NONE.
    {
        std::scoped_lock lock{m_protected_result_mutex};
        const u64 end = Common::AlignUp(addr + size, PageBits::PAGE_SIZE);
        for (u64 page = Common::AlignDown(addr, PageBits::PAGE_SIZE); page < end;
             page += PageBits::PAGE_SIZE) {
            if (delta > 0) {
                if (m_protected_result_counts[page]++ == 0) {
                    m_protected_result_pages.Set(page, PageBits::PAGE_SIZE);
                }
                continue;
            }
            const auto it = m_protected_result_counts.find(page);
            if (it != m_protected_result_counts.end() && --it->second == 0) {
                m_protected_result_counts.erase(it);
                m_protected_result_pages.Clear(page, PageBits::PAGE_SIZE);
            }
        }
    }
    m_device_memory.UpdatePagesReadCachedCount(addr, size, delta > 0 ? 1 : -1);
}

bool RasterizerMetal::CommitReadbacksForEarlySignal() {
    // Without CPU faults on pending readbacks, fast readback accepts stale reads.
    if (!m_device_memory.ProtectsReadCachedPages()) {
        return true;
    }
    bool pending;
    {
        std::scoped_lock lock{m_buffer_cache.mutex, m_texture_cache.mutex};
        pending = m_buffer_cache.HasUncommittedFlushes() || m_texture_cache.HasUncommittedFlushes();
    }
    if (pending) {
        m_fence_manager.SignalReference();
    }
    std::scoped_lock lock{m_buffer_cache.mutex, m_texture_cache.mutex};
    return !m_texture_cache.HasUnprotectedDownloads() && !m_buffer_cache.HasUnprotectedDownloads();
}

VideoCore::SharedPageAccess RasterizerMetal::ObserveSharedPage(DAddr addr, u64 size) {
    bool deferred = false;
    {
        std::scoped_lock lock{m_buffer_cache.mutex};
        if (m_buffer_cache.IsRegionGpuModified(addr, size)) {
            if (!m_buffer_cache.IsGpuModifiedRegionProtected(addr, size)) {
                return VideoCore::SharedPageAccess::Flush;
            }
            deferred = true;
        }
    }
    std::scoped_lock lock{m_texture_cache.mutex};
    if (m_texture_cache.ObserveReadImages(addr, size)) {
        return VideoCore::SharedPageAccess::Flush;
    }
    return deferred ? VideoCore::SharedPageAccess::Deferred : VideoCore::SharedPageAccess::Current;
}

bool RasterizerMetal::MustFlushRegion(DAddr addr, u64 size, CacheType which) {
    which = m_device_memory.FilterFlushCaches(addr, size, which);
    // Pending occlusion results must be flushed at any accuracy, as on Vulkan.
    if (True(which & CacheType::QueryCache) && PendingOcclusionTickAt(addr, size) != 0) {
        return true;
    }
    if (True(which & CacheType::BufferCache) && m_buffer_cache.MayHaveGpuWrites(addr, size)) {
        std::scoped_lock lock{m_buffer_cache.mutex};
        if (m_buffer_cache.IsRegionGpuModified(addr, size)) {
            return true;
        }
    }
    // Below High accuracy the texture cache is not consulted at all, matching
    // Vulkan. Answering yes makes the engine read a render target back through
    // guest memory, a full drain plus a download.
    if (!Settings::IsGPULevelHigh()) {
        return false;
    }
    if (True(which & CacheType::TextureCache)) {
        std::scoped_lock lock{m_texture_cache.mutex};
        return m_texture_cache.IsRegionGpuModified(addr, size);
    }
    return false;
}

RasterizerDownloadArea RasterizerMetal::GetFlushArea(DAddr addr, u64 size) {
    if (True(m_device_memory.FilterFlushCaches(addr, size, CacheType::TextureCache))) {
        std::scoped_lock lock{m_texture_cache.mutex};
        auto area = m_texture_cache.GetFlushArea(addr, size);
        if (area) {
            return *area;
        }
    }
    {
        std::scoped_lock lock{m_buffer_cache.mutex};
        auto area = m_buffer_cache.GetFlushArea(addr, size);
        if (area) {
            return *area;
        }
    }
    return RasterizerDownloadArea{
        .startAddress = Common::AlignDown(addr, Core::DEVICE_PAGESIZE),
        .endAddress = Common::AlignUp(addr + size, Core::DEVICE_PAGESIZE),
        .preemtive = true,
    };
}

void RasterizerMetal::InvalidateRegion(DAddr addr, u64 size, CacheType which) {
    if (addr == 0 || size == 0) {
        return;
    }
    InvalidatePendingOcclusion(addr, size);
    if (True(which & CacheType::TextureCache)) {
        std::scoped_lock lock{m_texture_cache.mutex};
        m_texture_cache.WriteMemory(addr, size);
    }
    if (True(which & CacheType::BufferCache)) {
        std::scoped_lock lock{m_buffer_cache.mutex};
        m_buffer_cache.WriteMemory(addr, size);
    }
    if (True(which & CacheType::ShaderCache)) {
        m_pipeline_cache.InvalidateRegion(addr, size);
    }
}

void RasterizerMetal::InnerInvalidation(
    std::span<const std::pair<DAddr, std::size_t>> sequences) {
    // Overridden purely for the LOCKING: the base class takes a pair of
    // cache-mutex round-trips per page rather than per batch.
    if (m_pending_occlusion_count.load(std::memory_order_acquire) != 0) {
        for (const auto& [addr, size] : sequences) {
            InvalidatePendingOcclusion(addr, size);
        }
    }
    {
        std::scoped_lock lock{m_texture_cache.mutex};
        for (const auto& [addr, size] : sequences) {
            if (addr == 0 || size == 0) {
                continue;
            }
            m_texture_cache.WriteMemory(addr, size);
        }
    }
    {
        std::scoped_lock lock{m_buffer_cache.mutex};
        for (const auto& [addr, size] : sequences) {
            if (addr == 0 || size == 0) {
                continue;
            }
            m_buffer_cache.WriteMemory(addr, size);
        }
    }
    for (const auto& [addr, size] : sequences) {
        if (addr == 0 || size == 0) {
            continue;
        }
        m_pipeline_cache.InvalidateRegion(addr, size);
    }
}

void RasterizerMetal::OnCacheInvalidation(PAddr addr, u64 size) {
    if (addr == 0 || size == 0) {
        return;
    }
    {
        std::scoped_lock lock{m_texture_cache.mutex};
        m_texture_cache.WriteMemory(addr, size);
    }
    {
        std::scoped_lock lock{m_buffer_cache.mutex};
        m_buffer_cache.WriteMemory(addr, size);
    }
    m_pipeline_cache.InvalidateRegion(addr, size);
}

u64 RasterizerMetal::OnCacheInvalidationAhead(PAddr addr, u64 size) {
    if (addr == 0 || size == 0) {
        return 0;
    }
    std::scoped_lock lock{m_buffer_cache.mutex};
    return m_buffer_cache.WriteMemoryAhead(addr, size);
}

bool RasterizerMetal::IsReadCachedOnlyForQueryResults(DAddr addr, u64 size) const {
    const DAddr end = addr + size;
    for (DAddr page = Common::AlignDown(addr, Core::DEVICE_PAGESIZE); page < end;
         page += Core::DEVICE_PAGESIZE) {
        if (!m_protected_result_pages.Any(page, Core::DEVICE_PAGESIZE) ||
            !m_device_memory.IsReadCachedOnce(page, Core::DEVICE_PAGESIZE)) {
            return false;
        }
    }
    return true;
}

void RasterizerMetal::OnCoarseCacheInvalidation(PAddr addr, u64 size) {
    if (addr == 0 || size == 0) {
        return;
    }
    const auto caches = m_device_memory.FilterFlushCaches(
        addr, size, CacheType::BufferCache | CacheType::TextureCache);
    if (True(caches & CacheType::TextureCache)) {
        std::scoped_lock lock{m_texture_cache.mutex};
        m_texture_cache.WriteMemory(addr, size);
    }
    if (True(caches & CacheType::BufferCache)) {
        std::scoped_lock lock{m_buffer_cache.mutex};
        m_buffer_cache.CoarseWriteMemory(addr, size);
    }
    m_pipeline_cache.InvalidateRegion(addr, size);
}

bool RasterizerMetal::OnCPUWrite(PAddr addr, u64 size) {
    if (addr == 0 || size == 0) {
        return false;
    }
    InvalidatePendingOcclusion(addr, size);
    m_pipeline_cache.OnCacheInvalidation(addr, size);
    const auto caches = m_device_memory.FilterFlushCaches(
        addr, size, CacheType::BufferCache | CacheType::TextureCache);
    if (True(caches & CacheType::BufferCache)) {
        std::scoped_lock lock{m_buffer_cache.mutex};
        if (m_buffer_cache.OnCPUWrite(addr, size)) {
            return true;
        }
    }
    if (True(caches & CacheType::TextureCache)) {
        std::scoped_lock lock{m_texture_cache.mutex};
        m_texture_cache.WriteMemory(addr, size);
    }
    return false;
}

void RasterizerMetal::InvalidateGPUCache() {
    m_gpu.InvalidateGPUCache();
}

void RasterizerMetal::UnmapMemory(DAddr addr, u64 size) {
    InvalidatePendingOcclusion(addr, size);
    {
        std::scoped_lock lock{m_texture_cache.mutex};
        m_texture_cache.UnmapMemory(addr, size);
    }
    {
        std::scoped_lock lock{m_buffer_cache.mutex};
        m_buffer_cache.WriteMemory(addr, size);
    }
    m_pipeline_cache.OnCacheInvalidation(addr, size);
}

void RasterizerMetal::ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size,
                                      bool preserve_contents) {
    std::scoped_lock lock{m_texture_cache.mutex};
    m_texture_cache.UnmapGPUMemory(as_id, addr, size, preserve_contents);
}

// These four must go through FenceManager rather than flushing inline. Flushing
// on every guest fence signal removes all CPU/GPU overlap and slows the guest by
// roughly two orders of magnitude.
void RasterizerMetal::SignalFence(std::function<void()>&& func) {
    DrainPendingXfb(XfbDrain::ForGuestRead);
    // As on Vulkan: the guest signal waits for the host fence, the guest takes it as
    // the GPU being done with everything before it.
    m_fence_manager.SignalQueryFence(std::move(func));
}

void RasterizerMetal::SignalMemoryOperationFence(std::function<void()>&& func) {
    DrainPendingXfb(XfbDrain::ForGuestRead);
    m_fence_manager.SignalMemoryOperationFence(std::move(func));
}

void RasterizerMetal::WaitForFence() {
    DrainPendingXfb(XfbDrain::ForGuestRead);
    m_fence_manager.WaitForFence();
}

void RasterizerMetal::SyncOperation(std::function<void()>&& func) {
    DrainPendingXfb(XfbDrain::ForGuestRead);
    m_fence_manager.SyncOperation(std::move(func));
}

void RasterizerMetal::SignalSyncPoint(u32 value) {
    DrainPendingXfb(XfbDrain::ForGuestRead);
    m_fence_manager.SignalSyncPoint(value);
}

void RasterizerMetal::SignalReference() {
    if (m_pending_occlusion_count.load(std::memory_order_acquire) != 0) {
        // Retire any occlusion results whose submission already completed so a guest CPU
        // waiting in AwaitPendingResult unblocks immediately without queueing a new fence.
        ResolveOcclusionQueries(false);
        if (m_pending_occlusion_count.load(std::memory_order_acquire) != 0) {
            m_occlusion_needs_fence.store(true, std::memory_order_release);
            m_fence_manager.SyncOperation([this] { ResolveOcclusionQueries(false); });
        }
    }
    m_fence_manager.SignalReference();
}

void RasterizerMetal::ReleaseFences(bool force) {
    m_fence_manager.WaitPendingFences(force);
}

void RasterizerMetal::FlushAndInvalidateRegion(DAddr addr, u64 size, CacheType which) {
    if (Settings::IsGPULevelHigh()) {
        FlushRegion(addr, size, which);
    }
    InvalidateRegion(addr, size, which);
}

void RasterizerMetal::WaitForIdle() {
    // A GPU *ordering* request, not a request to stall the CPU. The guest issues
    // it thousands of times a second. Ending the encoder is the ordering: Metal
    // 4 tracks no hazards, and the next encoder to open emits a queue-scope
    // barrier that waits for every prior stage. Copies and dispatches end the
    // pass anyway, so inside one only the draws' storage writes need it.
    m_scheduler.RequestDeferredFragmentBarrier();
    m_fence_manager.SignalOrdering();
}

void RasterizerMetal::FragmentBarrier() {
    // Ending the render pass publishes both attachment writes (TBDR tile memory -> RAM)
    // and fragment storage writes before subsequent draws sample them (matches MoltenVK).
    // Skip only when the pass has neither drawn nor cleared any attachment yet.
    if (!m_scheduler.PassDrew() && m_scheduler.PassWrittenAttachments() == 0) {
        return;
    }
    m_scheduler.RequestOutsideRenderPassOperationContext();
}

void RasterizerMetal::TiledCacheBarrier() {
    // On Apple Silicon TBDR, intra-pass attachment RMW is already hardware-ordered
    // in primitive order and TextureCache::CheckFeedbackLoop breaks the render pass
    // whenever a draw samples a render target written in the open pass. Only break on
    // storage writes when feedback-loop barriers are active to avoid dozens of wasted
    // tile store/reloads per frame.
    if (videoSettings.barrier_feedback_loops) {
        m_scheduler.RequestDeferredFragmentBarrier();
        return;
    }
    if (!m_scheduler.PassDrew()) {
        return;
    }
    m_scheduler.RequestOutsideRenderPassOperationContext();
}

void RasterizerMetal::FlushCommands() {
    if (m_draw_counter == 0) {
        return;
    }
    m_draw_counter = 0;
    // InnerFence::Queue() already flushed the current submission before SubmitFence
    // calls FlushCommands(); skip submitting a second empty command buffer.
    if (m_last_draw_tick != m_scheduler.CurrentTick()) {
        return;
    }
    m_scheduler.FlushAtPassBoundary();
}

void RasterizerMetal::DrainPendingXfb(XfbDrain drain) {
    if (m_pending_xfb.empty() || gpu_memory == nullptr) {
        return;
    }
    // Waiting on the captures instead of Finish()ing the queue: Finish flushes
    // everything recorded and blocks on the newest tick, which in Legends Z-A
    // meant ~170 full pipeline drains a frame and left the GPU idle half the
    // time.
    const bool wait = XFB_WRITEBACK_ENABLED && drain != XfbDrain::WhenComplete;
    if (wait) {
        u64 latest = 0;
        for (const PendingXfbGather& pending : m_pending_xfb) {
            latest = std::max(latest, pending.tick);
        }
        m_scheduler.Wait(latest);
    }

    const MasterSemaphore& semaphore = m_scheduler.GetMasterSemaphore();
    boost::container::small_vector<const PendingXfbGather*, 32> consumed;
    for (const PendingXfbGather& pending : m_pending_xfb) {
        // Recycling a capture segment the GPU is still writing into corrupts
        // the next capture that draws it.
        if (!wait && !semaphore.IsFree(pending.tick)) {
            continue;
        }
        const u8* const captured = static_cast<const u8*>(pending.capture->contents());
        if (captured == nullptr) {
            m_transform_feedback.ReleaseCaptureSegment(pending.capture);
            consumed.push_back(&pending);
            continue;
        }

        // Do NOT add a value check over these floats. Screening for non-finite
        // or absurd values looked great, 97% of captures came back "dirty", one
        // component per vertex, and it was all bogus. The flagged component is a
        // 10/10/10 packed normal the shader builds with as_type<float>, whose
        // bits are *meant* to read as 9.6e37 or NaN. A capture buffer holds
        // packed integers as often as geometry.
        for (const PendingXfbGather::Destination& destination : pending.destinations) {
            if (!XFB_WRITEBACK_ENABLED) {
                break;
            }
            // Clamped to what the guest declared. The full computed span spills
            // into the next mesh's vertices.
            const u64 wanted = u64{pending.vertices} * destination.out_stride;
            const u32 total_bytes = static_cast<u32>(std::min(wanted, destination.guest_size));
            if (total_bytes == 0) {
                continue;
            }
            // Read-modify-write, so gaps the varyings miss survive.
            boost::container::small_vector<u8, 4096> scratch(total_bytes);
            gpu_memory->ReadBlockUnsafe(destination.guest_address, scratch.data(), total_bytes);
            for (u32 vertex = 0; vertex < pending.vertices; ++vertex) {
                const u8* const src_vertex = captured + u64{vertex} * pending.capture_stride;
                u8* const dst_vertex = scratch.data() + u64{vertex} * destination.out_stride;
                for (const XfbCopy& copy : destination.copies) {
                    std::memcpy(dst_vertex + copy.dst_offset, src_vertex + copy.src_offset,
                                copy.components * sizeof(float));
                }
            }
            // WriteBlockUnsafe on purpose, the same bytes already reached the
            // cache buffer through the GPU gather, so no invalidation is wanted.
            gpu_memory->WriteBlockUnsafe(destination.guest_address, scratch.data(), total_bytes);
        }
        m_transform_feedback.ReleaseCaptureSegment(pending.capture);
        consumed.push_back(&pending);
    }

    if (wait) {
        m_pending_xfb.clear();
    } else {
        std::erase_if(m_pending_xfb, [&consumed](const PendingXfbGather& pending) {
            return std::find(consumed.begin(), consumed.end(), &pending) != consumed.end();
        });
    }
}

void RasterizerMetal::FlushWork() {
    // Bounded by encoded work, not draw count: one draw can carry a capture
    // kernel, an index expansion and a gather per destination buffer.
    //
    // The bound adapts. A submission the GPU watchdog kills fails with
    // MTL4CommandQueueErrorTimeout and is *discarded*, so the frame is torn as
    // well as late. Pokemon Legends Z-A lost 18 submissions in a session at 512,
    // each running 8-50 ms, Odyssey never trips it.
    static constexpr u32 CHECK_MASK = 7;
    static constexpr u32 MIN_WORK_TO_COMMIT = 64;

    // The per-submission pass ceiling lives in Scheduler::RequestRenderPass:
    // this runs per draw, so enforcing it here would split passes mid-flight.
    if ((++m_flush_work_counter & CHECK_MASK) != CHECK_MASK) {
        return;
    }
    static constexpr u32 MAX_WORK_TO_COMMIT = 512;
    const auto& semaphore = m_scheduler.GetMasterSemaphore();
    const u64 tick = m_scheduler.CurrentTick();
    if (const u32 timeouts = semaphore.TimeoutCount(); timeouts != m_seen_timeouts) {
        m_seen_timeouts = timeouts;
        m_last_watchdog_event = std::chrono::steady_clock::now();
        m_watchdog_gpu_tick = semaphore.KnownGpuTick();
        const u32 reduced = std::max(m_work_to_commit / 2, MIN_WORK_TO_COMMIT);
        const u32 reduced_passes =
            std::max(m_scheduler.MaxPassesPerSubmission() / 2, Scheduler::MIN_PASSES_PER_SUBMISSION);
        m_scheduler.SetMaxPassesPerSubmission(reduced_passes);
        if (reduced != m_work_to_commit) {
            m_work_to_commit = reduced;
            LOG_WARNING(Render_Metal,
                        "GPU watchdog killed a submission; committing every {} work units / {} "
                        "passes now",
                        m_work_to_commit, reduced_passes);
        }
    } else if (tick != m_flush_work_tick &&
               (m_work_to_commit < MAX_WORK_TO_COMMIT ||
                m_scheduler.MaxPassesPerSubmission() < Scheduler::MAX_PASSES_PER_SUBMISSION)) {
        // Checked only once per submission while throttled so the unthrottled
        // hot path does no clock reads, and growing back requires both real
        // GPU progress and a quiet wall-clock window.
        const u64 gpu_tick = semaphore.KnownGpuTick();
        if (gpu_tick >= m_watchdog_gpu_tick + WATCHDOG_GROW_MIN_GPU_TICKS) {
            const auto now = std::chrono::steady_clock::now();
            if (now - m_last_watchdog_event >= WATCHDOG_GROW_COOLDOWN) {
                // And back up again once the timeouts stop. Halving was one-way, so a
                // burst of kills ratcheted the bound to its floor and left it there for
                // the rest of the run: 256, then 128, then 64, which is 950 submissions
                // a second of pure overhead at 75% GPU occupancy with no frames to show
                // for it.
                m_last_watchdog_event = now;
                m_watchdog_gpu_tick = gpu_tick;
                m_work_to_commit = std::min(m_work_to_commit * 2, MAX_WORK_TO_COMMIT);
                const u32 grown_passes = std::min(m_scheduler.MaxPassesPerSubmission() * 2,
                                                  Scheduler::MAX_PASSES_PER_SUBMISSION);
                m_scheduler.SetMaxPassesPerSubmission(grown_passes);
                LOG_INFO(Render_Metal,
                         "No submission killed for a while; back to {} work units / {} passes",
                         m_work_to_commit, grown_passes);
            }
        }
    }
    // A submission since the last check, at a pass boundary we asked for, starts the count over.
    if (tick != m_flush_work_tick) {
        m_flush_work_tick = tick;
        m_flush_work_counter = 0;
        m_recorded_gpu_work = 0;
    }
    const u32 work = m_flush_work_counter + m_recorded_gpu_work;
    if (work < m_work_to_commit) {
        // Handing a chunk to the worker wakes it and swaps chunks under two
        // locks. Every 8 draws that cost more than the recording it saves; fewer,
        // larger hand-offs starve the GPU (256 draws: 107 -> 78 FPS).
        static constexpr u32 CHECKS_PER_DISPATCH = 8;
        if (++m_dispatch_checks >= CHECKS_PER_DISPATCH) {
            m_dispatch_checks = 0;
            m_scheduler.DispatchWork();
        }
        return;
    }
    // Waits for the open pass to end, unless a single pass carries enough to risk the watchdog.
    if (work < m_work_to_commit * 4) {
        m_scheduler.FlushAtPassBoundary();
        m_scheduler.DispatchWork();
        return;
    }
    m_scheduler.Flush();
    m_flush_work_counter = 0;
    m_recorded_gpu_work = 0;
}

RasterizerMetal::FrameDraws RasterizerMetal::TakeFrameDraws() {
    bool new_render_targets{};
    {
        std::scoped_lock lock{m_texture_cache.mutex};
        new_render_targets = m_texture_cache.TakeNewRenderTargets();
    }
    return {std::exchange(m_frame_draws, 0), std::exchange(m_frame_skipped_draws, 0),
            new_render_targets};
}

void RasterizerMetal::TickFrame() {
    // Throttling on purpose: the wait keeps the CPU from running so far ahead
    // that single submissions grow past the GPU watchdog limit. Inert right now,
    // the wait is tied to XFB_WRITEBACK_ENABLED, so FlushWork's adaptive bound
    // is what holds submissions down.
    DrainPendingXfb(XfbDrain::Throttle);
    // After the drain, so segments it just released are stamped too. Frame end
    // is the first point at which every submission referencing one is known.
    m_transform_feedback.StampReleasedSegments();
    m_draw_counter = 0;
    m_frame_had_vertex_storage_writes = false;
    m_flush_work_counter = 0;
    ResolveOcclusionQueries(false);
    m_fence_manager.TickFrame();
    // Required, or the bucketed staging cache holds its high-water mark for the
    // rest of the session - every converted index buffer takes one.
    m_staging_pool.TickFrame();
    {
        std::scoped_lock lock{m_texture_cache.mutex};
        m_texture_cache.TickFrame();
    }
    {
        std::scoped_lock lock{m_buffer_cache.mutex};
        m_buffer_cache.TickFrame();
    }
}

bool RasterizerMetal::HasDrawTransformFeedback() {
    // Gated on this backend's XFB emulation, not a device feature: with it off
    // nothing fills the capture buffer, so macro.cpp's CPU fallback is better.
    return Metal::Options::TransformFeedback();
}


bool RasterizerMetal::ResolveBufferPredicate(MTL::Buffer*& out_buffer, u32& out_offset) {
    std::scoped_lock lock{m_buffer_cache.mutex};
    const auto [buffer, offset] = m_buffer_cache.ObtainCPUBuffer(
        m_gpu_predicate.predicate_cpu_addr, m_gpu_predicate.predicate_bytes,
        VideoCommon::ObtainBufferSynchronize::FullSynchronize,
        VideoCommon::ObtainBufferOperation::DoNothing);
    if (buffer == nullptr || buffer->Handle() == nullptr || (offset & 3u) != 0u) {
        return false;
    }
    out_buffer = buffer->Handle();
    out_offset = offset;
    if (m_scheduler.IsBufferWritten(out_buffer)) {
        m_scheduler.Flush();
    }
    return true;
}

bool RasterizerMetal::EvaluateGpuPredicateOnCpu() {
    if (!m_gpu_predicate.active || m_gpu_predicate.engine != maxwell3d) {
        return true;
    }
    if (m_gpu_predicate.cpu_resolved) {
        return m_gpu_predicate.cpu_result;
    }
    if (!m_gpu_predicate.from_buffer) {
        ResolveOcclusionQueriesAt(m_gpu_predicate.condition_addr, 24);
    }
    bool result = true;
    if (gpu_memory != nullptr) {
        Tegra::Engines::Maxwell3D::Regs::ReportSemaphore::Compare cmp{};
        if (m_gpu_predicate.from_buffer) {
            gpu_memory->ReadBlock(m_gpu_predicate.condition_addr, &cmp, sizeof(cmp));
        } else {
            gpu_memory->ReadBlockUnsafe(m_gpu_predicate.condition_addr, &cmp, sizeof(cmp));
        }
        using Mode = Tegra::Engines::Maxwell3D::Regs::RenderEnable::Mode;
        switch (m_gpu_predicate.mode) {
        case Mode::Conditional:
            result = cmp.initial_sequence != 0u && cmp.initial_mode != 0u;
            break;
        case Mode::IfEqual:
            result = cmp.initial_sequence == cmp.current_sequence &&
                     cmp.initial_mode == cmp.current_mode;
            break;
        case Mode::IfNotEqual:
            result = cmp.initial_sequence != cmp.current_sequence ||
                     cmp.initial_mode != cmp.current_mode;
            break;
        default:
            result = true;
            break;
        }
    }
    m_gpu_predicate.cpu_resolved = true;
    m_gpu_predicate.cpu_result = result;
    return result;
}

bool RasterizerMetal::AccelerateConditionalRendering() {
    m_gpu_predicate = {};
    if (!maxwell3d || !gpu_memory) {
        return false;
    }
    using RenderEnable = Tegra::Engines::Maxwell3D::Regs::RenderEnable;
    const auto& regs = maxwell3d->regs;

    if (regs.render_enable_override != RenderEnable::Override::UseRenderEnable ||
        regs.render_enable.mode == RenderEnable::Mode::True ||
        regs.render_enable.mode == RenderEnable::Mode::False) {
        return false;
    }

    const GPUVAddr addr = regs.render_enable.Address();
    const bool uses_b = regs.render_enable.mode != RenderEnable::Mode::Conditional;

    // Retire any occlusion queries whose GPU tick already finished (non-blocking).
    ResolveOcclusionQueries(false);

    bool has_q0 = false;
    bool has_q1 = false;
    bool misaligned = false;
    PendingOcclusion q0{};
    PendingOcclusion q1{};
    {
        std::scoped_lock lock{m_occlusion_mutex};
        for (const PendingOcclusion& query : m_pending_occlusion) {
            if (query.invalidated) {
                continue;
            }
            const u64 query_size = query.has_timeout ? 16u : 4u;
            if (query.gpu_addr < addr + 8u && addr < query.gpu_addr + query_size) {
                if (query.gpu_addr == addr) {
                    q0 = query;
                    has_q0 = true;
                } else {
                    misaligned = true;
                }
            }
            if (uses_b && query.gpu_addr < addr + 24u && addr + 16u < query.gpu_addr + query_size) {
                if (query.gpu_addr == addr + 16u) {
                    q1 = query;
                    has_q1 = true;
                } else {
                    misaligned = true;
                }
            }
        }
    }

    if (!has_q0 && !has_q1) {
        // If the predicate address lives in a GPU-modified BufferCache buffer (e.g.
        // compute/SSBO culling in Luigi's Mansion 3), evaluate it directly on the GPU
        // instead of falling back to ReadBlock() -> DownloadMemory() -> Scheduler::Wait().
        if (m_buffer_cache_runtime.GetIndexPass().HasConditionalDraw()) {
            const u32 pred_bytes = uses_b ? 24u : 8u;
            if (const auto cpu_addr = gpu_memory->GpuToCpuAddress(addr)) {
                std::scoped_lock lock{m_buffer_cache.mutex};
                if (m_buffer_cache.IsRegionGpuModified(*cpu_addr, pred_bytes)) {
                    const auto [buffer, offset] = m_buffer_cache.ObtainCPUBuffer(
                        *cpu_addr, pred_bytes, VideoCommon::ObtainBufferSynchronize::FullSynchronize,
                        VideoCommon::ObtainBufferOperation::DoNothing);
                    if (buffer != nullptr && buffer->Handle() != nullptr && (offset & 3u) == 0u) {
                        m_gpu_predicate = GpuPredicate{
                            .active = true,
                            .cpu_resolved = false,
                            .cpu_result = true,
                            .engine = maxwell3d,
                            .mode = regs.render_enable.mode,
                            .condition_addr = addr,
                            .from_buffer = true,
                            .predicate_cpu_addr = *cpu_addr,
                            .predicate_bytes = pred_bytes,
                            .wait_tick = 0u,
                        };
                        return true;
                    }
                }
            }
        }
        // Results are already in guest RAM: let Maxwell3D::ProcessQueryCondition
        // evaluate them directly on CPU with zero GPU work and zero stall.
        return false;
    }

    const bool slots_fit = (!has_q0 || (q0.end_slot >= q0.first_slot &&
                                        q0.end_slot - q0.first_slot <= VISIBILITY_SLOTS)) &&
                           (!has_q1 || (q1.end_slot >= q1.first_slot &&
                                        q1.end_slot - q1.first_slot <= VISIBILITY_SLOTS));
    if (misaligned || !slots_fit || !m_visibility_buffer ||
        !m_buffer_cache_runtime.GetIndexPass().HasConditionalDraw()) {
        ResolveOcclusionQueriesAt(addr, 24);
        return false;
    }

    // Use ReadBlockUnsafe so reading a retired operand on the same 4 KiB page as an
    // in-flight query does not trigger a synchronous QueryCache page flush.
    u64 val_a = 0;
    u64 val_b = 0;
    if (!has_q0 || !q0.has_timeout) {
        gpu_memory->ReadBlockUnsafe(addr, &val_a, sizeof(val_a));
    }
    if (uses_b && (!has_q1 || !q1.has_timeout)) {
        gpu_memory->ReadBlockUnsafe(addr + 16u, &val_b, sizeof(val_b));
    }

    m_gpu_predicate = GpuPredicate{
        .active = true,
        .cpu_resolved = false,
        .cpu_result = true,
        .engine = maxwell3d,
        .mode = regs.render_enable.mode,
        .condition_addr = addr,
        .a_from_gpu = has_q0,
        .a_is_64bit = has_q0 ? q0.has_timeout : true,
        .a_first_slot = has_q0 ? q0.first_slot : 0u,
        .a_end_slot = has_q0 ? q0.end_slot : 0u,
        .a_cpu_value = val_a,
        .b_from_gpu = has_q1,
        .b_is_64bit = has_q1 ? q1.has_timeout : true,
        .b_first_slot = has_q1 ? q1.first_slot : 0u,
        .b_end_slot = has_q1 ? q1.end_slot : 0u,
        .b_cpu_value = val_b,
        .wait_tick = std::max(has_q0 ? q0.tick : 0u, has_q1 ? q1.tick : 0u),
    };
    return true;
}

bool RasterizerMetal::AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                                            const Tegra::Engines::Fermi2D::Surface& dst,
                                            const Tegra::Engines::Fermi2D::Config& copy_config) {
    std::scoped_lock lock{m_texture_cache.mutex};
    return m_texture_cache.BlitImage(dst, src, copy_config);
}

Tegra::Engines::AccelerateDMAInterface& RasterizerMetal::AccessAccelerateDMA() {
    return m_accelerate_dma;
}

void RasterizerMetal::AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                               std::span<const u8> memory) {
    // The write is the point of this call - invalidating alone leaves the caches
    // re-reading memory nothing ever wrote. Kepler uploads the compute launch
    // descriptor this way, so dropping it zeroes program_start.
    auto cpu_addr = gpu_memory->GpuToCpuAddress(address);
    if (!cpu_addr) {
        gpu_memory->WriteBlock(address, memory.data(), copy_size);
        m_gpu.InvalidateGPUCache();
        return;
    }
    gpu_memory->WriteBlockUnsafe(address, memory.data(), copy_size);
    m_gpu.InvalidateRegion(*cpu_addr, copy_size);

    std::scoped_lock lock{m_buffer_cache.mutex};
    if (!m_buffer_cache.InlineMemory(*cpu_addr, copy_size, memory)) {
        m_gpu.InvalidateRegion(*cpu_addr, copy_size);
    }
}

void RasterizerMetal::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                        const VideoCore::DiskResourceLoadCallback& callback,
                                        std::string_view, std::string_view, std::string_view,
                                        VideoCore::ShaderImportRequest*) {
    m_pipeline_cache.LoadDiskResources(title_id, stop_loading, callback);
}

MTL::Texture* RasterizerMetal::GetFramebufferTexture(const Tegra::FramebufferConfig& config,
                                                     u32* unscaled_width, u32* unscaled_height) {
    // Already a device address - do not run it through GpuToCpuAddress.
    const DAddr framebuffer_addr = config.address + config.offset;
    if (framebuffer_addr == 0) {
        return nullptr;
    }

    std::scoped_lock lock{m_texture_cache.mutex};
    const auto [image_view, is_scaled] =
        m_texture_cache.TryFindFramebufferImageView(config, framebuffer_addr);
    if (image_view == nullptr) {
        return nullptr;
    }

    // Reported so the caller can normalise the crop, which is in GUEST pixels.
    // The handle is the shader-visible view, not RenderTarget(): the shared
    // cache swizzles w_source to OneFloat for an opaque framebuffer, and
    // RenderTarget() carries no swizzle (Metal will not render into a swizzled
    // view) so it would hand the compositor the guest's real alpha.
    if (unscaled_width != nullptr) {
        *unscaled_width = image_view->size.width;
    }
    if (unscaled_height != nullptr) {
        *unscaled_height = image_view->size.height;
    }
    return image_view->Handle(Shader::TextureType::Color2D);
}

namespace {

void SetupPipelineStateDirtyFlags(Tegra::Engines::Maxwell3D::DirtyState::Tables& tables) {
    using Regs = Tegra::Engines::Maxwell3D::Regs;
#define PSD_OFF(field) MAXWELL3D_REG_INDEX(field)
#define PSD_NUM(field) (sizeof(Regs::field) / size_t{4})
    std::array<bool, Regs::NUM_REGS> ignored{};
    const auto ignore = [&](size_t begin, size_t count) {
        std::fill_n(ignored.begin() + begin, std::min(count, Regs::NUM_REGS - begin), true);
    };
#define PSD_IGNORE(field) ignore(PSD_OFF(field), PSD_NUM(field))
    PSD_IGNORE(nop);
    PSD_IGNORE(notify);
    PSD_IGNORE(wait_for_idle);
    PSD_IGNORE(load_mme);
    PSD_IGNORE(shadow_ram_control);
    PSD_IGNORE(upload);
    PSD_IGNORE(launch_dma);
    PSD_IGNORE(inline_data);
    PSD_IGNORE(sync_info);
    PSD_IGNORE(viewport_transform);
    PSD_IGNORE(viewports);
    PSD_IGNORE(clear_rect);
    PSD_IGNORE(vertex_buffer);
    PSD_IGNORE(clear_color);
    PSD_IGNORE(clear_depth);
    PSD_IGNORE(clear_stencil);
    PSD_IGNORE(scissor_test);
    PSD_IGNORE(stencil_back_ref);
    PSD_IGNORE(stencil_front_ref);
    PSD_IGNORE(tiled_cache_barrier);
    PSD_IGNORE(fragment_barrier);
    PSD_IGNORE(surface_clip);
    PSD_IGNORE(draw_texture);
    PSD_IGNORE(clear_control);
    PSD_IGNORE(vertex_id_base);
    PSD_IGNORE(vertex_array_instance_first);
    PSD_IGNORE(vertex_array_instance_subsequent);
    PSD_IGNORE(draw_auto_byte_count);
    PSD_IGNORE(draw_auto_stride);
    PSD_IGNORE(draw_auto_start_byte_count);
    PSD_IGNORE(inline_index_4x8);
    PSD_IGNORE(inline_index_2x16);
    PSD_IGNORE(draw_inline_index);
    PSD_IGNORE(blend_color);
    PSD_IGNORE(global_base_vertex_index);
    PSD_IGNORE(global_base_instance_index);
    PSD_IGNORE(zpass_pixel_count_enable);
    PSD_IGNORE(clear_report_value);
    PSD_IGNORE(render_enable);
    PSD_IGNORE(tex_sampler);
    PSD_IGNORE(slope_scale_depth_bias);
    PSD_IGNORE(tex_header);
    PSD_IGNORE(depth_bias);
    PSD_IGNORE(draw);
    PSD_IGNORE(index_buffer);
    PSD_IGNORE(index_buffer32_first);
    PSD_IGNORE(index_buffer16_first);
    PSD_IGNORE(index_buffer8_first);
    PSD_IGNORE(index_buffer32_subsequent);
    PSD_IGNORE(index_buffer16_subsequent);
    PSD_IGNORE(index_buffer8_subsequent);
    PSD_IGNORE(depth_bias_clamp);
    PSD_IGNORE(clear_surface);
    PSD_IGNORE(report_semaphore);
    PSD_IGNORE(pipelines);
    PSD_IGNORE(const_buffer);
    PSD_IGNORE(bind_groups);
#undef PSD_IGNORE
#undef PSD_NUM
#undef PSD_OFF
    for (size_t reg = 0; reg < Regs::NUM_REGS; ++reg) {
        if (ignored[reg]) {
            continue;
        }
        // A register with both entries taken (render targets, vertex stream strides) is
        // compared directly by UpdatePipelineState instead.
        if (tables[0][reg] == VideoCommon::Dirty::NullEntry) {
            tables[0][reg] = PIPELINE_STATE_DIRTY;
        } else if (tables[1][reg] == VideoCommon::Dirty::NullEntry) {
            tables[1][reg] = PIPELINE_STATE_DIRTY;
        }
    }
    // Tag all registers that affect viewport/scissor computation so DrawImpl can skip
    // rebuilding all 16 Metal viewports/scissors when unchanged.
    const auto tag_vp = [&](size_t begin, size_t count) {
        const size_t end = std::min(begin + count, Regs::NUM_REGS);
        for (size_t reg = begin; reg < end; ++reg) {
            if (tables[0][reg] == VideoCommon::Dirty::NullEntry) {
                tables[0][reg] = VIEWPORT_SCISSOR_DIRTY;
            } else if (tables[1][reg] == VideoCommon::Dirty::NullEntry) {
                tables[1][reg] = VIEWPORT_SCISSOR_DIRTY;
            }
        }
    };
    tag_vp(MAXWELL3D_REG_INDEX(viewport_transform), sizeof(Regs::viewport_transform) / 4);
    tag_vp(MAXWELL3D_REG_INDEX(scissor_test), sizeof(Regs::scissor_test) / 4);
    tag_vp(MAXWELL3D_REG_INDEX(surface_clip), sizeof(Regs::surface_clip) / 4);
    tag_vp(MAXWELL3D_REG_INDEX(viewport_scale_offset_enabled),
           sizeof(Regs::viewport_scale_offset_enabled) / 4);
    tag_vp(MAXWELL3D_REG_INDEX(window_origin), sizeof(Regs::window_origin) / 4);
    tag_vp(MAXWELL3D_REG_INDEX(depth_mode), sizeof(Regs::depth_mode) / 4);
    {
        const size_t reg = MAXWELL3D_REG_INDEX(transform_feedback_enabled);
        if (tables[0][reg] == VideoCommon::Dirty::NullEntry) {
            tables[0][reg] = XFB_SESSION_DIRTY;
        } else if (tables[1][reg] == VideoCommon::Dirty::NullEntry) {
            tables[1][reg] = XFB_SESSION_DIRTY;
        }
    }
}
} // Anonymous namespace

void RasterizerMetal::InitializeChannel(Tegra::Control::ChannelState& channel) {
    // Required: without it the dirty tables stay empty, writing a render-target
    // register never raises Dirty::RenderTargets, and UpdateRenderTargets
    // early-outs after the first call, so only one render target ever exists.
    VideoCommon::Dirty::SetupDirtyFlags(channel.maxwell_3d->dirty.tables);
    SetupPipelineStateDirtyFlags(channel.maxwell_3d->dirty.tables);

    CreateChannel(channel);
    {
        std::scoped_lock lock{m_texture_cache.mutex, m_buffer_cache.mutex};
        m_texture_cache.CreateChannel(channel);
        m_buffer_cache.CreateChannel(channel);
    }
    m_pipeline_cache.CreateChannel(channel);
}

void RasterizerMetal::BindChannel(Tegra::Control::ChannelState& channel) {
    m_gpu_predicate = {};
    BindToChannel(channel.bind_id);
    {
        std::scoped_lock lock{m_texture_cache.mutex, m_buffer_cache.mutex};
        m_texture_cache.BindToChannel(channel.bind_id);
        m_buffer_cache.BindToChannel(channel.bind_id);
    }
    m_pipeline_cache.BindToChannel(channel.bind_id);

    // The shared buffer cache only rebinds streams whose dirty flag is set, and
    // this backend's vertex_bindings array is global rather than per channel, so
    // an unmarked slot keeps the previous channel's buffer.
    if (maxwell3d != nullptr) {
        auto& flags = maxwell3d->dirty.flags;
        flags[VideoCommon::Dirty::VertexBuffers] = true;
        for (u32 index = VideoCommon::Dirty::VertexBuffer0;
             index <= VideoCommon::Dirty::VertexBuffer31; ++index) {
            flags[index] = true;
        }
    }
}

void RasterizerMetal::ReleaseChannel(s32 channel_id) {
    m_gpu_predicate = {};
    // Drain pending occlusion queries before erasing the channel so no in-flight
    // query retains a dangling Tegra::MemoryManager* after teardown.
    ResolveOcclusionQueries(true);
    EraseChannel(channel_id);
    {
        std::scoped_lock lock{m_texture_cache.mutex, m_buffer_cache.mutex};
        m_texture_cache.EraseChannel(channel_id);
        m_buffer_cache.EraseChannel(channel_id);
    }
    m_pipeline_cache.EraseChannel(channel_id);
}

} // namespace Metal
