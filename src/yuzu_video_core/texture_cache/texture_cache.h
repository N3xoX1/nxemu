// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <bit>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <boost/container/small_vector.hpp>
#include <unordered_set>

#include "video_settings.h"
#include "yuzu_common/alignment.h"
#include "yuzu_common/settings.h"
#include "yuzu_shader_recompiler/exception.h"
#include "yuzu_video_core/control/channel_state.h"
#include "yuzu_video_core/dirty_flags.h"
#include "yuzu_video_core/engines/kepler_compute.h"
#include "yuzu_video_core/guest_memory.h"
#include "yuzu_video_core/textures/swizzle_range.h"
#include "yuzu_video_core/host1x/gpu_device_memory_manager.h"
#include "yuzu_video_core/texture_cache/format_lookup_table.h"
#include "yuzu_video_core/texture_cache/observed_readback_plan.h"
#include "yuzu_video_core/texture_cache/full_clear.h"
#include "yuzu_video_core/texture_cache/image_copy_coverage.h"
#include "yuzu_video_core/texture_cache/image_view_base.h"
#include "yuzu_video_core/texture_cache/image_view_invalidation.h"
#include "yuzu_video_core/texture_cache/samples_helper.h"
#include "yuzu_video_core/texture_cache/texture_cache_base.h"
#include "yuzu_video_core/texture_cache/util.h"

namespace VideoCommon {

using Tegra::Texture::TICEntry;
using Tegra::Texture::TSCEntry;
using VideoCore::Surface::GetFormatType;
using VideoCore::Surface::PixelFormat;
using VideoCore::Surface::SurfaceType;
using namespace Common::Literals;

template <class P>
TextureCache<P>::TextureCache(Runtime& runtime_, Tegra::MaxwellDeviceMemoryManager& device_memory_)
    : runtime{runtime_}, device_memory{device_memory_} {
    // Configure null sampler
    TSCEntry sampler_descriptor{};
    sampler_descriptor.min_filter.Assign(Tegra::Texture::TextureFilter::Linear);
    sampler_descriptor.mag_filter.Assign(Tegra::Texture::TextureFilter::Linear);
    sampler_descriptor.mipmap_filter.Assign(Tegra::Texture::TextureMipmapFilter::Linear);
    sampler_descriptor.cubemap_anisotropy.Assign(1);

    // These values were chosen based on typical peak swizzle data sizes seen in some titles
    static constexpr size_t SWIZZLE_DATA_BUFFER_INITIAL_CAPACITY = 8_MiB;
    static constexpr size_t UNSWIZZLE_DATA_BUFFER_INITIAL_CAPACITY = 1_MiB;
    swizzle_data_buffer.resize_destructive(SWIZZLE_DATA_BUFFER_INITIAL_CAPACITY);
    unswizzle_data_buffer.resize_destructive(UNSWIZZLE_DATA_BUFFER_INITIAL_CAPACITY);

    // Make sure the first index is reserved for the null resources
    // This way the null resource becomes a compile time constant
    void(slot_images.insert(NullImageParams{}));
    void(slot_image_views.insert(runtime, NullImageViewParams{}));
    void(slot_samplers.insert(runtime, sampler_descriptor));

    if constexpr (HAS_DEVICE_MEMORY_INFO) {
        const s64 device_local_memory = static_cast<s64>(runtime.GetDeviceLocalMemory());
        const s64 min_spacing_expected = device_local_memory - 1_GiB;
        const s64 min_spacing_critical = device_local_memory - 512_MiB;
        const s64 mem_threshold = std::min(device_local_memory, TARGET_THRESHOLD);
        const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
        const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
        expected_memory = static_cast<u64>(
            std::max(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                     DEFAULT_EXPECTED_MEMORY));
        critical_memory = static_cast<u64>(
            std::max(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                     DEFAULT_CRITICAL_MEMORY));
        minimum_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
    } else {
        expected_memory = DEFAULT_EXPECTED_MEMORY + 512_MiB;
        critical_memory = DEFAULT_CRITICAL_MEMORY + 1_GiB;
        minimum_memory = 0;
    }
}

template <class P>
TextureCache<P>::~TextureCache() {
    DrainGCReadbacks();
}

template <class P>
void TextureCache<P>::DrainGCReadbacks() {
    if constexpr (SUPPORTS_ASYNC_GC) {
        for (auto& readback : gc_readbacks) {
            if (readback.id) {
                // RunGarbageCollector has already submitted these copies. Waiting for
                // their ticks avoids submitting a fresh batch during destruction.
                runtime.WaitReadback(readback.tick);
                runtime.FreeDeferredStagingBuffer(readback.map);
            }
        }
        gc_readbacks.clear();
        gc_readback_ids.clear();
        gc_readback_bytes = 0;
    }
}

template <class P>
bool TextureCache<P>::QueueGCReadback(ImageId id) {
    if constexpr (!SUPPORTS_ASYNC_GC) {
        return false;
    } else {
        auto& image = slot_images[id];
        if (image.unswizzled_size_bytes == 0 || image.unswizzled_size_bytes > GC_READBACK_LIMIT) {
            return false;
        }
        // The image may belong to a channel other than the bound one; the poll writes it
        // back through its own address space.
        const auto address_space = ImageAddressSpace(image);
        if (!address_space) {
            return false;
        }
        if (gc_readback_ids.contains(id)) {
            return true;
        }
        const size_t capacity = std::bit_ceil(size_t(image.unswizzled_size_bytes));
        if (capacity > GC_READBACK_LIMIT - gc_readback_bytes) {
            return false;
        }
        auto map = runtime.DownloadStagingBuffer(image.unswizzled_size_bytes, true);
        image.DownloadMemory(map, FullDownloadCopies(image.info));
        gc_readbacks.push_back({id, image.modification_tick, lru_cache.GetTick(image.lru_index),
                                runtime.GetReadbackTick(), *address_space,
                                image.gpu_page_table_index, map});
        gc_readback_ids.insert(id);
        gc_readback_bytes += map.mapped_span.size_bytes();
        return true;
    }
}

template <class P>
void TextureCache<P>::PollGCReadbacks() {
    if constexpr (SUPPORTS_ASYNC_GC) {
        while (!gc_readbacks.empty()) {
            auto& readback = gc_readbacks.front();
            if (!readback.id) {
                gc_readbacks.pop_front();
                continue;
            }
            if (!runtime.IsReadbackReady(readback.tick)) {
                break;
            }
            const ImageId id = readback.id;
            auto& image = slot_images[id];
            const auto owner = address_spaces.find(readback.address_space);
            // Do not permanently reject an unchanged image because an alias is
            // newer: every GC pass would cancel and requeue it without retiring it.
            const bool unchanged =
                image.IsSafeDownload() &&
                False(image.flags & ImageFlagBits::BadOverlap) &&
                image.modification_tick == readback.modification_tick &&
                lru_cache.GetTick(image.lru_index) == readback.last_use_tick &&
                owner != address_spaces.end() &&
                owner->second.storage_id * 2 == readback.storage_index;
            gc_readback_ids.erase(id);
            if (unchanged) {
                const auto copies = FullDownloadCopies(image.info);
                SwizzleImage(*owner->second.gpu_memory, image.gpu_addr, image.info, copies,
                             readback.map.mapped_span, swizzle_data_buffer);
                if (True(image.flags & ImageFlagBits::Tracked)) {
                    UntrackImage(image, id);
                }
                UnregisterImage(id);
                DeleteImage(id, false, &gc_removed_views);
            }
            gc_readback_bytes -= readback.map.mapped_span.size_bytes();
            runtime.FreeDeferredStagingBuffer(readback.map);
            gc_readbacks.pop_front();
        }
    }
}

template <class P>
std::optional<size_t> TextureCache<P>::ImageAddressSpace(const ImageBase& image) const {
    if (image.gpu_page_table_index == channel_state->gpu_page_table_index) {
        return current_address_space;
    }
    for (const auto& [id, address_space] : address_spaces) {
        if (address_space.storage_id * 2 == image.gpu_page_table_index) {
            return id;
        }
    }
    return std::nullopt;
}

template <class P>
Tegra::MemoryManager* TextureCache<P>::ImageGpuMemory(const ImageBase& image) const {
    if (image.gpu_page_table_index == channel_state->gpu_page_table_index) {
        return gpu_memory;
    }
    const auto id = ImageAddressSpace(image);
    return id ? address_spaces.at(*id).gpu_memory : nullptr;
}

template <class P>
void TextureCache<P>::CancelPendingDownloads(ImageId id) {
    // The slot is erased (and may be reused) before queued downloads are processed.
    // Uncommitted copies were not recorded yet, so drop them; committed copies already
    // target the sentenced image and only need their publication skipped.
    std::erase_if(uncommitted_downloads, [id](const PendingDownload& download) {
        return download.is_swizzle && download.object_id == id;
    });
    for (auto& downloads : committed_downloads) {
        for (PendingDownload& download : downloads) {
            if (download.is_swizzle && download.object_id == id) {
                download.discarded = true;
            }
        }
    }
}

template <class P>
void TextureCache<P>::CancelGCReadback(ImageId id) {
    if constexpr (SUPPORTS_ASYNC_GC) {
        if (gc_readback_ids.erase(id) == 0) {
            return;
        }
        for (auto& readback : gc_readbacks) {
            if (readback.id != id) {
                continue;
            }
            // The slot may be reused after DeleteImage. Complete its old copy
            // before freeing either its source image or its staging allocation.
            runtime.WaitReadback(readback.tick);
            gc_readback_bytes -= readback.map.mapped_span.size_bytes();
            runtime.FreeDeferredStagingBuffer(readback.map);
            readback.id = {};
            break;
        }
    }
}

template <class P>
void TextureCache<P>::RunGarbageCollector() {
    gc_removed_views.clear();
    PollGCReadbacks();
    const size_t queued_before = gc_readbacks.size();
    bool high_priority_mode = false;
    bool aggressive_mode = false;
    u64 ticks_to_destroy = 0;
    size_t num_iterations = 0;

    const auto Configure = [&](bool allow_aggressive) {
        high_priority_mode = total_used_memory >= expected_memory;
        aggressive_mode = allow_aggressive && total_used_memory >= critical_memory;
        ticks_to_destroy = aggressive_mode ? 10ULL : high_priority_mode ? 25ULL : 50ULL;
        num_iterations = aggressive_mode ? 40 : (high_priority_mode ? 20 : 10);
    };
    const auto Cleanup = [this, &num_iterations, &high_priority_mode,
                          &aggressive_mode](ImageId image_id) {
        if (num_iterations == 0) {
            return true;
        }
        --num_iterations;
        auto& image = slot_images[image_id];
        if (True(image.flags & ImageFlagBits::IsDecoding)) {
            // This image is still being decoded, deleting it will invalidate the slot
            // used by the async decoder thread.
            return false;
        }
        if (!aggressive_mode && True(image.flags & ImageFlagBits::CostlyLoad)) {
            return false;
        }
        if (gc_readback_ids.contains(image_id)) {
            return false;
        }
        const bool must_download =
            image.IsSafeDownload() && False(image.flags & ImageFlagBits::BadOverlap);
        if (!high_priority_mode && must_download) {
            return false;
        }
        if (must_download && QueueGCReadback(image_id)) {
            return false;
        }
        // The collector may run while another channel is bound. Write the contents back to
        // the address space the image belongs to; nothing can read them once it is gone.
        Tegra::MemoryManager* const image_gpu_memory =
            must_download ? ImageGpuMemory(image) : nullptr;
        if (image_gpu_memory) {
            auto map = runtime.DownloadStagingBuffer(image.unswizzled_size_bytes);
            const auto copies = FullDownloadCopies(image.info);
            image.DownloadMemory(map, copies);
            runtime.Finish();
            SwizzleImage(*image_gpu_memory, image.gpu_addr, image.info, copies, map.mapped_span,
                         swizzle_data_buffer);
        }
        if (True(image.flags & ImageFlagBits::Tracked)) {
            UntrackImage(image, image_id);
        }
        UnregisterImage(image_id);
        DeleteImage(image_id, image.scale_tick > frame_tick + 5, &gc_removed_views);
        if (total_used_memory < critical_memory) {
            if (aggressive_mode) {
                // Sink the aggresiveness.
                num_iterations >>= 2;
                aggressive_mode = false;
                return false;
            }
            if (high_priority_mode && total_used_memory < expected_memory) {
                num_iterations >>= 1;
                high_priority_mode = false;
            }
        }
        return false;
    };

    // Try to remove anything old enough and not high priority.
    Configure(false);
    lru_cache.ForEachItemBelow(frame_tick - ticks_to_destroy, Cleanup);

    // If pressure is still too high, prune aggressively.
    if (total_used_memory >= critical_memory) {
        Configure(true);
        lru_cache.ForEachItemBelow(frame_tick - ticks_to_destroy, Cleanup);
    }

    if constexpr (SUPPORTS_ASYNC_GC) {
        if (gc_readbacks.size() > queued_before) {
            runtime.SubmitReadbacks();
        }
    }
    // GC never creates image views. Their retired IDs can be invalidated as one
    // batch before any draw or view allocation can reuse them.
    RemoveImageViewReferences(gc_removed_views);
    RemoveFramebuffers(gc_removed_views);
}

template <class P>
void TextureCache<P>::TickFrame() {
    // If we can obtain the memory info, use it instead of the estimate.
    if (runtime.CanReportMemoryUsage()) {
        total_used_memory = runtime.GetDeviceMemoryUsage();
    }
    if (total_used_memory > minimum_memory) {
        RunGarbageCollector();
    }
    sentenced_images.Tick();
    sentenced_framebuffers.Tick();
    sentenced_image_view.Tick();
    TickAsyncDecode();

    runtime.TickFrame();
    ++frame_tick;

    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        for (auto& buffer : async_buffers_death_ring) {
            runtime.FreeDeferredStagingBuffer(buffer);
        }
        async_buffers_death_ring.clear();
    }
}

template <class P>
const typename P::ImageView& TextureCache<P>::GetImageView(ImageViewId id) const noexcept {
    return slot_image_views[id];
}

template <class P>
typename P::ImageView& TextureCache<P>::GetImageView(ImageViewId id) noexcept {
    return slot_image_views[id];
}

template <class P>
typename P::ImageView& TextureCache<P>::GetImageView(u32 index) noexcept {
    const auto image_view_id = VisitImageView(channel_state->graphics_image_table,
                                              channel_state->graphics_image_view_ids, index);
    return slot_image_views[image_view_id];
}

template <class P>
void TextureCache<P>::MarkModification(ImageId id) noexcept {
    MarkModification(slot_images[id], id);
}

template <class P>
template <bool has_blacklists>
void TextureCache<P>::FillGraphicsImageViews(std::span<ImageViewInOut> views) {
    FillImageViews<has_blacklists>(channel_state->graphics_image_table,
                                   channel_state->graphics_image_view_ids, views);
}

template <class P>
void TextureCache<P>::FillComputeImageViews(std::span<ImageViewInOut> views) {
    FillImageViews<true>(channel_state->compute_image_table, channel_state->compute_image_view_ids,
                         views);
}

template <class P>
void TextureCache<P>::CheckFeedbackLoop(std::span<const ImageViewInOut> views) {
    if (!videoSettings.barrier_feedback_loops) {
        return;
    }

    const bool requires_barrier = [&] {
        // Only writes since the last barrier can be missing from sampled reads, and they went to
        // the current render targets: the render pass changes when they change.
        const u64 written_tick = runtime.AttachmentBarrierTick() + 1;
        std::array<const ImageViewBase*, NUM_RT + 1> target_views;
        size_t num_targets = 0;
        const auto add_target = [&](ImageViewId target_id) {
            if (!target_id) {
                return;
            }
            const ImageViewBase& target = slot_image_views[target_id];
            if (slot_images[target.image_id].attachment_write_tick == written_tick) {
                target_views[num_targets++] = &target;
            }
        };
        for (const auto& ct_view_id : render_targets.color_buffer_ids) {
            add_target(ct_view_id);
        }
        add_target(render_targets.depth_buffer_id);
        if (num_targets == 0) {
            return false;
        }
        const std::span targets{target_views.data(), num_targets};
        const auto overlaps = [&](const ImageViewBase& lhs, const ImageViewBase& rhs) {
            if (lhs.image_id != rhs.image_id) {
                return false;
            }
            // Slices of 3D images are not described by the layer range
            if (slot_images[lhs.image_id].info.type == ImageType::e3D) {
                return true;
            }
            const SubresourceRange& a = lhs.range;
            const SubresourceRange& b = rhs.range;
            return a.base.level < b.base.level + b.extent.levels &&
                   b.base.level < a.base.level + a.extent.levels &&
                   a.base.layer < b.base.layer + b.extent.layers &&
                   b.base.layer < a.base.layer + a.extent.layers;
        };
        // Large bindless arrays repeat their views, often back to back
        ImageViewId last_checked{};
        for (const auto& view : views) {
            // A whole texture table usually holds the render targets too
            if (!view.id || view.is_table || view.id == last_checked) {
                continue;
            }
            last_checked = view.id;
            const ImageViewBase& sampled = slot_image_views[view.id];
            for (const ImageViewBase* target : targets) {
                if (overlaps(sampled, *target)) {
                    return true;
                }
            }
        }

        return false;
    }();

    if (requires_barrier) {
        runtime.BarrierFeedbackLoop();
    }
}

template <class P>
void TextureCache<P>::MarkRenderTargetsWritten(u32 color_mask, bool depth) {
    const u64 written_tick = runtime.AttachmentBarrierTick() + 1;
    const auto mark = [&](ImageViewId target_id) {
        if (target_id) {
            slot_images[slot_image_views[target_id].image_id].attachment_write_tick = written_tick;
        }
    };
    for (size_t index = 0; index < NUM_RT; ++index) {
        if ((color_mask >> index) & 1) {
            mark(render_targets.color_buffer_ids[index]);
        }
    }
    if (depth) {
        mark(render_targets.depth_buffer_id);
    }
}

template <class P>
typename P::Sampler* TextureCache<P>::GetGraphicsSampler(u32 index) {
    return &slot_samplers[GetGraphicsSamplerId(index)];
}

template <class P>
typename P::Sampler* TextureCache<P>::GetComputeSampler(u32 index) {
    return &slot_samplers[GetComputeSamplerId(index)];
}

template <class P>
SamplerId TextureCache<P>::GetGraphicsSamplerId(u32 index) {
    if (index > channel_state->graphics_sampler_table.Limit()) {
        LOG_DEBUG(HW_GPU, "Invalid sampler index={}", index);
        return NULL_SAMPLER_ID;
    }
    const auto [descriptor, is_new] = channel_state->graphics_sampler_table.Read(index);
    SamplerId& id = channel_state->graphics_sampler_ids[index];
    if (is_new) {
        id = FindSampler(descriptor);
    }
    return id;
}

template <class P>
SamplerId TextureCache<P>::GetComputeSamplerId(u32 index) {
    if (index > channel_state->compute_sampler_table.Limit()) {
        LOG_DEBUG(HW_GPU, "Invalid sampler index={}", index);
        return NULL_SAMPLER_ID;
    }
    const auto [descriptor, is_new] = channel_state->compute_sampler_table.Read(index);
    SamplerId& id = channel_state->compute_sampler_ids[index];
    if (is_new) {
        id = FindSampler(descriptor);
    }
    return id;
}

template <class P>
const typename P::Sampler& TextureCache<P>::GetSampler(SamplerId id) const noexcept {
    return slot_samplers[id];
}

template <class P>
typename P::Sampler& TextureCache<P>::GetSampler(SamplerId id) noexcept {
    return slot_samplers[id];
}

template <class P>
void TextureCache<P>::SynchronizeGraphicsDescriptors() {
    using SamplerBinding = Tegra::Engines::Maxwell3D::Regs::SamplerBinding;
    const bool linked_tsc = maxwell3d->regs.sampler_binding == SamplerBinding::ViaHeaderBinding;
    const u32 tic_limit = maxwell3d->regs.tex_header.limit;
    const u32 tsc_limit = linked_tsc ? tic_limit : maxwell3d->regs.tex_sampler.limit;
    if (channel_state->graphics_sampler_table.Synchronize(maxwell3d->regs.tex_sampler.Address(),
                                                          tsc_limit)) {
        channel_state->graphics_sampler_ids.resize(tsc_limit + 1, CORRUPT_ID);
    }
    if (channel_state->graphics_image_table.Synchronize(maxwell3d->regs.tex_header.Address(),
                                                        tic_limit)) {
        channel_state->graphics_image_view_ids.resize(tic_limit + 1, CORRUPT_ID);
    }
}

template <class P>
void TextureCache<P>::SynchronizeComputeDescriptors() {
    const bool linked_tsc = kepler_compute->launch_description.linked_tsc;
    const u32 tic_limit = kepler_compute->regs.tic.limit;
    const u32 tsc_limit = linked_tsc ? tic_limit : kepler_compute->regs.tsc.limit;
    const GPUVAddr tsc_gpu_addr = kepler_compute->regs.tsc.Address();
    if (channel_state->compute_sampler_table.Synchronize(tsc_gpu_addr, tsc_limit)) {
        channel_state->compute_sampler_ids.resize(tsc_limit + 1, CORRUPT_ID);
    }
    if (channel_state->compute_image_table.Synchronize(kepler_compute->regs.tic.Address(),
                                                       tic_limit)) {
        channel_state->compute_image_view_ids.resize(tic_limit + 1, CORRUPT_ID);
    }
}

template <class P>
bool TextureCache<P>::IsCompute3DImage(u32 index, bool is_integer, bool is_signed) {
    auto& table = channel_state->compute_image_table;
    if (index > table.Limit()) {
        return false;
    }
    const auto [tic, is_new] = table.Read(index);
    if (is_new) {
        channel_state->compute_image_view_ids[index] = CORRUPT_ID;
    }
    if (tic.texture_type != Tegra::Texture::TextureType::Texture3D) {
        return false;
    }
    const auto format = PixelFormatFromTextureInfo(
        tic.format, tic.r_type, tic.g_type, tic.b_type, tic.a_type, tic.srgb_conversion);
    return VideoCore::Surface::IsPixelFormatInteger(format) == is_integer &&
           (tic.r_type == Tegra::Texture::ComponentType::SINT) == is_signed;
}

template <class P>
bool TextureCache<P>::RescaleRenderTargets(u32 clear_layer_count) {
    auto& flags = maxwell3d->dirty.flags;
    u32 scale_rating = 0;
    bool rescaled = false;
    std::array<ImageId, NUM_RT> tmp_color_images{};
    ImageId tmp_depth_image{};
    do {
        flags[Dirty::RenderTargets] = false;

        has_deleted_images = false;
        // Render target control is used on all render targets, so force look ups when this one is
        // up
        const bool force = flags[Dirty::RenderTargetControl];
        flags[Dirty::RenderTargetControl] = false;

        scale_rating = 0;
        bool any_rescaled = false;
        bool can_rescale = true;
        const auto check_rescale = [&](ImageViewId view_id, ImageId& id_save) {
            if (view_id != NULL_IMAGE_VIEW_ID && view_id != ImageViewId{}) {
                const auto& view = slot_image_views[view_id];
                const auto image_id = view.image_id;
                id_save = image_id;
                auto& image = slot_images[image_id];
                can_rescale &= ImageCanRescale(image);
                any_rescaled |= True(image.flags & ImageFlagBits::Rescaled) ||
                                GetFormatType(image.info.format) != SurfaceType::ColorTexture;
                scale_rating = std::max<u32>(scale_rating, image.scale_tick <= frame_tick
                                                               ? image.scale_rating + 1U
                                                               : image.scale_rating);
            } else {
                id_save = CORRUPT_ID;
            }
        };
        for (size_t index = 0; index < NUM_RT; ++index) {
            ImageViewId& color_buffer_id = render_targets.color_buffer_ids[index];
            if (flags[Dirty::ColorBuffer0 + index] || force) {
                flags[Dirty::ColorBuffer0 + index] = false;
                BindRenderTarget(&color_buffer_id, FindColorBuffer(index, clear_layer_count));
            }
            check_rescale(color_buffer_id, tmp_color_images[index]);
        }
        if (flags[Dirty::ZetaBuffer] || force) {
            flags[Dirty::ZetaBuffer] = false;
            BindRenderTarget(&render_targets.depth_buffer_id, FindDepthBuffer(clear_layer_count));
        }
        check_rescale(render_targets.depth_buffer_id, tmp_depth_image);

        if (can_rescale) {
            rescaled = any_rescaled || scale_rating >= 2;
            const auto scale_up = [this](ImageId image_id) {
                if (image_id != CORRUPT_ID) {
                    Image& image = slot_images[image_id];
                    ScaleUp(image);
                }
            };
            if (rescaled) {
                for (size_t index = 0; index < NUM_RT; ++index) {
                    scale_up(tmp_color_images[index]);
                }
                scale_up(tmp_depth_image);
                scale_rating = 2;
            }
        } else {
            rescaled = false;
            const auto scale_down = [this](ImageId image_id) {
                if (image_id != CORRUPT_ID) {
                    Image& image = slot_images[image_id];
                    ScaleDown(image);
                }
            };
            for (size_t index = 0; index < NUM_RT; ++index) {
                scale_down(tmp_color_images[index]);
            }
            scale_down(tmp_depth_image);
            scale_rating = 1;
        }
    } while (has_deleted_images);
    const auto set_rating = [this, scale_rating](ImageId image_id) {
        if (image_id != CORRUPT_ID) {
            Image& image = slot_images[image_id];
            image.scale_rating = scale_rating;
            if (image.scale_tick <= frame_tick) {
                image.scale_tick = frame_tick + 1;
            }
        }
    };
    for (size_t index = 0; index < NUM_RT; ++index) {
        set_rating(tmp_color_images[index]);
    }
    set_rating(tmp_depth_image);

    return rescaled;
}

template <class P>
void TextureCache<P>::UpdateRenderTargets(bool is_clear, u32 clear_layer_count) {
    using namespace VideoCommon::Dirty;
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::RenderTargets]) {
        for (size_t index = 0; index < NUM_RT; ++index) {
            ImageViewId& color_buffer_id = render_targets.color_buffer_ids[index];
            PrepareImageView(color_buffer_id, true, is_clear && IsFullClear(color_buffer_id));
            MarkRenderTarget(color_buffer_id);
        }
        const ImageViewId depth_buffer_id = render_targets.depth_buffer_id;
        PrepareImageView(depth_buffer_id, true, is_clear && IsFullClear(depth_buffer_id));
        MarkRenderTarget(depth_buffer_id);
        return;
    }

    const bool rescaled = RescaleRenderTargets(is_clear ? clear_layer_count : 0);
    if (is_rescaling != rescaled) {
        flags[Dirty::RescaleViewports] = true;
        flags[Dirty::RescaleScissors] = true;
        is_rescaling = rescaled;
    }

    for (size_t index = 0; index < NUM_RT; ++index) {
        ImageViewId& color_buffer_id = render_targets.color_buffer_ids[index];
        PrepareImageView(color_buffer_id, true, is_clear && IsFullClear(color_buffer_id));
        MarkRenderTarget(color_buffer_id);
    }
    const ImageViewId depth_buffer_id = render_targets.depth_buffer_id;

    PrepareImageView(depth_buffer_id, true, is_clear && IsFullClear(depth_buffer_id));
    MarkRenderTarget(depth_buffer_id);

    for (size_t index = 0; index < NUM_RT; ++index) {
        render_targets.draw_buffers[index] = static_cast<u8>(maxwell3d->regs.rt_control.Map(index));
    }
    u32 up_scale = 1;
    u32 down_shift = 0;
    if (is_rescaling) {
        up_scale = videoSettings.resolution_info.up_scale;
        down_shift = videoSettings.resolution_info.down_shift;
    }
    render_targets.size = Extent2D{
        (maxwell3d->regs.surface_clip.width * up_scale) >> down_shift,
        (maxwell3d->regs.surface_clip.height * up_scale) >> down_shift,
    };
    render_targets.is_rescaled = is_rescaling;

    flags[Dirty::DepthBiasGlobal] = true;
}

template <class P>
void TextureCache<P>::MarkRenderTarget(ImageViewId view_id) {
    if (!view_id) {
        return;
    }
    const ImageViewBase& view = slot_image_views[view_id];
    if (!view.IsBuffer()) {
        Image& image = slot_images[view.image_id];
        if (image.first_render_frame > frame_tick) {
            image.first_render_frame = frame_tick;
            has_new_render_targets = true;
        }
    }
}

template <class P>
bool TextureCache<P>::RenderTargetsDrawnBefore() const {
    const auto drawn_before = [this](ImageViewId view_id) {
        if (!view_id) {
            return true;
        }
        const ImageViewBase& view = slot_image_views[view_id];
        return view.IsBuffer() || slot_images[view.image_id].first_render_frame < frame_tick;
    };
    return std::ranges::all_of(render_targets.color_buffer_ids, drawn_before) &&
           drawn_before(render_targets.depth_buffer_id);
}

template <class P>
typename P::Framebuffer* TextureCache<P>::GetFramebuffer() {
    return &slot_framebuffers[GetFramebufferId(render_targets)];
}

template <class P>
typename P::Framebuffer* TextureCache<P>::GetClearFramebuffer(size_t color_attachment, u32 layer) {
    const bool is_color = color_attachment < NUM_RT;
    const ImageViewId source_id = is_color ? render_targets.color_buffer_ids[color_attachment]
                                           : render_targets.depth_buffer_id;
    const ImageView& source = slot_image_views[source_id];
    ASSERT(layer < static_cast<u32>(source.range.extent.layers));
    SubresourceRange range = source.range;
    range.base.layer += static_cast<s32>(layer);
    range.extent.layers = 1;
    const ImageId image_id = source.image_id;
    const ImageViewInfo view_info{ImageViewType::e2D, source.format, range};
    const ImageViewId view_id = FindOrEmplaceImageView(image_id, view_info);
    const RenderTargets key{
        .color_buffer_ids = {is_color ? view_id : ImageViewId{}},
        .depth_buffer_id = is_color ? ImageViewId{} : view_id,
        .size = render_targets.size,
        .is_rescaled = render_targets.is_rescaled,
    };
    return &slot_framebuffers[GetFramebufferId(key)];
}

template <class P>
template <bool has_blacklists>
void TextureCache<P>::FillImageViews(DescriptorTable<TICEntry>& table,
                                     std::span<ImageViewId> cached_image_view_ids,
                                     std::span<ImageViewInOut> views) {
    // Large bindless arrays repeat the same few handles. Within a pass, a later visit of an
    // index would read the same descriptor and find the same view, so it reuses the result.
    // Nothing is kept across passes: guest memory may change between them.
    const bool skip_repeats = views.size() >= 64;
    if (skip_repeats && visited_pass.size() <= table.Limit()) {
        visited_pass.resize(size_t{table.Limit()} + 1, 0);
        visited_view_ids.resize(visited_pass.size());
    }
    bool has_blacklisted = false;
    do {
        has_deleted_images = false;
        if constexpr (has_blacklists) {
            has_blacklisted = false;
        }
        if (skip_repeats && ++current_visit_pass == 0) {
            std::ranges::fill(visited_pass, 0);
            current_visit_pass = 1;
        }
        for (ImageViewInOut& view : views) {
            // A deletion or rescale erases views mid-pass, so the stored results may be gone.
            // The pass is repeated anyway; visit the rest of it without reusing them.
            if (skip_repeats && !has_deleted_images && view.index <= table.Limit()) {
                u32& pass = visited_pass[view.index];
                ImageViewId& visited_id = visited_view_ids[view.index];
                if (pass != current_visit_pass) {
                    pass = current_visit_pass;
                    visited_id = VisitImageView(table, cached_image_view_ids, view.index);
                }
                view.id = visited_id;
            } else {
                view.id = VisitImageView(table, cached_image_view_ids, view.index);
            }
            if constexpr (has_blacklists) {
                if (view.blacklist && view.id != NULL_IMAGE_VIEW_ID) {
                    const ImageViewBase& image_view{slot_image_views[view.id]};
                    auto& image = slot_images[image_view.image_id];
                    has_blacklisted |= ScaleDown(image);
                    image.scale_rating = 0;
                }
            }
        }
    } while (has_deleted_images || (has_blacklists && has_blacklisted));
}

template <class P>
ImageViewId TextureCache<P>::VisitImageView(DescriptorTable<TICEntry>& table,
                                            std::span<ImageViewId> cached_image_view_ids,
                                            u32 index) {
    if (index > table.Limit()) {
        LOG_DEBUG(HW_GPU, "Invalid image view index={}", index);
        return NULL_IMAGE_VIEW_ID;
    }
    const auto [descriptor, is_new] = table.Read(index);
    ImageViewId& image_view_id = cached_image_view_ids[index];
    if (is_new || image_view_id == CORRUPT_ID) {
        image_view_id = FindImageView(descriptor);
    }
    if (image_view_id != NULL_IMAGE_VIEW_ID) {
        PrepareImageView(image_view_id, false, false);
    }
    return image_view_id;
}

template <class P>
FramebufferId TextureCache<P>::GetFramebufferId(const RenderTargets& key) {
    const auto [pair, is_new] = framebuffers.try_emplace(key);
    FramebufferId& framebuffer_id = pair->second;
    if (!is_new) {
        return framebuffer_id;
    }
    std::array<ImageView*, NUM_RT> color_buffers;
    std::ranges::transform(key.color_buffer_ids, color_buffers.begin(),
                           [this](ImageViewId id) { return id ? &slot_image_views[id] : nullptr; });
    ImageView* const depth_buffer =
        key.depth_buffer_id ? &slot_image_views[key.depth_buffer_id] : nullptr;
    framebuffer_id = slot_framebuffers.insert(runtime, color_buffers, depth_buffer, key);
    return framebuffer_id;
}

template <class P>
void TextureCache<P>::WriteMemory(DAddr cpu_addr, size_t size) {
    ForEachImageInRegion(cpu_addr, size, [this](ImageId image_id, Image& image) {
        if (True(image.flags & ImageFlagBits::CpuModified)) {
            return;
        }
        image.flags |= ImageFlagBits::CpuModified;
        if (True(image.flags & ImageFlagBits::Tracked)) {
            UntrackImage(image, image_id);
        }
    });
}

template <class P>
void TextureCache<P>::DownloadMemory(DAddr cpu_addr, size_t size, bool native_read) {
    static const bool release_native = std::getenv("NXEMU_NCE_RELEASE_READBACK") != nullptr;
    boost::container::small_vector<ImageId, 16> images;
    ForEachImageInRegion(cpu_addr, size, [&images, native_read](ImageId image_id, ImageBase& image) {
        if (!image.IsSafeDownload()) {
            return;
        }
        // The release candidate uses ordinary download ownership: the backing is
        // current after publication, and MarkModification protects it on the next
        // GPU write. Keeping GpuModified here recopies unchanged images on later faults.
        if (!native_read || release_native) image.flags &= ~ImageFlagBits::GpuModified;
        images.push_back(image_id);
    });
    if (images.empty()) {
        return;
    }
    std::ranges::sort(images, [this](ImageId lhs, ImageId rhs) {
        return slot_images[lhs].modification_tick < slot_images[rhs].modification_tick;
    });
    // Keep independent staging allocations alive until their shared GPU fence completes.
    // A 16 KiB native fault can overlap many tiny images; waiting once per image
    // serializes CPU and GPU even though every copy belongs to the same read request.
    static const bool batch_native = std::getenv("NXEMU_NCE_BATCH_READBACK") != nullptr;
    if (native_read && batch_native) {
        constexpr size_t MAX_BATCH_BYTES = 64_MiB;
        constexpr size_t MAX_BATCH_IMAGES = 64;
        boost::container::small_vector<std::pair<ImageId, AsyncBuffer>, 4> downloads;
        size_t batch_bytes{};
        const auto finish = [&] {
            if (downloads.empty()) return;
            runtime.Finish();
            for (auto& [id, map] : downloads) {
                Image& image = slot_images[id];
                SwizzleImage(*gpu_memory, image.gpu_addr, image.info, FullDownloadCopies(image.info),
                             map.mapped_span, swizzle_data_buffer);
                MarkReadbackSynchronized(image);
                UpdateNativeReadTracking(image, id);
                runtime.FreeDeferredStagingBuffer(map);
            }
            downloads.clear();
            batch_bytes = 0;
        };
        for (const ImageId id : images) {
            Image& image = slot_images[id];
            if (!downloads.empty() &&
                (downloads.size() >= MAX_BATCH_IMAGES || batch_bytes >= MAX_BATCH_BYTES ||
                 image.unswizzled_size_bytes > MAX_BATCH_BYTES - batch_bytes)) {
                finish();
            }
            auto& download = downloads.emplace_back(
                id, runtime.DownloadStagingBuffer(image.unswizzled_size_bytes, true));
            image.DownloadMemory(download.second, FullDownloadCopies(image.info));
            batch_bytes += image.unswizzled_size_bytes;
        }
        finish();
        return;
    }
    for (const ImageId image_id : images) {
        Image& image = slot_images[image_id];
        auto map = runtime.DownloadStagingBuffer(image.unswizzled_size_bytes);
        const auto copies = FullDownloadCopies(image.info);
        image.DownloadMemory(map, copies);
        runtime.Finish();
        SwizzleImage(*gpu_memory, image.gpu_addr, image.info, copies, map.mapped_span,
                     swizzle_data_buffer);
        MarkReadbackSynchronized(image);
        UpdateNativeReadTracking(image, image_id);
    }
}

template <class P>
std::optional<RasterizerDownloadArea> TextureCache<P>::GetFlushArea(DAddr cpu_addr,
                                                                               u64 size) {
    static const bool observed = std::getenv("NXEMU_NCE_OBSERVED_READBACK") != nullptr;
    if (observed && size != 0) {
        const auto end = Common::AlignUp(cpu_addr + size, ReadbackPageTracker::PAGE_SIZE);
        cpu_addr = Common::AlignDown(cpu_addr, ReadbackPageTracker::PAGE_SIZE);
        size = end - cpu_addr;
    }
    std::optional<RasterizerDownloadArea> area{};
    ForEachImageInRegion(cpu_addr, size, [&](ImageId, ImageBase& image) {
        if (False(image.flags & ImageFlagBits::GpuModified)) {
            return;
        }
        if (!area) {
            area.emplace();
            area->startAddress = cpu_addr;
            area->endAddress = cpu_addr + size;
            area->preemtive = true;
        }
        area->startAddress = std::min(area->startAddress, image.cpu_addr);
        area->endAddress = std::max(area->endAddress, image.cpu_addr_end);
        for (auto image_view_id : image.image_view_ids) {
            auto& image_view = slot_image_views[image_view_id];
            image_view.flags |= ImageViewFlagBits::PreemtiveDownload;
        }
        area->preemtive &= image.info.forced_flushed;
        image.info.forced_flushed = true;
    });
    if (observed && area) {
        // Cache only the requested range in the OS, so the first read of another
        // page cannot inherit this image's preemptive-ready flag.
        area->preemtive &= readback_pages.Observe(cpu_addr, size);
        area->startAddress = cpu_addr;
        area->endAddress = cpu_addr + size;
    }
    return area;
}

template <class P>
void TextureCache<P>::UnmapMemory(DAddr cpu_addr, size_t size) {
    readback_pages.Forget(cpu_addr, size);
    boost::container::small_vector<ImageId, 16> deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        Image& image = slot_images[id];
        if (True(image.flags & ImageFlagBits::Tracked)) {
            UntrackImage(image, id);
        }
        UnregisterImage(id);
        DeleteImage(id);
    }
}

template <class P>
void TextureCache<P>::UnmapGPUMemory(size_t as_id, GPUVAddr gpu_addr, size_t size,
                                    bool preserve_contents) {
    boost::container::small_vector<ImageId, 16> deleted_images;
    ForEachImageInRegionGPU(as_id, gpu_addr, size,
                            [&](ImageId id, Image&) { deleted_images.push_back(id); });
    // Write older aliases first so the most recently modified image wins in guest memory.
    std::ranges::sort(deleted_images, [this](ImageId lhs, ImageId rhs) {
        return slot_images[lhs].modification_tick < slot_images[rhs].modification_tick;
    });
    constexpr size_t MAX_DOWNLOAD_BATCH_SIZE = 64_MiB;
    constexpr size_t MAX_DOWNLOAD_BATCH_IMAGES = 16;
    boost::container::small_vector<std::pair<ImageId, AsyncBuffer>, 4> downloads;
    size_t batch_size{};
    const auto finish_downloads = [&] {
        if (downloads.empty()) {
            return;
        }
        runtime.Finish();
        for (auto& [id, map] : downloads) {
            const Image& image = slot_images[id];
            const auto copies = FullDownloadCopies(image.info);
            SwizzleImage(*this->GetFromID(as_id), image.gpu_addr, image.info, copies, map.mapped_span,
                         swizzle_data_buffer);
            runtime.FreeDeferredStagingBuffer(map);
        }
        downloads.clear();
        batch_size = 0;
    };
    for (const ImageId id : deleted_images) {
        Image& image = slot_images[id];
        const bool fully_remapped =
            gpu_addr <= image.gpu_addr && image.gpu_addr - gpu_addr <= size &&
            image.guest_size_bytes <= size - (image.gpu_addr - gpu_addr);
        if ((preserve_contents || !fully_remapped) && image.IsSafeDownload()) {
            if (!downloads.empty() &&
                (downloads.size() >= MAX_DOWNLOAD_BATCH_IMAGES ||
                 batch_size >= MAX_DOWNLOAD_BATCH_SIZE ||
                 image.unswizzled_size_bytes > MAX_DOWNLOAD_BATCH_SIZE - batch_size)) {
                finish_downloads();
            }
            // Reserve buffers until their CPU readback is complete; share one GPU wait per batch.
            auto& download = downloads.emplace_back(
                id, runtime.DownloadStagingBuffer(image.unswizzled_size_bytes, true));
            const auto copies = FullDownloadCopies(image.info);
            image.DownloadMemory(download.second, copies);
            batch_size += image.unswizzled_size_bytes;
        }
    }
    finish_downloads();
    for (const ImageId id : deleted_images) {
        Image& image = slot_images[id];
        image.flags &= ~ImageFlagBits::GpuModified;
        UpdateNativeReadTracking(image, id);
        if (False(image.flags & ImageFlagBits::CpuModified)) {
            image.flags |= ImageFlagBits::CpuModified;
            if (True(image.flags & ImageFlagBits::Tracked)) {
                UntrackImage(image, id);
            }
        }

        if (True(image.flags & ImageFlagBits::Remapped)) {
            continue;
        }
        image.flags |= ImageFlagBits::Remapped;
    }
}

template <class P>
bool TextureCache<P>::BlitImage(const Tegra::Engines::Fermi2D::Surface& dst,
                                const Tegra::Engines::Fermi2D::Surface& src,
                                const Tegra::Engines::Fermi2D::Config& copy) {
    const auto result = GetBlitImages(dst, src, copy);
    if (!result) {
        return false;
    }
    const BlitImages images = *result;
    const ImageId dst_id = images.dst_id;
    const ImageId src_id = images.src_id;

    PrepareImage(src_id, false, false);
    PrepareImage(dst_id, true, false);

    Image& dst_image = slot_images[dst_id];
    Image& src_image = slot_images[src_id];
    bool is_src_rescaled = True(src_image.flags & ImageFlagBits::Rescaled);
    bool is_dst_rescaled = True(dst_image.flags & ImageFlagBits::Rescaled);

    const bool is_resolve = src_image.info.num_samples != 1 && dst_image.info.num_samples == 1;
    if (is_src_rescaled != is_dst_rescaled) {
        if (ImageCanRescale(src_image)) {
            ScaleUp(src_image);
            is_src_rescaled = True(src_image.flags & ImageFlagBits::Rescaled);
            if (is_resolve) {
                dst_image.info.rescaleable = true;
                for (const auto& alias : dst_image.aliased_images) {
                    Image& other_image = slot_images[alias.id];
                    other_image.info.rescaleable = true;
                }
            }
        }
        if (ImageCanRescale(dst_image)) {
            ScaleUp(dst_image);
            is_dst_rescaled = True(dst_image.flags & ImageFlagBits::Rescaled);
        }
    }
    if (is_resolve && (is_src_rescaled != is_dst_rescaled)) {
        // A resolve requires both images to be the same dimensions. Resize down if needed.
        ScaleDown(src_image);
        ScaleDown(dst_image);
        is_src_rescaled = True(src_image.flags & ImageFlagBits::Rescaled);
        is_dst_rescaled = True(dst_image.flags & ImageFlagBits::Rescaled);
    }
    const auto& resolution = videoSettings.resolution_info;
    const auto scale_region = [&](Region2D& region) {
        region.start.x = resolution.ScaleUp(region.start.x);
        region.start.y = resolution.ScaleUp(region.start.y);
        region.end.x = resolution.ScaleUp(region.end.x);
        region.end.y = resolution.ScaleUp(region.end.y);
    };

    // TODO: Deduplicate
    const std::optional src_base = src_image.TryFindBase(src.Address());
    const SubresourceRange src_range{.base = src_base.value(), .extent = {1, 1}};
    const ImageViewInfo src_view_info(ImageViewType::e2D, images.src_format, src_range);
    const auto [src_framebuffer_id, src_view_id] = RenderTargetFromImage(src_id, src_view_info);
    const auto [src_samples_x, src_samples_y] = SamplesLog2(src_image.info.num_samples);
    Region2D src_region{
        Offset2D{.x = copy.src_x0 >> src_samples_x, .y = copy.src_y0 >> src_samples_y},
        Offset2D{.x = copy.src_x1 >> src_samples_x, .y = copy.src_y1 >> src_samples_y},
    };
    if (is_src_rescaled) {
        scale_region(src_region);
    }

    const std::optional dst_base = dst_image.TryFindBase(dst.Address());
    const SubresourceRange dst_range{.base = dst_base.value(), .extent = {1, 1}};
    const ImageViewInfo dst_view_info(ImageViewType::e2D, images.dst_format, dst_range);
    const auto [dst_framebuffer_id, dst_view_id] = RenderTargetFromImage(dst_id, dst_view_info);
    const auto [dst_samples_x, dst_samples_y] = SamplesLog2(dst_image.info.num_samples);
    Region2D dst_region{
        Offset2D{.x = copy.dst_x0 >> dst_samples_x, .y = copy.dst_y0 >> dst_samples_y},
        Offset2D{.x = copy.dst_x1 >> dst_samples_x, .y = copy.dst_y1 >> dst_samples_y},
    };
    if (is_dst_rescaled) {
        scale_region(dst_region);
    }

    // Always call this after src_framebuffer_id was queried, as the address might be invalidated.
    Framebuffer* const dst_framebuffer = &slot_framebuffers[dst_framebuffer_id];
    if constexpr (FRAMEBUFFER_BLITS) {
        // OpenGL blits from framebuffers, not images
        Framebuffer* const src_framebuffer = &slot_framebuffers[src_framebuffer_id];
        runtime.BlitFramebuffer(dst_framebuffer, src_framebuffer, dst_region, src_region,
                                copy.filter, copy.operation);
    } else {
        // Vulkan can blit images, but it lacks format reinterpretations
        // Provide a framebuffer in case it's necessary
        ImageView& dst_view = slot_image_views[dst_view_id];
        ImageView& src_view = slot_image_views[src_view_id];
        runtime.BlitImage(dst_framebuffer, dst_view, src_view, dst_region, src_region, copy.filter,
                          copy.operation);
    }
    // Fermi2D bypasses BindRenderTarget and must enqueue preemptive readbacks.
    const ImageViewBase& dst_view = slot_image_views[dst_view_id];
    if (True(dst_view.flags & ImageViewFlagBits::PreemtiveDownload)) {
        uncommitted_downloads.emplace_back(PendingDownload{true, 0, dst_id});
    }
    return true;
}

template <class P>
std::pair<typename P::ImageView*, bool> TextureCache<P>::TryFindFramebufferImageView(
    const Tegra::FramebufferConfig& config, DAddr cpu_addr) {
    // TODO: Properly implement this
    const auto it = page_table.find(cpu_addr >> YUZU_PAGEBITS);
    if (it == page_table.end()) {
        return {};
    }
    const auto& image_map_ids = it->second;
    boost::container::small_vector<ImageId, 4> valid_image_ids;
    for (const ImageMapId map_id : image_map_ids) {
        const ImageMapView& map = slot_map_views[map_id];
        const ImageBase& image = slot_images[map.image_id];
        if (image.cpu_addr != cpu_addr) {
            continue;
        }
        if (image.image_view_ids.empty()) {
            continue;
        }
        valid_image_ids.push_back(map.image_id);
    }

    const auto view_format = [&]() {
        switch (config.pixel_format) {
        case Service::android::PixelFormat::Rgb565:
            return PixelFormat::R5G6B5_UNORM;
        case Service::android::PixelFormat::Bgra8888:
            return PixelFormat::B8G8R8A8_UNORM;
        default:
            return PixelFormat::A8B8G8R8_UNORM;
        }
    }();

    const auto GetImageViewForFramebuffer = [&](ImageId image_id) {
        ImageViewInfo info{ImageViewType::e2D, view_format};
        if (config.blending == Tegra::BlendMode::Opaque) {
            info.x_source = static_cast<u8>(SwizzleSource::R);
            info.y_source = static_cast<u8>(SwizzleSource::G);
            info.z_source = static_cast<u8>(SwizzleSource::B);
            info.w_source = static_cast<u8>(SwizzleSource::OneFloat);
        }
        return std::make_pair(&slot_image_views[FindOrEmplaceImageView(image_id, info)],
                              slot_images[image_id].IsRescaled());
    };

    if (valid_image_ids.size() == 1) [[likely]] {
        return GetImageViewForFramebuffer(valid_image_ids.front());
    }

    if (valid_image_ids.size() > 0) [[unlikely]] {
        auto most_recent = std::ranges::max_element(valid_image_ids, [&](auto a, auto b) {
            return slot_images[a].modification_tick < slot_images[b].modification_tick;
        });
        return GetImageViewForFramebuffer(*most_recent);
    }

    return {};
}

template <class P>
bool TextureCache<P>::HasUncommittedFlushes() const noexcept {
    return !uncommitted_downloads.empty();
}

template <class P>
bool TextureCache<P>::ShouldWaitAsyncFlushes() const noexcept {
    return !committed_downloads.empty() && !committed_downloads.front().empty();
}

template <class P>
void TextureCache<P>::DeduplicateDownloads() {
    if constexpr (!IMPLEMENTS_ASYNC_DOWNLOADS) {
        return;
    }
    if (uncommitted_downloads.size() < 2) {
        return;
    }
    // Full images share final GPU contents; retain the first for reverse publication.
    // Keep buffer snapshots distinct.
    auto output = uncommitted_downloads.begin();
    for (auto input = uncommitted_downloads.begin(); input != uncommitted_downloads.end();
         ++input) {
        if (input->is_swizzle) {
            Image& image = slot_images[input->object_id];
            if (True(image.flags & ImageFlagBits::DownloadQueued)) {
                continue;
            }
            image.flags |= ImageFlagBits::DownloadQueued;
        }
        *output++ = *input;
    }
    uncommitted_downloads.erase(output, uncommitted_downloads.end());
    for (const PendingDownload& download : uncommitted_downloads) {
        if (download.is_swizzle) {
            slot_images[download.object_id].flags &= ~ImageFlagBits::DownloadQueued;
        }
    }
}

template <class P>
bool TextureCache<P>::TryDownloadObservedImage(Image& image, AsyncBuffer& map,
                                              PendingDownload& download) {
    static const bool enabled = std::getenv("NXEMU_NCE_OBSERVED_GPU_COPIES") != nullptr &&
                                std::getenv("NXEMU_NCE_OBSERVED_READBACK") != nullptr;
    const auto& info = image.info;
    if (!enabled || info.type != ImageType::e2D || info.resources.levels != 1 ||
        info.resources.layers != 1 || info.size.depth != 1 || info.block.depth != 0 ||
        info.tile_width_spacing != 0 || info.num_samples != 1 ||
        info.size.width <= 0 || info.size.height <= 0 ||
        u64{info.size.width} * 4 > std::numeric_limits<u32>::max() ||
        BytesPerBlock(info.format) != 4 || GetFormatType(info.format) != SurfaceType::ColorTexture ||
        True(image.flags & (ImageFlagBits::Sparse | ImageFlagBits::Remapped))) return false;
    const auto address_space = ImageAddressSpace(image);
    if (!address_space) return false;
    const auto owner = address_spaces.find(*address_space);
    if (owner == address_spaces.end()) return false;
    const auto address = owner->second.gpu_memory->GpuToCpuAddress(image.gpu_addr, image.guest_size_bytes);
    if (!address || *address != image.cpu_addr) return false;
    const u64 linear_bytes = u64{info.size.width} * info.size.height * 4;
    if (image.unswizzled_size_bytes < linear_bytes) return false;
    std::vector<std::pair<u64, u64>> ranges;
    readback_pages.ForEachRange(image.cpu_addr, image.guest_size_bytes, download.observation_serial,
                               [&](u64 offset, u64 size) { ranges.emplace_back(offset, size); });
    const auto plan = PlanObservedReadback(info.size.width * 4, info.size.height,
                                           MipBlockSize(info, 0).height, ranges);
    if (!plan) return false;
    boost::container::small_vector<BufferImageCopy, 16> copies;
    u64 transferred{};
    for (const auto& rect : *plan) {
        copies.push_back({
            .buffer_offset = (u64{rect.y} * info.size.width + rect.x) * 4,
            .buffer_size = (u64{rect.height - 1} * info.size.width + rect.width) * 4,
            .buffer_row_length = static_cast<u32>(info.size.width),
            .buffer_image_height = static_cast<u32>(info.size.height),
            .image_subresource = {.base_level = 0, .base_layer = 0, .num_layers = 1},
            .image_offset = {static_cast<s32>(rect.x), static_cast<s32>(rect.y), 0},
            .image_extent = {rect.width, rect.height, 1},
        });
        transferred += u64{rect.width} * rect.height * 4;
    }
    if (!copies.empty()) image.DownloadMemory(map, copies);
    // Until retirement, observations can only be removed by the serial filter.
    // A newly observed page must take the synchronous full-copy route first.
    download.observed_partial = true;
    download.observed_address_space = *address_space;
    static const bool profile = std::getenv("NXEMU_NCE_READBACK_PROFILE") != nullptr;
    if (profile) {
        using Clock = std::chrono::steady_clock;
        static thread_local auto reported = Clock::now();
        static thread_local u64 images{}, full{}, partial{}, rectangles{};
        ++images;full += linear_bytes;partial += transferred;rectangles += copies.size();
        const auto now = Clock::now();
        if (now - reported >= std::chrono::seconds(5)) {
            std::fprintf(stderr, "[NCE-GPU-COPY-PLAN] interval=%.6f images=%llu rectangles=%llu full_MiB=%.3f copied_MiB=%.3f end_s=%.6f\n",
                std::chrono::duration<double>(now - reported).count(),
                (unsigned long long)images, (unsigned long long)rectangles,
                static_cast<double>(full) / 1048576.0, static_cast<double>(partial) / 1048576.0,
                std::chrono::duration<double>(now.time_since_epoch()).count());
            images = full = partial = rectangles = 0;reported = now;
        }
    }
    return true;
}

template <class P>
void TextureCache<P>::CommitAsyncFlushes() {
    DeduplicateDownloads();
    for (auto& download : uncommitted_downloads) {
        download.observation_serial = readback_pages.Serial();
    }
    // This is intentionally passing the value by copy
    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        auto& download_ids = uncommitted_downloads;
        if (download_ids.empty()) {
            committed_downloads.emplace_back(std::move(uncommitted_downloads));
            uncommitted_downloads.clear();
            async_buffers.emplace_back(std::move(uncommitted_async_buffers));
            uncommitted_async_buffers.clear();
            return;
        }
        size_t total_size_bytes = 0;
        size_t last_async_buffer_id = uncommitted_async_buffers.size();
        bool any_none_dma = false;
        for (PendingDownload& download_info : download_ids) {
            if (download_info.is_swizzle) {
                download_info.staging_bytes =
                    Common::AlignUp(slot_images[download_info.object_id].unswizzled_size_bytes, 64);
                total_size_bytes += download_info.staging_bytes;
                any_none_dma = true;
                download_info.async_buffer_id = last_async_buffer_id;
            }
        }

        if (any_none_dma) {
            auto download_map = runtime.DownloadStagingBuffer(total_size_bytes, true);
            for (PendingDownload& download_info : download_ids) {
                if (download_info.is_swizzle) {
                    Image& image = slot_images[download_info.object_id];
                    if (!TryDownloadObservedImage(image, download_map, download_info)) {
                        const auto copies = FullDownloadCopies(image.info);
                        image.DownloadMemory(download_map, copies);
                    }
                    download_map.offset += download_info.staging_bytes;
                }
            }
            uncommitted_async_buffers.emplace_back(download_map);
        }

        async_buffers.emplace_back(std::move(uncommitted_async_buffers));
        uncommitted_async_buffers.clear();
    }
    committed_downloads.emplace_back(std::move(uncommitted_downloads));
    uncommitted_downloads.clear();
}

template <class P>
void TextureCache<P>::MarkReadbackSynchronized(Image& image) {
    static const bool enabled = std::getenv("NXEMU_NCE_OBSERVED_READBACK") != nullptr;
    if (!enabled) return;
    if (False(image.flags & ImageFlagBits::Sparse)) {
        if (image.cpu_addr < ~(1ULL << 40))
            readback_pages.MarkSynchronized(image.cpu_addr, image.guest_size_bytes);
    } else {
        ForEachSparseSegment(image, [this](GPUVAddr, DAddr address, size_t size) {
            readback_pages.MarkSynchronized(address, size);
        });
    }
}

template <class P>
bool TextureCache<P>::PublishObservedReadback(const ImageBase& image,
                                             std::span<const BufferImageCopy> copies,
                                             std::span<const u8> memory, u64 observation_serial,
                                             Tegra::MemoryManager* publication_memory) {
    static const bool enabled = std::getenv("NXEMU_NCE_OBSERVED_READBACK") != nullptr;
    const auto& info = image.info;
    if (!enabled || info.type != ImageType::e2D || info.resources.levels != 1 ||
        info.resources.layers != 1 || info.size.depth != 1 || info.block.depth != 0 ||
        info.tile_width_spacing != 0 || info.num_samples != 1 ||
        BytesPerBlock(info.format) != 4 || copies.size() != 1 ||
        True(image.flags & ImageFlagBits::Sparse)) return false;
    auto* const memory_owner = publication_memory ? publication_memory : gpu_memory;
    const auto address = memory_owner->GpuToCpuAddress(image.gpu_addr, image.guest_size_bytes);
    if (!address || *address != image.cpu_addr) return false;
    const auto& copy = copies.front();
    if (copy.image_offset.x != 0 || copy.image_offset.y != 0 || copy.image_offset.z != 0 ||
        copy.image_extent != info.size) return false;
    const u64 linear_bytes = u64{info.size.width} * info.size.height * 4;
    if (copy.buffer_offset > memory.size() || linear_bytes > memory.size() - copy.buffer_offset)
        return false;
    const auto input = memory.subspan(copy.buffer_offset, linear_bytes);
    static const bool profile = std::getenv("NXEMU_NCE_READBACK_PROFILE") != nullptr;
    using Clock = std::chrono::steady_clock;
    const auto started = profile ? Clock::now() : Clock::time_point{};
    u64 published{};
    readback_pages.ForEachRange(image.cpu_addr, image.guest_size_bytes, observation_serial,
        [&](u64 offset, u64 size) {
            published += size;
            Tegra::Memory::GpuGuestMemoryScoped<u8, GuestMemoryFlags::UnsafeReadWrite>
                destination(*memory_owner, image.gpu_addr + offset, size, &swizzle_data_buffer);
            Tegra::Texture::Swizzle2DRange(destination, input, info.size.width * 4,
                                         info.size.height, MipBlockSize(info, 0).height, offset);
        });
    if (profile) {
        static thread_local u64 images{}, bytes{}, full_bytes{};
        static thread_local double conversion_ms{};
        static thread_local auto reported = Clock::now();
        const auto now = Clock::now();
        ++images;
        bytes += published;
        full_bytes += linear_bytes;
        conversion_ms += std::chrono::duration<double, std::milli>(now - started).count();
        if (now - reported >= std::chrono::seconds(5)) {
            std::fprintf(stderr, "[NCE-OBSERVED] interval=%.3f images=%llu published_KiB=%.3f full_MiB=%.3f conversion_ms=%.3f\n",
                std::chrono::duration<double>(now - reported).count(),
                (unsigned long long)images, static_cast<double>(bytes) / 1024.0,
                static_cast<double>(full_bytes) / 1048576.0, conversion_ms);
            images = bytes = full_bytes = 0;
            conversion_ms = 0;
            reported = now;
        }
    }
    return true;
}

template <class P>
void TextureCache<P>::PopAsyncFlushes() {
    if (committed_downloads.empty()) {
        return;
    }
    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        const auto& download_ids = committed_downloads.front();
        if (download_ids.empty()) {
            committed_downloads.pop_front();
            async_buffers.pop_front();
            return;
        }
        auto download_map = std::move(async_buffers.front());
        for (size_t i = download_ids.size(); i > 0; i--) {
            auto& download_info = download_ids[i - 1];
            auto& download_buffer = download_map[download_info.async_buffer_id];
            if (download_info.is_swizzle) {
                download_buffer.offset -= download_info.staging_bytes;
                if (download_info.discarded) {
                    continue;
                }
                const ImageBase& image = slot_images[download_info.object_id];
                const auto copies = FullDownloadCopies(image.info);
                std::span<u8> download_span =
                    download_buffer.mapped_span.subspan(download_buffer.offset);
                if (False(image.flags & ImageFlagBits::Remapped)) {
                    if (download_info.observed_partial) {
                        // Never fall back to a full swizzle of a partially initialized
                        // staging buffer. Use this image's original address space even
                        // if another GPU channel is currently bound.
                        const auto owner = address_spaces.find(download_info.observed_address_space);
                        if (owner != address_spaces.end()) {
                            const auto address = owner->second.gpu_memory->GpuToCpuAddress(
                                image.gpu_addr, image.guest_size_bytes);
                            if (address && *address == image.cpu_addr) {
                                const bool published = PublishObservedReadback(
                                    image, copies, download_span, download_info.observation_serial,
                                    owner->second.gpu_memory);
                                ASSERT(published);
                            }
                        }
                    } else if (!PublishObservedReadback(image, copies, download_span,
                                                        download_info.observation_serial)) {
                        SwizzleImage(*gpu_memory, image.gpu_addr, image.info, copies, download_span,
                                     swizzle_data_buffer);
                    }
                }
            } else {
                const BufferDownload& buffer_info = slot_buffer_downloads[download_info.object_id];
                std::span<u8> download_span =
                    download_buffer.mapped_span.subspan(download_buffer.offset);
                gpu_memory->WriteBlockUnsafe(buffer_info.address, download_span.data(),
                                             buffer_info.size);
                slot_buffer_downloads.erase(download_info.object_id);
            }
        }
        for (auto& download_buffer : download_map) {
            async_buffers_death_ring.emplace_back(download_buffer);
        }
        committed_downloads.pop_front();
        async_buffers.pop_front();
    } else {
        const auto& download_ids = committed_downloads.front();
        if (download_ids.empty()) {
            committed_downloads.pop_front();
            return;
        }
        size_t total_size_bytes = 0;
        for (const PendingDownload& download_info : download_ids) {
            if (download_info.is_swizzle && !download_info.discarded) {
                total_size_bytes += slot_images[download_info.object_id].unswizzled_size_bytes;
            }
        }
        auto download_map = runtime.DownloadStagingBuffer(total_size_bytes);
        const size_t original_offset = download_map.offset;
        for (const PendingDownload& download_info : download_ids) {
            if (!download_info.is_swizzle || download_info.discarded) {
                continue;
            }
            Image& image = slot_images[download_info.object_id];
            const auto copies = FullDownloadCopies(image.info);
            image.DownloadMemory(download_map, copies);
            download_map.offset += image.unswizzled_size_bytes;
        }
        // Wait for downloads to finish
        runtime.Finish();
        download_map.offset = original_offset;
        std::span<u8> download_span = download_map.mapped_span;
        for (const PendingDownload& download_info : download_ids) {
            if (!download_info.is_swizzle || download_info.discarded) {
                continue;
            }
            const ImageBase& image = slot_images[download_info.object_id];
            const auto copies = FullDownloadCopies(image.info);
            if (False(image.flags & ImageFlagBits::Remapped)) {
                SwizzleImage(*gpu_memory, image.gpu_addr, image.info, copies, download_span,
                             swizzle_data_buffer);
            }
            download_map.offset += image.unswizzled_size_bytes;
            download_span = download_span.subspan(image.unswizzled_size_bytes);
        }
        committed_downloads.pop_front();
    }
}

template <class P>
ImageId TextureCache<P>::DmaImageId(const Tegra::DMA::ImageOperand& operand, bool is_upload) {
    const ImageInfo dst_info(operand);
    const ImageId dst_id = FindDMAImage(dst_info, operand.address);
    if (!dst_id) {
        return NULL_IMAGE_ID;
    }
    auto& image = slot_images[dst_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        // No need to waste time on an image that's synced with guest
        return NULL_IMAGE_ID;
    }
    if (image.info.type == ImageType::e3D) {
        // Don't accelerate 3D images.
        return NULL_IMAGE_ID;
    }
    if (!is_upload && !image.info.dma_downloaded) {
        // Force a full sync.
        image.info.dma_downloaded = true;
        return NULL_IMAGE_ID;
    }
    const auto base = image.TryFindBase(operand.address);
    if (!base) {
        return NULL_IMAGE_ID;
    }
    return dst_id;
}

template <class P>
bool TextureCache<P>::DmaImageToImage(const Tegra::DMA::ImageCopy& copy_info,
                                      const Tegra::DMA::ImageOperand& src,
                                      const Tegra::DMA::ImageOperand& dst) {
    // The caller has checked the complete, non-remapped 2D layout and excluded
    // buffer-cache registrations while holding both cache locks.
    const auto& s = src.params;
    const auto& d = dst.params;
    const ImageId src_id = FindDMAImage(ImageInfo(src), src.address);
    if (!src_id) {
        return false;
    }
    const ImageId dst_id = FindDMAImage(ImageInfo(dst), dst.address);
    if (!dst_id || src_id == dst_id) {
        return false;
    }
    const Image& src_image = slot_images[src_id];
    const Image& dst_image = slot_images[dst_id];
    const auto unsupported = ImageFlagBits::BadOverlap | ImageFlagBits::Converted |
                             ImageFlagBits::AsynchronousDecode | ImageFlagBits::Rescaled |
                             ImageFlagBits::Remapped | ImageFlagBits::Sparse |
                             ImageFlagBits::IsDecoding;
    const u32 bpp = VideoCore::Surface::BytesPerBlock(src_image.info.format);
    if (False(src_image.flags & ImageFlagBits::GpuModified) ||
        True(src_image.flags & unsupported) || True(dst_image.flags & unsupported) ||
        src_image.info.format != dst_image.info.format ||
        GetFormatType(src_image.info.format) != SurfaceType::ColorTexture ||
        !src_image.aliased_images.empty() || !dst_image.aliased_images.empty() ||
        !src_image.overlapping_images.empty() || !dst_image.overlapping_images.empty() ||
        src_image.info.size.depth != 1 || dst_image.info.size.depth != 1 ||
        src_image.info.type != ImageType::e2D || dst_image.info.type != ImageType::e2D ||
        src_image.info.is_sparse || dst_image.info.is_sparse ||
        src_image.info.tile_width_spacing != 0 || dst_image.info.tile_width_spacing != 0 ||
        src_image.info.num_samples != 1 || dst_image.info.num_samples != 1 ||
        src_image.info.resources.levels != 1 || dst_image.info.resources.levels != 1 ||
        src_image.info.resources.layers != 1 || dst_image.info.resources.layers != 1 ||
        src_image.gpu_addr != src.address || dst_image.gpu_addr != dst.address ||
        src_image.gpu_page_table_index != channel_state->gpu_page_table_index ||
        dst_image.gpu_page_table_index != channel_state->gpu_page_table_index ||
        src_image.info.block.width != s.block_size.width ||
        src_image.info.block.height != s.block_size.height ||
        src_image.info.block.depth != s.block_size.depth ||
        dst_image.info.block.width != d.block_size.width ||
        dst_image.info.block.height != d.block_size.height ||
        dst_image.info.block.depth != d.block_size.depth ||
        src_image.info.size.width != dst_image.info.size.width ||
        src_image.info.size.height != dst_image.info.size.height ||
        u64{src_image.info.size.width} * bpp != copy_info.length_x ||
        src_image.info.size.height != copy_info.length_y ||
        VideoCore::Surface::DefaultBlockWidth(src_image.info.format) != 1 ||
        VideoCore::Surface::DefaultBlockHeight(src_image.info.format) != 1) {
        return false;
    }

    // Host copy commands must not overlap physically aliased source and target.
    const u64 size = src_image.guest_size_bytes;
    const auto cpu_src = gpu_memory->GpuToCpuAddress(src.address);
    const auto cpu_dst = gpu_memory->GpuToCpuAddress(dst.address);
    if (!size || size != dst_image.guest_size_bytes || !cpu_src || !cpu_dst ||
        gpu_memory->MaxContinuousRange(src.address, size) != size ||
        gpu_memory->MaxContinuousRange(dst.address, size) != size) {
        return false;
    }
    const auto* src_ptr = device_memory.GetSpan(*cpu_src, size);
    const auto* dst_ptr = device_memory.GetSpan(*cpu_dst, size);
    if (!src_ptr || !dst_ptr) {
        return false;
    }
    const auto src_host = reinterpret_cast<uintptr_t>(src_ptr);
    const auto dst_host = reinterpret_cast<uintptr_t>(dst_ptr);
    const auto distance = src_host < dst_host ? dst_host - src_host : src_host - dst_host;
    if (distance < size) {
        return false;
    }

    PrepareImage(src_id, false, false);
    // Every destination texel is overwritten. Do not upload CPU-dirty contents
    // which would immediately be discarded. Aliases were excluded above so
    // PrepareImage cannot rescale either image after the eligibility checks.
    PrepareImage(dst_id, true, true);
    ImageCopy copy{
        .src_subresource = {.base_level = 0, .base_layer = 0, .num_layers = 1},
        .dst_subresource = {.base_level = 0, .base_layer = 0, .num_layers = 1},
        .src_offset = {},
        .dst_offset = {},
        .extent = {.width = src_image.info.size.width,
                   .height = src_image.info.size.height,
                   .depth = 1},
    };
    CopyImage(dst_id, src_id, {copy});

    // Match framebuffer blits: native readbacks must observe the new contents.
    for (const ImageViewId view_id : slot_images[dst_id].image_view_ids) {
        if (True(slot_image_views[view_id].flags & ImageViewFlagBits::PreemtiveDownload)) {
            uncommitted_downloads.emplace_back(PendingDownload{true, 0, dst_id});
            break;
        }
    }
    return true;
}

template <class P>
bool TextureCache<P>::IsRescaling() const noexcept {
    return is_rescaling;
}

template <class P>
bool TextureCache<P>::IsRescaling(const ImageViewBase& image_view) const noexcept {
    if (image_view.type == ImageViewType::Buffer) {
        return false;
    }
    const ImageBase& image = slot_images[image_view.image_id];
    return True(image.flags & ImageFlagBits::Rescaled);
}

template <class P>
bool TextureCache<P>::IsRegionGpuModified(DAddr addr, size_t size) {
    bool is_modified = false;
    ForEachImageInRegion(addr, size, [&is_modified](ImageId, ImageBase& image) {
        if (False(image.flags & ImageFlagBits::GpuModified)) {
            return false;
        }
        is_modified = true;
        return true;
    });
    return is_modified;
}

template <class P>
std::pair<typename TextureCache<P>::Image*, BufferImageCopy> TextureCache<P>::DmaBufferImageCopy(
    const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::BufferOperand& buffer_operand,
    const Tegra::DMA::ImageOperand& image_operand, ImageId image_id, bool modifies_image) {
    const auto [level, base] = PrepareDmaImage(image_id, image_operand.address, modifies_image);
    auto* image = &slot_images[image_id];
    const u32 buffer_size = static_cast<u32>(buffer_operand.pitch * buffer_operand.height);
    const u32 bpp = VideoCore::Surface::BytesPerBlock(image->info.format);
    const auto convert = [old_bpp = image_operand.bytes_per_pixel, bpp](u32 value) {
        return (old_bpp * value) / bpp;
    };
    const u32 base_x = convert(image_operand.params.origin.x.Value());
    const u32 base_y = image_operand.params.origin.y.Value();
    const u32 length_x = convert(copy_info.length_x);
    const u32 length_y = copy_info.length_y;

    const BufferImageCopy copy{
        .buffer_offset = 0,
        .buffer_size = buffer_size,
        .buffer_row_length = convert(buffer_operand.pitch),
        .buffer_image_height = buffer_operand.height,
        .image_subresource =
            {
                .base_level = static_cast<s32>(level),
                .base_layer = static_cast<s32>(base),
                .num_layers = 1,
            },
        .image_offset =
            {
                .x = static_cast<s32>(base_x),
                .y = static_cast<s32>(base_y),
                .z = 0,
            },
        .image_extent =
            {
                .width = length_x,
                .height = length_y,
                .depth = 1,
            },
    };
    return {image, copy};
}

template <class P>
void TextureCache<P>::DownloadImageIntoBuffer(typename TextureCache<P>::Image* image,
                                              typename TextureCache<P>::BufferType buffer,
                                              size_t buffer_offset,
                                              std::span<const VideoCommon::BufferImageCopy> copies,
                                              GPUVAddr address, size_t size) {
    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        const BufferDownload new_buffer_download{address, size};
        auto slot = slot_buffer_downloads.insert(new_buffer_download);
        const PendingDownload new_download{false, uncommitted_async_buffers.size(), slot};
        uncommitted_downloads.emplace_back(new_download);
        auto download_map = runtime.DownloadStagingBuffer(size, true);
        uncommitted_async_buffers.emplace_back(download_map);
        std::array buffers{
            buffer,
            download_map.buffer,
        };
        std::array<size_t, 2> buffer_offsets{
            buffer_offset,
            download_map.offset,
        };
        image->DownloadMemory(buffers, buffer_offsets, copies);
    } else {
        image->DownloadMemory(buffer, buffer_offset, copies);
    }
}

template <class P>
void TextureCache<P>::RefreshContents(Image& image, ImageId image_id) {
    if (False(image.flags & ImageFlagBits::CpuModified)) {
        // Only upload modified images
        return;
    }
    image.flags &= ~ImageFlagBits::CpuModified;
    TrackImage(image, image_id);

    if (image.info.num_samples > 1 && !runtime.CanUploadMSAA(image)) {
        LOG_WARNING(HW_GPU, "MSAA image upload is not supported for format {}",
                    static_cast<u32>(image.info.format));
        runtime.TransitionImageLayout(image);
        return;
    }
    if (True(image.flags & ImageFlagBits::AsynchronousDecode)) {
        QueueAsyncDecode(image, image_id);
        return;
    }
    auto staging = runtime.UploadStagingBuffer(MapSizeBytes(image));
    UploadImageContents(image, staging);
    runtime.InsertUploadMemoryBarrier();
}

template <class P>
template <typename StagingBuffer>
void TextureCache<P>::UploadImageContents(Image& image, StagingBuffer& staging) {
    const std::span<u8> mapped_span = staging.mapped_span;
    const GPUVAddr gpu_addr = image.gpu_addr;

    if (True(image.flags & ImageFlagBits::AcceleratedUpload)) {
        gpu_memory->ReadBlock(gpu_addr, mapped_span.data(), mapped_span.size_bytes(),
                              VideoCommon::CacheType::NoTextureCache);
        const auto uploads = FullUploadSwizzles(image.info);
        runtime.AccelerateImageUpload(image, staging, uploads);
        return;
    }

    Tegra::Memory::GpuGuestMemory<u8, GuestMemoryFlags::UnsafeRead> swizzle_data(*gpu_memory, gpu_addr, image.guest_size_bytes, &swizzle_data_buffer);

    if (True(image.flags & ImageFlagBits::Converted)) {
        unswizzle_data_buffer.resize_destructive(image.unswizzled_size_bytes);
        auto copies =
            UnswizzleImage(*gpu_memory, gpu_addr, image.info, swizzle_data, unswizzle_data_buffer);
        ConvertImage(unswizzle_data_buffer, image.info, mapped_span, copies);
        image.UploadMemory(staging, copies);
    } else {
        const auto copies =
            UnswizzleImage(*gpu_memory, gpu_addr, image.info, swizzle_data, mapped_span);
        image.UploadMemory(staging, copies);
    }
}

template <class P>
ImageViewId TextureCache<P>::FindImageView(const TICEntry& config) {
    if (!IsValidEntry(*gpu_memory, config)) {
        return NULL_IMAGE_VIEW_ID;
    }
    const auto [pair, is_new] = channel_state->image_views.try_emplace(config);
    ImageViewId& image_view_id = pair->second;
    if (is_new) {
        image_view_id = CreateImageView(config);
    }
    return image_view_id;
}

template <class P>
ImageViewId TextureCache<P>::CreateImageView(const TICEntry& config) {
    const ImageInfo info(config);
    if (info.type == ImageType::Buffer) {
        const ImageViewInfo view_info(config, 0);
        return slot_image_views.insert(runtime, info, view_info, config.Address());
    }
    const u32 layer_offset = config.BaseLayer() * info.layer_stride;
    const GPUVAddr image_gpu_addr = config.Address() - layer_offset;
    const ImageId image_id = FindOrInsertImage(info, image_gpu_addr);
    if (!image_id) {
        return NULL_IMAGE_VIEW_ID;
    }
    ImageBase& image = slot_images[image_id];
    const SubresourceBase base = image.TryFindBase(config.Address()).value();
    ASSERT(base.level == 0);
    const ImageViewInfo view_info(config, base.layer);
    const ImageViewId image_view_id = FindOrEmplaceImageView(image_id, view_info);
    ImageViewBase& image_view = slot_image_views[image_view_id];
    image_view.flags |= ImageViewFlagBits::Strong;
    image.flags |= ImageFlagBits::Strong;
    return image_view_id;
}

template <class P>
ImageId TextureCache<P>::FindOrInsertImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                           RelaxedOptions options, bool discard_contents) {
    if (const ImageId image_id = FindImage(info, gpu_addr, options); image_id) {
        return image_id;
    }
    return InsertImage(info, gpu_addr, options, discard_contents);
}

template <class P>
ImageId TextureCache<P>::FindImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                   RelaxedOptions options) {
    std::optional<DAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
    if (!cpu_addr) {
        cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr, CalculateGuestSizeInBytes(info));
        if (!cpu_addr) {
            return ImageId{};
        }
    }
    const bool broken_views =
        runtime.HasBrokenTextureViewFormats() || True(options & RelaxedOptions::ForceBrokenViews);
    const bool native_bgr = runtime.HasNativeBgr();
    const bool flexible_formats = True(options & RelaxedOptions::Format);
    ImageId image_id{};
    boost::container::small_vector<ImageId, 8> image_ids;
    const auto lambda = [&](ImageId existing_image_id, ImageBase& existing_image) {
        if (True(existing_image.flags & ImageFlagBits::Remapped)) {
            return false;
        }
        if (info.type == ImageType::Linear || existing_image.info.type == ImageType::Linear)
            [[unlikely]] {
            const bool strict_size = False(options & RelaxedOptions::Size) &&
                                     True(existing_image.flags & ImageFlagBits::Strong);
            const ImageInfo& existing = existing_image.info;
            if (existing_image.gpu_addr == gpu_addr && existing.type == info.type &&
                existing.pitch == info.pitch &&
                IsPitchLinearSameSize(existing, info, strict_size) &&
                IsViewCompatible(existing.format, info.format, broken_views, native_bgr)) {
                image_id = existing_image_id;
                image_ids.push_back(existing_image_id);
                return !flexible_formats && existing.format == info.format;
            }
        } else if (IsSubresource(info, existing_image, gpu_addr, options, broken_views,
                                 native_bgr)) {
            image_id = existing_image_id;
            image_ids.push_back(existing_image_id);
            return !flexible_formats && existing_image.info.format == info.format;
        }
        return false;
    };
    ForEachImageInRegion(*cpu_addr, CalculateGuestSizeInBytes(info), lambda);
    if (image_ids.size() <= 1) [[likely]] {
        return image_id;
    }
    auto image_ids_compare = [this](ImageId a, ImageId b) {
        auto& image_a = slot_images[a];
        auto& image_b = slot_images[b];
        return image_a.modification_tick < image_b.modification_tick;
    };
    return *std::ranges::max_element(image_ids, image_ids_compare);
}

template <class P>
bool TextureCache<P>::ImageCanRescale(ImageBase& image) {
    if (!image.info.rescaleable) {
        return false;
    }
    if (videoSettings.resolution_info.downscale && !image.info.downscaleable) {
        return false;
    }
    if (True(image.flags & (ImageFlagBits::Rescaled | ImageFlagBits::CheckingRescalable))) {
        return true;
    }
    if (True(image.flags & ImageFlagBits::IsRescalable)) {
        return true;
    }
    image.flags |= ImageFlagBits::CheckingRescalable;
    for (const auto& alias : image.aliased_images) {
        Image& other_image = slot_images[alias.id];
        if (!ImageCanRescale(other_image)) {
            image.flags &= ~ImageFlagBits::CheckingRescalable;
            return false;
        }
    }
    image.flags &= ~ImageFlagBits::CheckingRescalable;
    image.flags |= ImageFlagBits::IsRescalable;
    return true;
}

template <class P>
void TextureCache<P>::InvalidateScale(Image& image) {
    if (image.scale_tick <= frame_tick) {
        image.scale_tick = frame_tick + 1;
    }
    const std::span<const ImageViewId> image_view_ids = image.image_view_ids;
    auto& dirty = maxwell3d->dirty.flags;
    dirty[Dirty::RenderTargets] = true;
    dirty[Dirty::ZetaBuffer] = true;
    for (size_t rt = 0; rt < NUM_RT; ++rt) {
        dirty[Dirty::ColorBuffer0 + rt] = true;
    }
    for (const ImageViewId image_view_id : image_view_ids) {
        std::ranges::replace(render_targets.color_buffer_ids, image_view_id, ImageViewId{});
        if (render_targets.depth_buffer_id == image_view_id) {
            render_targets.depth_buffer_id = ImageViewId{};
        }
    }
    RemoveImageViewReferences(image_view_ids);
    RemoveFramebuffers(image_view_ids);
    for (const ImageViewId image_view_id : image_view_ids) {
        sentenced_image_view.Push(std::move(slot_image_views[image_view_id]));
        slot_image_views.erase(image_view_id);
    }
    image.image_view_ids.clear();
    image.image_view_infos.clear();
    for (size_t c : active_channel_ids) {
        auto& channel_info = channel_storage[c];
        if constexpr (ENABLE_VALIDATION) {
            std::ranges::fill(channel_info.graphics_image_view_ids, CORRUPT_ID);
            std::ranges::fill(channel_info.compute_image_view_ids, CORRUPT_ID);
        }
        channel_info.graphics_image_table.Invalidate();
        channel_info.compute_image_table.Invalidate();
    }
    has_deleted_images = true;
}

template <class P>
u64 TextureCache<P>::GetScaledImageSizeBytes(const ImageBase& image) {
    const u64 scale_up = static_cast<u64>(videoSettings.resolution_info.up_scale *
                                          videoSettings.resolution_info.up_scale);
    const u64 down_shift = static_cast<u64>(videoSettings.resolution_info.down_shift +
                                            videoSettings.resolution_info.down_shift);
    const u64 image_size_bytes =
        static_cast<u64>(std::max(image.guest_size_bytes, image.unswizzled_size_bytes));
    const u64 tentative_size = (image_size_bytes * scale_up) >> down_shift;
    const u64 fitted_size = Common::AlignUp(tentative_size, 1024);
    return fitted_size;
}

template <class P>
void TextureCache<P>::QueueAsyncDecode(Image& image, ImageId image_id) {
    UNIMPLEMENTED_IF(False(image.flags & ImageFlagBits::Converted));
    LOG_INFO(HW_GPU, "Queuing async texture decode");

    image.flags |= ImageFlagBits::IsDecoding;
    auto decode = std::make_unique<AsyncDecodeContext>();
    auto* decode_ptr = decode.get();
    decode->image_id = image_id;
    async_decodes.push_back(std::move(decode));

    static Common::ScratchBuffer<u8> local_unswizzle_data_buffer;
    local_unswizzle_data_buffer.resize_destructive(image.unswizzled_size_bytes);
    Tegra::Memory::GpuGuestMemory<u8, GuestMemoryFlags::UnsafeRead> swizzle_data(*gpu_memory, image.gpu_addr, image.guest_size_bytes, &swizzle_data_buffer);

    auto copies = UnswizzleImage(*gpu_memory, image.gpu_addr, image.info, swizzle_data,
                                 local_unswizzle_data_buffer);
    const size_t out_size = MapSizeBytes(image);

    auto func = [out_size, copies, info = image.info,
                 input = std::move(local_unswizzle_data_buffer),
                 async_decode = decode_ptr]() mutable {
        async_decode->decoded_data.resize_destructive(out_size);
        std::span copies_span{copies.data(), copies.size()};
        ConvertImage(input, info, async_decode->decoded_data, copies_span);

        // TODO: Do we need this lock?
        std::unique_lock lock{async_decode->mutex};
        async_decode->copies = std::move(copies);
        async_decode->complete = true;
    };
    texture_decode_worker.QueueWork(std::move(func));
}

template <class P>
void TextureCache<P>::TickAsyncDecode() {
    bool has_uploads{};
    auto i = async_decodes.begin();
    while (i != async_decodes.end()) {
        auto* async_decode = i->get();
        std::unique_lock lock{async_decode->mutex};
        if (!async_decode->complete) {
            ++i;
            continue;
        }
        Image& image = slot_images[async_decode->image_id];
        auto staging = runtime.UploadStagingBuffer(MapSizeBytes(image));
        std::memcpy(staging.mapped_span.data(), async_decode->decoded_data.data(),
                    async_decode->decoded_data.size());
        image.UploadMemory(staging, async_decode->copies);
        image.flags &= ~ImageFlagBits::IsDecoding;
        has_uploads = true;
        i = async_decodes.erase(i);
    }
    if (has_uploads) {
        runtime.InsertUploadMemoryBarrier();
    }
}

template <class P>
bool TextureCache<P>::ScaleUp(Image& image) {
    const bool has_copy = image.HasScaled();
    const bool rescaled = image.ScaleUp();
    if (!rescaled) {
        return false;
    }
    if (!has_copy) {
        total_used_memory += GetScaledImageSizeBytes(image);
    }
    InvalidateScale(image);
    return true;
}

template <class P>
bool TextureCache<P>::ScaleDown(Image& image) {
    const bool rescaled = image.ScaleDown();
    if (!rescaled) {
        return false;
    }
    InvalidateScale(image);
    return true;
}

template <class P>
ImageId TextureCache<P>::InsertImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                     RelaxedOptions options, bool discard_contents) {
    std::optional<DAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
    if (!cpu_addr) {
        const auto size = CalculateGuestSizeInBytes(info);
        cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr, size);
        if (!cpu_addr) {
            const DAddr fake_addr = ~(1ULL << 40ULL) + virtual_invalid_space;
            virtual_invalid_space += Common::AlignUp(size, 32);
            cpu_addr = std::optional<DAddr>(fake_addr);
        }
    }
    ASSERT_MSG(cpu_addr, "Tried to insert an image to an invalid gpu_addr=0x{:x}", gpu_addr);
    const ImageId image_id = JoinImages(info, gpu_addr, *cpu_addr, discard_contents);
    const Image& image = slot_images[image_id];
    // Using "image.gpu_addr" instead of "gpu_addr" is important because it might be different
    const auto [it, is_new] = image_allocs_table.try_emplace(image.gpu_addr);
    if (is_new) {
        it->second = slot_image_allocs.insert();
    }
    slot_image_allocs[it->second].images.push_back(image_id);
    return image_id;
}

template <class P>
ImageId TextureCache<P>::JoinImages(const ImageInfo& info, GPUVAddr gpu_addr, DAddr cpu_addr,
                                    bool discard_contents) {
    const GPUVAddr requested_address = gpu_addr;
    ImageInfo new_info = info;
    const size_t size_bytes = CalculateGuestSizeInBytes(new_info);
    const bool broken_views = runtime.HasBrokenTextureViewFormats();
    const bool native_bgr = runtime.HasNativeBgr();
    join_overlap_ids.clear();
    join_overlaps_found.clear();
    join_left_aliased_ids.clear();
    join_right_aliased_ids.clear();
    join_ignore_textures.clear();
    join_bad_overlap_ids.clear();
    join_copies_to_do.clear();
    join_alias_indices.clear();
    const bool this_is_linear = info.type == ImageType::Linear;
    const auto region_check = [&](ImageId overlap_id, ImageBase& overlap) {
        if (True(overlap.flags & ImageFlagBits::Remapped)) {
            join_ignore_textures.insert(overlap_id);
            return;
        }
        const bool overlap_is_linear = overlap.info.type == ImageType::Linear;
        if (this_is_linear != overlap_is_linear) {
            return;
        }
        if (this_is_linear && overlap_is_linear) {
            if (info.pitch == overlap.info.pitch && gpu_addr == overlap.gpu_addr) {
                // Alias linear images with the same pitch
                join_left_aliased_ids.push_back(overlap_id);
            }
            return;
        }
        join_overlaps_found.insert(overlap_id);
        static constexpr bool strict_size = true;
        const std::optional<OverlapResult> solution = ResolveOverlap(
            new_info, gpu_addr, cpu_addr, overlap, strict_size, broken_views, native_bgr);
        if (solution) {
            gpu_addr = solution->gpu_addr;
            cpu_addr = solution->cpu_addr;
            new_info.resources = solution->resources;
            join_overlap_ids.push_back(overlap_id);
            join_copies_to_do.emplace_back(JoinCopy{false, overlap_id});
            return;
        }
        static constexpr auto options = RelaxedOptions::Size | RelaxedOptions::Format;
        const ImageBase new_image_base(new_info, gpu_addr, cpu_addr);
        if (IsSubresource(new_info, overlap, gpu_addr, options, broken_views, native_bgr)) {
            join_left_aliased_ids.push_back(overlap_id);
            overlap.flags |= ImageFlagBits::Alias;
            join_copies_to_do.emplace_back(JoinCopy{true, overlap_id});
        } else if (IsSubresource(overlap.info, new_image_base, overlap.gpu_addr, options,
                                 broken_views, native_bgr)) {
            join_right_aliased_ids.push_back(overlap_id);
            overlap.flags |= ImageFlagBits::Alias;
            join_copies_to_do.emplace_back(JoinCopy{true, overlap_id});
        } else {
            join_bad_overlap_ids.push_back(overlap_id);
        }
    };
    ForEachImageInRegion(cpu_addr, size_bytes, region_check);
    const auto region_check_gpu = [&](ImageId overlap_id, ImageBase& overlap) {
        if (!join_overlaps_found.contains(overlap_id)) {
            if (True(overlap.flags & ImageFlagBits::Remapped)) {
                join_ignore_textures.insert(overlap_id);
            }
            if (overlap.gpu_addr == gpu_addr && overlap.guest_size_bytes == size_bytes) {
                join_ignore_textures.insert(overlap_id);
            }
        }
    };
    ForEachSparseImageInRegion(channel_state->gpu_memory.GetID(), gpu_addr, size_bytes,
                               region_check_gpu);

    bool can_rescale = info.rescaleable;
    bool any_rescaled = false;
    for (const auto& copy : join_copies_to_do) {
        if (!can_rescale) {
            break;
        }
        Image& sibling = slot_images[copy.id];
        can_rescale &= ImageCanRescale(sibling);
        any_rescaled |= True(sibling.flags & ImageFlagBits::Rescaled);
    }

    can_rescale &= any_rescaled;

    if (can_rescale) {
        for (const auto& copy : join_copies_to_do) {
            Image& sibling = slot_images[copy.id];
            ScaleUp(sibling);
        }
    } else {
        for (const auto& copy : join_copies_to_do) {
            Image& sibling = slot_images[copy.id];
            ScaleDown(sibling);
        }
    }

    const ImageId new_image_id = slot_images.insert(runtime, new_info, gpu_addr, cpu_addr);
    Image& new_image = slot_images[new_image_id];

    if (!gpu_memory->IsContinuousRange(new_image.gpu_addr, new_image.guest_size_bytes) &&
        new_info.is_sparse) {
        new_image.flags |= ImageFlagBits::Sparse;
    }

    for (const ImageId overlap_id : join_ignore_textures) {
        Image& overlap = slot_images[overlap_id];
        if (True(overlap.flags & ImageFlagBits::GpuModified)) {
            UNIMPLEMENTED();
        }
        if (True(overlap.flags & ImageFlagBits::Tracked)) {
            UntrackImage(overlap, overlap_id);
        }
        UnregisterImage(overlap_id);
        DeleteImage(overlap_id);
    }

    std::ranges::sort(join_copies_to_do, [this](const JoinCopy& lhs, const JoinCopy& rhs) {
        const ImageBase& lhs_image = slot_images[lhs.id];
        const ImageBase& rhs_image = slot_images[rhs.id];
        return lhs_image.modification_tick < rhs_image.modification_tick;
    });

    ImageBase& new_image_base = new_image;
    for (const ImageId aliased_id : join_right_aliased_ids) {
        ImageBase& aliased = slot_images[aliased_id];
        size_t alias_index = new_image_base.aliased_images.size();
        if (!AddImageAlias(new_image_base, aliased, new_image_id, aliased_id)) {
            continue;
        }
        join_alias_indices.emplace(aliased_id, alias_index);
        new_image.flags |= ImageFlagBits::Alias;
    }
    for (const ImageId aliased_id : join_left_aliased_ids) {
        ImageBase& aliased = slot_images[aliased_id];
        size_t alias_index = new_image_base.aliased_images.size();
        if (!AddImageAlias(aliased, new_image_base, aliased_id, new_image_id)) {
            continue;
        }
        join_alias_indices.emplace(aliased_id, alias_index);
        new_image.flags |= ImageFlagBits::Alias;
    }
    for (const ImageId aliased_id : join_bad_overlap_ids) {
        ImageBase& aliased = slot_images[aliased_id];
        aliased.overlapping_images.push_back(new_image_id);
        new_image.overlapping_images.push_back(aliased_id);
        if (aliased.info.resources.levels == 1 && aliased.info.block.depth == 0 &&
            aliased.overlapping_images.size() > 1) {
            aliased.flags |= ImageFlagBits::BadOverlap;
        }
        if (new_image.info.resources.levels == 1 && new_image.info.block.depth == 0 &&
            new_image.overlapping_images.size() > 1) {
            new_image.flags |= ImageFlagBits::BadOverlap;
        }
    }

    ImageCopyCoverage coverage{new_info};
    // When this join cannot be rescaled, its sources have already been brought
    // back to native size above, even if resolution scaling is enabled globally.
    const bool allow_upload_elision =
        !can_rescale && new_info.type == ImageType::e2D && new_info.num_samples == 1 &&
        DefaultBlockWidth(new_info.format) == 1 && DefaultBlockHeight(new_info.format) == 1 &&
        False(new_image.flags & (ImageFlagBits::Converted | ImageFlagBits::AsynchronousDecode));
    if (allow_upload_elision) {
        for (const auto& copy_object : join_copies_to_do) {
            const Image& overlap = slot_images[copy_object.id];
            if (overlap.info.num_samples != 1 || overlap.info.format != new_info.format ||
                !overlap.IsSafeDownload()) {
                continue;
            }
            if (copy_object.is_alias) {
                const auto alias = join_alias_indices.find(copy_object.id);
                if (alias != join_alias_indices.end()) {
                    coverage.Add(overlap.info, new_image.aliased_images[alias->second].copies);
                }
            } else {
                const auto base = new_image.TryFindBase(overlap.gpu_addr);
                if (base) {
                    coverage.Add(overlap.info,
                                 MakeShrinkImageCopies(new_info, overlap.info, *base, 1, 0));
                }
            }
        }
    }
    const bool discard_for_clear =
        discard_contents && ClearCoversJoinedImage(info, new_info, requested_address, gpu_addr) &&
        False(new_image.flags & (ImageFlagBits::Converted | ImageFlagBits::AsynchronousDecode));
    if (discard_for_clear || (allow_upload_elision && coverage.Complete())) {
        // Every subresource will be replaced by the imminent full clear or by
        // the GPU copies below. Preserve layout transitions and memory tracking.
        new_image.flags &= ~ImageFlagBits::CpuModified;
        new_image.flags |= ImageFlagBits::GpuModified;
        UpdateNativeReadTracking(new_image, new_image_id);
        TrackImage(new_image, new_image_id);
        runtime.TransitionImageLayout(new_image);
    } else {
        RefreshContents(new_image, new_image_id);
    }
    if (can_rescale) {
        ScaleUp(new_image);
    } else {
        ScaleDown(new_image);
    }

    for (const auto& copy_object : join_copies_to_do) {
        Image& overlap = slot_images[copy_object.id];
        if (copy_object.is_alias) {
            if (!overlap.IsSafeDownload()) {
                continue;
            }
            const auto alias_pointer = join_alias_indices.find(copy_object.id);
            if (alias_pointer == join_alias_indices.end()) {
                continue;
            }
            const AliasedImage& aliased = new_image.aliased_images[alias_pointer->second];
            CopyImage(new_image_id, aliased.id, aliased.copies);
            new_image.modification_tick = overlap.modification_tick;
            continue;
        }
        if (True(overlap.flags & ImageFlagBits::GpuModified)) {
            new_image.flags |= ImageFlagBits::GpuModified;
            UpdateNativeReadTracking(new_image, new_image_id);
            const auto& resolution = videoSettings.resolution_info;
            const SubresourceBase base = new_image.TryFindBase(overlap.gpu_addr).value();
            const u32 up_scale = can_rescale ? resolution.up_scale : 1;
            const u32 down_shift = can_rescale ? resolution.down_shift : 0;
            auto copies = MakeShrinkImageCopies(new_info, overlap.info, base, up_scale, down_shift);
            if (overlap.info.num_samples != new_image.info.num_samples) {
                runtime.CopyImageMSAA(new_image, overlap, std::move(copies));
            } else {
                runtime.CopyImage(new_image, overlap, std::move(copies));
            }
            new_image.modification_tick = overlap.modification_tick;
        }
        if (True(overlap.flags & ImageFlagBits::Tracked)) {
            UntrackImage(overlap, copy_object.id);
        }
        UnregisterImage(copy_object.id);
        DeleteImage(copy_object.id);
    }

    RegisterImage(new_image_id);
    return new_image_id;
}

template <class P>
std::optional<typename TextureCache<P>::BlitImages> TextureCache<P>::GetBlitImages(
    const Tegra::Engines::Fermi2D::Surface& dst, const Tegra::Engines::Fermi2D::Surface& src,
    const Tegra::Engines::Fermi2D::Config& copy) {

    static constexpr auto FIND_OPTIONS = RelaxedOptions::Samples;
    const GPUVAddr dst_addr = dst.Address();
    const GPUVAddr src_addr = src.Address();
    ImageInfo dst_info(dst);
    ImageInfo src_info(src);
    const bool can_be_depth_blit =
        dst_info.format == src_info.format && copy.filter == Tegra::Engines::Fermi2D::Filter::Point;
    ImageId dst_id;
    ImageId src_id;
    RelaxedOptions try_options = FIND_OPTIONS;
    if (can_be_depth_blit) {
        try_options |= RelaxedOptions::Format;
    }
    do {
        has_deleted_images = false;
        src_id = FindImage(src_info, src_addr, try_options);
        dst_id = FindImage(dst_info, dst_addr, try_options);
        if (!copy.must_accelerate) {
            do {
                if (!src_id && !dst_id) {
                    return std::nullopt;
                }
                if (src_id && True(slot_images[src_id].flags & ImageFlagBits::GpuModified)) {
                    break;
                }
                if (dst_id && True(slot_images[dst_id].flags & ImageFlagBits::GpuModified)) {
                    break;
                }
                return std::nullopt;
            } while (false);
        }
        const ImageBase* const src_image = src_id ? &slot_images[src_id] : nullptr;
        if (src_image && src_image->info.num_samples > 1) {
            RelaxedOptions find_options{FIND_OPTIONS | RelaxedOptions::ForceBrokenViews};
            src_id = FindOrInsertImage(src_info, src_addr, find_options);
            dst_id = FindOrInsertImage(dst_info, dst_addr, find_options);
            if (has_deleted_images) {
                continue;
            }
            break;
        }
        if (can_be_depth_blit) {
            const ImageBase* const dst_image = dst_id ? &slot_images[dst_id] : nullptr;
            DeduceBlitImages(dst_info, src_info, dst_image, src_image);
            if (GetFormatType(dst_info.format) != GetFormatType(src_info.format)) {
                continue;
            }
        }
        if (!src_id) {
            src_id = InsertImage(src_info, src_addr, RelaxedOptions{});
        }
        if (!dst_id) {
            dst_id = InsertImage(dst_info, dst_addr, RelaxedOptions{});
        }
    } while (has_deleted_images);
    const ImageBase& src_image = slot_images[src_id];
    const ImageBase& dst_image = slot_images[dst_id];
    const bool native_bgr = runtime.HasNativeBgr();
    if (GetFormatType(dst_info.format) != GetFormatType(dst_image.info.format) ||
        GetFormatType(src_info.format) != GetFormatType(src_image.info.format) ||
        !VideoCore::Surface::IsViewCompatible(dst_info.format, dst_image.info.format, false,
                                              native_bgr) ||
        !VideoCore::Surface::IsViewCompatible(src_info.format, src_image.info.format, false,
                                              native_bgr)) {
        // Make sure the images match the expected format.
        do {
            has_deleted_images = false;
            src_id = FindOrInsertImage(src_info, src_addr, RelaxedOptions{});
            dst_id = FindOrInsertImage(dst_info, dst_addr, RelaxedOptions{});
        } while (has_deleted_images);
    }
    return {BlitImages{
        .dst_id = dst_id,
        .src_id = src_id,
        .dst_format = dst_info.format,
        .src_format = src_info.format,
    }};
}

template <class P>
ImageId TextureCache<P>::FindDMAImage(const ImageInfo& info, GPUVAddr gpu_addr) {
    std::optional<DAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
    if (!cpu_addr) {
        cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr, CalculateGuestSizeInBytes(info));
        if (!cpu_addr) {
            return ImageId{};
        }
    }
    ImageId image_id{};
    boost::container::small_vector<ImageId, 8> image_ids;
    const auto lambda = [&](ImageId existing_image_id, ImageBase& existing_image) {
        if (True(existing_image.flags & ImageFlagBits::Remapped)) {
            return false;
        }
        if (info.type == ImageType::Linear || existing_image.info.type == ImageType::Linear)
            [[unlikely]] {
            const bool strict_size = True(existing_image.flags & ImageFlagBits::Strong);
            const ImageInfo& existing = existing_image.info;
            if (existing_image.gpu_addr == gpu_addr && existing.type == info.type &&
                existing.pitch == info.pitch &&
                IsPitchLinearSameSize(existing, info, strict_size) &&
                IsViewCompatible(existing.format, info.format, false, true)) {
                image_id = existing_image_id;
                image_ids.push_back(existing_image_id);
                return true;
            }
        } else if (IsSubCopy(info, existing_image, gpu_addr)) {
            image_id = existing_image_id;
            image_ids.push_back(existing_image_id);
            return true;
        }
        return false;
    };
    ForEachImageInRegion(*cpu_addr, CalculateGuestSizeInBytes(info), lambda);
    if (image_ids.size() <= 1) [[likely]] {
        return image_id;
    }
    auto image_ids_compare = [this](ImageId a, ImageId b) {
        auto& image_a = slot_images[a];
        auto& image_b = slot_images[b];
        return image_a.modification_tick < image_b.modification_tick;
    };
    return *std::ranges::max_element(image_ids, image_ids_compare);
}

template <class P>
std::pair<u32, u32> TextureCache<P>::PrepareDmaImage(ImageId dst_id, GPUVAddr base_addr,
                                                     bool mark_as_modified) {
    const auto& image = slot_images[dst_id];
    const auto base = image.TryFindBase(base_addr);
    PrepareImage(dst_id, mark_as_modified, false);
    const auto& new_image = slot_images[dst_id];
    lru_cache.Touch(new_image.lru_index, frame_tick);
    return std::make_pair(base->level, base->layer);
}

template <class P>
SamplerId TextureCache<P>::FindSampler(const TSCEntry& config) {
    if (std::ranges::all_of(config.raw, [](u64 value) { return value == 0; })) {
        return NULL_SAMPLER_ID;
    }
    const auto [pair, is_new] = channel_state->samplers.try_emplace(config);
    if (is_new) {
        pair->second = slot_samplers.insert(runtime, config);
    }
    return pair->second;
}

template <class P>
bool TextureCache<P>::CanDiscardOnClear(const ImageInfo& info, bool is_color, size_t index,
                                        u32 clear_layer_count) {
    if (clear_layer_count == 0) {
        return false;
    }
    const auto& regs = maxwell3d->regs;
    const auto& clear = regs.clear_surface;
    const auto& scissor = regs.scissor_test[0];
    bool all_aspects = false;
    if (is_color) {
        all_aspects = index == clear.RT && clear.R && clear.G && clear.B && clear.A;
    } else {
        switch (GetFormatType(info.format)) {
        case SurfaceType::Depth:
            all_aspects = clear.Z;
            break;
        case SurfaceType::Stencil:
            all_aspects = clear.S;
            break;
        case SurfaceType::DepthStencil:
            all_aspects = clear.Z && clear.S;
            break;
        default:
            break;
        }
    }
    const bool scissor_enabled = regs.clear_control.use_scissor && scissor.enable;
    if (scissor_enabled &&
        regs.window_origin.mode != Tegra::Engines::Maxwell3D::Regs::WindowOrigin::Mode::UpperLeft) {
        return false;
    }
    const FullClearCoverage coverage{
        .all_aspects = all_aspects,
        .viewport_clip = regs.clear_control.use_viewport_clip0 != 0,
        .first_layer = clear.layer,
        .layer_count = clear_layer_count,
        .area_width = regs.surface_clip.width,
        .area_height = regs.surface_clip.height,
        .scissor_enabled = scissor_enabled,
        .min_x = static_cast<s32>(scissor.min_x.Value()),
        .min_y = static_cast<s32>(scissor.min_y.Value()),
        .max_x = static_cast<s32>(scissor.max_x.Value()),
        .max_y = static_cast<s32>(scissor.max_y.Value()),
    };
    if (!IsWholeImageClear(info, coverage)) {
        return false;
    }
    const GPUVAddr target_address = is_color ? regs.rt[index].Address() : regs.zeta.Address();
    const size_t target_size = CalculateGuestSizeInBytes(info);
    const auto target_cpu = gpu_memory->GpuToCpuAddress(target_address);
    if (!target_cpu || info.is_sparse ||
        !gpu_memory->IsContinuousRange(target_address, target_size)) {
        return false;
    }
    // Vulkan clamps attachment clears to the common framebuffer render area.
    // A smaller sibling attachment must therefore preserve the initial contents.
    const auto covers_target = [&](const ImageInfo& sibling, GPUVAddr address) {
        if (sibling.num_samples != 1 || sibling.size.width < info.size.width ||
            sibling.size.height < info.size.height || sibling.is_sparse) {
            return false;
        }
        const size_t size = CalculateGuestSizeInBytes(sibling);
        const auto cpu = gpu_memory->GpuToCpuAddress(address);
        if (!cpu || !gpu_memory->IsContinuousRange(address, size)) {
            return false;
        }
        // Another bound attachment must not consume discarded contents while
        // the framebuffer is being assembled, including through a CPU alias.
        const bool gpu_overlap =
            address < target_address + target_size && target_address < address + size;
        const bool cpu_overlap = *cpu < *target_cpu + target_size && *target_cpu < *cpu + size;
        return !gpu_overlap && !cpu_overlap;
    };
    for (size_t rt = 0; rt < regs.rt_control.count; ++rt) {
        if (is_color && rt == index) {
            continue;
        }
        if (regs.rt[rt].Address() != 0 && regs.rt[rt].format != Tegra::RenderTargetFormat::NONE &&
            !covers_target(ImageInfo(regs.rt[rt], regs.anti_alias_samples_mode),
                           regs.rt[rt].Address())) {
            return false;
        }
    }
    if (is_color && regs.zeta_enable && regs.zeta.Address() != 0 &&
        !covers_target(ImageInfo(regs.zeta, regs.zeta_size, regs.anti_alias_samples_mode),
                       regs.zeta.Address())) {
        return false;
    }
    return true;
}

template <class P>
ImageViewId TextureCache<P>::FindColorBuffer(size_t index, u32 clear_layer_count) {
    const auto& regs = maxwell3d->regs;
    if (index >= regs.rt_control.count) {
        return ImageViewId{};
    }
    const auto& rt = regs.rt[index];
    const GPUVAddr gpu_addr = rt.Address();
    if (gpu_addr == 0) {
        return ImageViewId{};
    }
    if (rt.format == Tegra::RenderTargetFormat::NONE) {
        return ImageViewId{};
    }
    const ImageInfo info(regs.rt[index], regs.anti_alias_samples_mode);
    return FindRenderTargetView(info, gpu_addr,
                                CanDiscardOnClear(info, true, index, clear_layer_count));
}

template <class P>
ImageViewId TextureCache<P>::FindDepthBuffer(u32 clear_layer_count) {
    const auto& regs = maxwell3d->regs;
    if (!regs.zeta_enable) {
        return ImageViewId{};
    }
    const GPUVAddr gpu_addr = regs.zeta.Address();
    if (gpu_addr == 0) {
        return ImageViewId{};
    }
    const ImageInfo info(regs.zeta, regs.zeta_size, regs.anti_alias_samples_mode);
    return FindRenderTargetView(info, gpu_addr,
                                CanDiscardOnClear(info, false, 0, clear_layer_count));
}

template <class P>
ImageViewId TextureCache<P>::FindRenderTargetView(const ImageInfo& info, GPUVAddr gpu_addr,
                                                  bool discard_contents) {
    ImageId image_id{};
    bool delete_state = has_deleted_images;
    do {
        has_deleted_images = false;
        image_id = FindOrInsertImage(info, gpu_addr, RelaxedOptions{}, discard_contents);
        delete_state |= has_deleted_images;
    } while (has_deleted_images);
    has_deleted_images = delete_state;
    if (!image_id) {
        return NULL_IMAGE_VIEW_ID;
    }
    Image& image = slot_images[image_id];
    const ImageViewType view_type = RenderTargetImageViewType(info);
    SubresourceBase base;
    if (image.info.type == ImageType::Linear) {
        base = SubresourceBase{.level = 0, .layer = 0};
    } else {
        base = image.TryFindBase(gpu_addr).value();
    }
    const s32 layers = image.info.type == ImageType::e3D ? info.size.depth : info.resources.layers;
    const SubresourceRange range{
        .base = base,
        .extent = {.levels = 1, .layers = layers},
    };
    return FindOrEmplaceImageView(image_id, ImageViewInfo(view_type, info.format, range));
}

template <class P>
template <typename Func>
void TextureCache<P>::ForEachImageInRegion(DAddr cpu_addr, size_t size, Func&& func) {
    using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
    static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
    boost::container::small_vector<ImageId, 32> images;
    boost::container::small_vector<ImageMapId, 32> maps;
    ForEachCPUPage(cpu_addr, size, [this, &images, &maps, cpu_addr, size, func](u64 page) {
        const auto it = page_table.find(page);
        if (it == page_table.end()) {
            if constexpr (BOOL_BREAK) {
                return false;
            } else {
                return;
            }
        }
        for (const ImageMapId map_id : it->second) {
            ImageMapView& map = slot_map_views[map_id];
            if (map.picked) {
                continue;
            }
            if (!map.Overlaps(cpu_addr, size)) {
                continue;
            }
            map.picked = true;
            maps.push_back(map_id);
            Image& image = slot_images[map.image_id];
            if (True(image.flags & ImageFlagBits::Picked)) {
                continue;
            }
            image.flags |= ImageFlagBits::Picked;
            images.push_back(map.image_id);
            if constexpr (BOOL_BREAK) {
                if (func(map.image_id, image)) {
                    return true;
                }
            } else {
                func(map.image_id, image);
            }
        }
        if constexpr (BOOL_BREAK) {
            return false;
        }
    });
    for (const ImageId image_id : images) {
        slot_images[image_id].flags &= ~ImageFlagBits::Picked;
    }
    for (const ImageMapId map_id : maps) {
        slot_map_views[map_id].picked = false;
    }
}

template <class P>
template <typename Func>
void TextureCache<P>::ForEachImageInRegionGPU(size_t as_id, GPUVAddr gpu_addr, size_t size,
                                              Func&& func) {
    using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
    static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
    boost::container::small_vector<ImageId, 8> images;
    auto storage_id = getStorageID(as_id);
    if (!storage_id) {
        return;
    }
    auto& gpu_page_table = gpu_page_table_storage[*storage_id * 2];
    ForEachGPUPage(gpu_addr, size,
                   [this, &gpu_page_table, &images, gpu_addr, size, func](u64 page) {
                       const auto it = gpu_page_table.find(page);
                       if (it == gpu_page_table.end()) {
                           if constexpr (BOOL_BREAK) {
                               return false;
                           } else {
                               return;
                           }
                       }
                       for (const ImageId image_id : it->second) {
                           Image& image = slot_images[image_id];
                           if (True(image.flags & ImageFlagBits::Picked)) {
                               continue;
                           }
                           if (!image.OverlapsGPU(gpu_addr, size)) {
                               continue;
                           }
                           image.flags |= ImageFlagBits::Picked;
                           images.push_back(image_id);
                           if constexpr (BOOL_BREAK) {
                               if (func(image_id, image)) {
                                   return true;
                               }
                           } else {
                               func(image_id, image);
                           }
                       }
                       if constexpr (BOOL_BREAK) {
                           return false;
                       }
                   });
    for (const ImageId image_id : images) {
        slot_images[image_id].flags &= ~ImageFlagBits::Picked;
    }
}

template <class P>
template <typename Func>
void TextureCache<P>::ForEachSparseImageInRegion(size_t as_id, GPUVAddr gpu_addr, size_t size,
                                                 Func&& func) {
    using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
    static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
    boost::container::small_vector<ImageId, 8> images;
    auto storage_id = getStorageID(as_id);
    if (!storage_id) {
        return;
    }
    auto& sparse_page_table = gpu_page_table_storage[*storage_id * 2 + 1];
    ForEachGPUPage(gpu_addr, size,
                   [this, &sparse_page_table, &images, gpu_addr, size, func](u64 page) {
                       const auto it = sparse_page_table.find(page);
                       if (it == sparse_page_table.end()) {
                           if constexpr (BOOL_BREAK) {
                               return false;
                           } else {
                               return;
                           }
                       }
                       for (const ImageId image_id : it->second) {
                           Image& image = slot_images[image_id];
                           if (True(image.flags & ImageFlagBits::Picked)) {
                               continue;
                           }
                           if (!image.OverlapsGPU(gpu_addr, size)) {
                               continue;
                           }
                           image.flags |= ImageFlagBits::Picked;
                           images.push_back(image_id);
                           if constexpr (BOOL_BREAK) {
                               if (func(image_id, image)) {
                                   return true;
                               }
                           } else {
                               func(image_id, image);
                           }
                       }
                       if constexpr (BOOL_BREAK) {
                           return false;
                       }
                   });
    for (const ImageId image_id : images) {
        slot_images[image_id].flags &= ~ImageFlagBits::Picked;
    }
}

template <class P>
template <typename Func>
void TextureCache<P>::ForEachSparseSegment(ImageBase& image, Func&& func) {
    using FuncReturn = typename std::invoke_result<Func, GPUVAddr, DAddr, size_t>::type;
    static constexpr bool RETURNS_BOOL = std::is_same_v<FuncReturn, bool>;
    const auto segments = gpu_memory->GetSubmappedRange(image.gpu_addr, image.guest_size_bytes);
    for (const auto& [gpu_addr, size] : segments) {
        std::optional<DAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
        ASSERT(cpu_addr);
        if constexpr (RETURNS_BOOL) {
            if (func(gpu_addr, *cpu_addr, size)) {
                break;
            }
        } else {
            func(gpu_addr, *cpu_addr, size);
        }
    }
}

template <class P>
ImageViewId TextureCache<P>::FindOrEmplaceImageView(ImageId image_id, const ImageViewInfo& info) {
    Image& image = slot_images[image_id];
    if (const ImageViewId image_view_id = image.FindView(info); image_view_id) {
        return image_view_id;
    }
    const ImageViewId image_view_id =
        slot_image_views.insert(runtime, info, image_id, image, slot_images);
    image.InsertView(info, image_view_id);
    return image_view_id;
}

template <class P>
void TextureCache<P>::RegisterImage(ImageId image_id) {
    ImageBase& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    u64 tentative_size = std::max(image.guest_size_bytes, image.unswizzled_size_bytes);
    if ((IsPixelFormatASTC(image.info.format) &&
         True(image.flags & ImageFlagBits::AcceleratedUpload)) ||
        True(image.flags & ImageFlagBits::Converted)) {
        tentative_size = TranscodedAstcSize(tentative_size, image.info.format);
    }
    total_used_memory += Common::AlignUp(tentative_size, 1024);
    image.lru_index = lru_cache.Insert(image_id, frame_tick);
    image.gpu_page_table_index = channel_state->gpu_page_table_index;

    ForEachGPUPage(image.gpu_addr, image.guest_size_bytes, [this, image_id](u64 page) {
        (*channel_state->gpu_page_table)[page].push_back(image_id);
    });
    if (False(image.flags & ImageFlagBits::Sparse)) {
        device_memory.RegisterFlushCacheRegion(image.cpu_addr, image.guest_size_bytes,
                                              CacheType::TextureCache);
        auto map_id =
            slot_map_views.insert(image.gpu_addr, image.cpu_addr, image.guest_size_bytes, image_id);
        ForEachCPUPage(image.cpu_addr, image.guest_size_bytes,
                       [this, map_id](u64 page) { page_table[page].push_back(map_id); });
        image.map_view_id = map_id;
        return;
    }
    boost::container::small_vector<ImageViewId, 16> sparse_maps;
    ForEachSparseSegment(
        image, [this, image_id, &sparse_maps](GPUVAddr gpu_addr, DAddr cpu_addr, size_t size) {
            device_memory.RegisterFlushCacheRegion(cpu_addr, size, CacheType::TextureCache);
            auto map_id = slot_map_views.insert(gpu_addr, cpu_addr, size, image_id);
            ForEachCPUPage(cpu_addr, size,
                           [this, map_id](u64 page) { page_table[page].push_back(map_id); });
            sparse_maps.push_back(map_id);
        });
    sparse_views.emplace(image_id, std::move(sparse_maps));
    ForEachGPUPage(image.gpu_addr, image.guest_size_bytes, [this, image_id](u64 page) {
        (*channel_state->sparse_page_table)[page].push_back(image_id);
    });
}

template <class P>
void TextureCache<P>::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already registered image");
    image.flags &= ~ImageFlagBits::Registered;
    image.flags &= ~ImageFlagBits::BadOverlap;
    lru_cache.Free(image.lru_index);
    const auto& clear_page_table =
        [image_id](u64 page,
                   std::unordered_map<u64, std::vector<ImageId>, Common::IdentityHash<u64>>&
                       selected_page_table) {
            const auto page_it = selected_page_table.find(page);
            if (page_it == selected_page_table.end()) {
                ASSERT_MSG(false, "Unregistering unregistered page=0x{:x}", page << YUZU_PAGEBITS);
                return;
            }
            std::vector<ImageId>& image_ids = page_it->second;
            const auto vector_it = std::ranges::find(image_ids, image_id);
            if (vector_it == image_ids.end()) {
                ASSERT_MSG(false, "Unregistering unregistered image in page=0x{:x}",
                           page << YUZU_PAGEBITS);
                return;
            }
            image_ids.erase(vector_it);
        };
    auto& image_page_table = gpu_page_table_storage[image.gpu_page_table_index];
    ForEachGPUPage(image.gpu_addr, image.guest_size_bytes,
                   [&image_page_table, &clear_page_table](u64 page) {
                       clear_page_table(page, image_page_table);
                   });
    if (False(image.flags & ImageFlagBits::Sparse)) {
        const auto map_id = image.map_view_id;
        ForEachCPUPage(image.cpu_addr, image.guest_size_bytes, [this, map_id](u64 page) {
            const auto page_it = page_table.find(page);
            if (page_it == page_table.end()) {
                ASSERT_MSG(false, "Unregistering unregistered page=0x{:x}", page << YUZU_PAGEBITS);
                return;
            }
            std::vector<ImageMapId>& image_map_ids = page_it->second;
            const auto vector_it = std::ranges::find(image_map_ids, map_id);
            if (vector_it == image_map_ids.end()) {
                ASSERT_MSG(false, "Unregistering unregistered image in page=0x{:x}",
                           page << YUZU_PAGEBITS);
                return;
            }
            image_map_ids.erase(vector_it);
        });
        slot_map_views.erase(map_id);
        return;
    }
    auto& image_sparse_page_table = gpu_page_table_storage[image.gpu_page_table_index + 1];
    ForEachGPUPage(image.gpu_addr, image.guest_size_bytes,
                   [&image_sparse_page_table, &clear_page_table](u64 page) {
                       clear_page_table(page, image_sparse_page_table);
                   });
    auto it = sparse_views.find(image_id);
    ASSERT(it != sparse_views.end());
    auto& sparse_maps = it->second;
    for (auto& map_view_id : sparse_maps) {
        const auto& map_range = slot_map_views[map_view_id];
        const DAddr cpu_addr = map_range.cpu_addr;
        const std::size_t size = map_range.size;
        ForEachCPUPage(cpu_addr, size, [this, image_id](u64 page) {
            const auto page_it = page_table.find(page);
            if (page_it == page_table.end()) {
                ASSERT_MSG(false, "Unregistering unregistered page=0x{:x}", page << YUZU_PAGEBITS);
                return;
            }
            std::vector<ImageMapId>& image_map_ids = page_it->second;
            auto vector_it = image_map_ids.begin();
            while (vector_it != image_map_ids.end()) {
                ImageMapView& map = slot_map_views[*vector_it];
                if (map.image_id != image_id) {
                    vector_it++;
                    continue;
                }
                if (!map.picked) {
                    map.picked = true;
                }
                vector_it = image_map_ids.erase(vector_it);
            }
        });
        slot_map_views.erase(map_view_id);
    }
    sparse_views.erase(it);
}

template <class P>
void TextureCache<P>::NotifyNativeImageModification(ImageBase& image, ImageId image_id) {
#if defined(__APPLE__)
    if (False(image.flags & ImageFlagBits::Sparse)) {
        if (image.cpu_addr < ~(1ULL << 40))
            device_memory.InvalidateNativeReadRegion(image.cpu_addr, image.guest_size_bytes);
        return;
    }
    if (True(image.flags & ImageFlagBits::Registered)) {
        const auto it = sparse_views.find(image_id);
        ASSERT(it != sparse_views.end());
        for (const auto map_view_id : it->second) {
            const auto& map = slot_map_views[map_view_id];
            device_memory.InvalidateNativeReadRegion(map.cpu_addr, map.size);
        }
        return;
    }
    ForEachSparseSegment(image, [this](GPUVAddr, DAddr address, size_t size) {
        device_memory.InvalidateNativeReadRegion(address, size);
    });
#endif
}

template <class P>
void TextureCache<P>::UpdateNativeReadTracking(ImageBase& image, ImageId image_id) {
#if defined(__APPLE__)
    const bool required = True(image.flags & ImageFlagBits::Tracked) &&
                          True(image.flags & ImageFlagBits::GpuModified);
    const bool tracked = True(image.flags & ImageFlagBits::NativeReadTracked);
    if (required && !tracked) NotifyNativeImageModification(image, image_id);
    if (required == tracked) return;
    const s32 delta = required ? 1 : -1;
    if (required) image.flags |= ImageFlagBits::NativeReadTracked;
    else image.flags &= ~ImageFlagBits::NativeReadTracked;
    if (False(image.flags & ImageFlagBits::Sparse)) {
        if (image.cpu_addr < ~(1ULL << 40))
            device_memory.UpdatePagesReadCachedCount(image.cpu_addr, image.guest_size_bytes, delta);
        return;
    }
    if (True(image.flags & ImageFlagBits::Registered)) {
        const auto it = sparse_views.find(image_id);
        ASSERT(it != sparse_views.end());
        for (const auto map_view_id : it->second) {
            const auto& map = slot_map_views[map_view_id];
            device_memory.UpdatePagesReadCachedCount(map.cpu_addr, map.size, delta);
        }
        return;
    }
    ForEachSparseSegment(image, [this, delta](GPUVAddr, DAddr address, size_t size) {
        device_memory.UpdatePagesReadCachedCount(address, size, delta);
    });
#endif
}

template <class P>
void TextureCache<P>::TrackImage(ImageBase& image, ImageId image_id) {
    ASSERT(False(image.flags & ImageFlagBits::Tracked));
    image.flags |= ImageFlagBits::Tracked;
    if (False(image.flags & ImageFlagBits::Sparse)) {
        if (image.cpu_addr < ~(1ULL << 40)) {
            device_memory.UpdatePagesCachedCount(image.cpu_addr, image.guest_size_bytes, 1);
        }
        UpdateNativeReadTracking(image, image_id);
        return;
    }
    if (True(image.flags & ImageFlagBits::Registered)) {
        auto it = sparse_views.find(image_id);
        ASSERT(it != sparse_views.end());
        auto& sparse_maps = it->second;
        for (auto& map_view_id : sparse_maps) {
            const auto& map = slot_map_views[map_view_id];
            const DAddr cpu_addr = map.cpu_addr;
            const std::size_t size = map.size;
            device_memory.UpdatePagesCachedCount(cpu_addr, size, 1);
        }
        UpdateNativeReadTracking(image, image_id);
        return;
    }
    ForEachSparseSegment(image,
                         [this]([[maybe_unused]] GPUVAddr gpu_addr, DAddr cpu_addr, size_t size) {
                             device_memory.UpdatePagesCachedCount(cpu_addr, size, 1);
                         });
    UpdateNativeReadTracking(image, image_id);
}

template <class P>
void TextureCache<P>::UntrackImage(ImageBase& image, ImageId image_id) {
    ASSERT(True(image.flags & ImageFlagBits::Tracked));
    image.flags &= ~ImageFlagBits::Tracked;
    UpdateNativeReadTracking(image, image_id);
    if (False(image.flags & ImageFlagBits::Sparse)) {
        if (image.cpu_addr < ~(1ULL << 40)) {
            device_memory.UpdatePagesCachedCount(image.cpu_addr, image.guest_size_bytes, -1);
        }
        return;
    }
    ASSERT(True(image.flags & ImageFlagBits::Registered));
    auto it = sparse_views.find(image_id);
    ASSERT(it != sparse_views.end());
    auto& sparse_maps = it->second;
    for (auto& map_view_id : sparse_maps) {
        const auto& map = slot_map_views[map_view_id];
        const DAddr cpu_addr = map.cpu_addr;
        const std::size_t size = map.size;
        device_memory.UpdatePagesCachedCount(cpu_addr, size, -1);
    }
}

template <class P>
void TextureCache<P>::DeleteImage(ImageId image_id, bool immediate_delete,
                                  std::vector<ImageViewId>* deferred_views) {
    CancelGCReadback(image_id);
    CancelPendingDownloads(image_id);
    ImageBase& image = slot_images[image_id];
    if (image.HasScaled()) {
        total_used_memory -= GetScaledImageSizeBytes(image);
    }
    u64 tentative_size = std::max(image.guest_size_bytes, image.unswizzled_size_bytes);
    if ((IsPixelFormatASTC(image.info.format) &&
         True(image.flags & ImageFlagBits::AcceleratedUpload)) ||
        True(image.flags & ImageFlagBits::Converted)) {
        tentative_size = TranscodedAstcSize(tentative_size, image.info.format);
    }
    total_used_memory -= Common::AlignUp(tentative_size, 1024);
    const GPUVAddr gpu_addr = image.gpu_addr;
    const auto alloc_it = image_allocs_table.find(gpu_addr);
    if (alloc_it == image_allocs_table.end()) {
        ASSERT_MSG(false, "Trying to delete an image alloc that does not exist in address 0x{:x}",
                   gpu_addr);
        return;
    }
    const ImageAllocId alloc_id = alloc_it->second;
    std::vector<ImageId>& alloc_images = slot_image_allocs[alloc_id].images;
    const auto alloc_image_it = std::ranges::find(alloc_images, image_id);
    if (alloc_image_it == alloc_images.end()) {
        ASSERT_MSG(false, "Trying to delete an image that does not exist");
        return;
    }
    ASSERT_MSG(False(image.flags & ImageFlagBits::Tracked), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

    // Mark render targets as dirty
    auto& dirty = maxwell3d->dirty.flags;
    dirty[Dirty::RenderTargets] = true;
    dirty[Dirty::ZetaBuffer] = true;
    for (size_t rt = 0; rt < NUM_RT; ++rt) {
        dirty[Dirty::ColorBuffer0 + rt] = true;
    }
    const std::span<const ImageViewId> image_view_ids = image.image_view_ids;
    for (const ImageViewId image_view_id : image_view_ids) {
        std::ranges::replace(render_targets.color_buffer_ids, image_view_id, ImageViewId{});
        if (render_targets.depth_buffer_id == image_view_id) {
            render_targets.depth_buffer_id = ImageViewId{};
        }
    }
    if (deferred_views) {
        deferred_views->insert(deferred_views->end(), image_view_ids.begin(), image_view_ids.end());
    } else {
        RemoveImageViewReferences(image_view_ids);
        RemoveFramebuffers(image_view_ids);
    }

    for (const AliasedImage& alias : image.aliased_images) {
        ImageBase& other_image = slot_images[alias.id];
        [[maybe_unused]] const size_t num_removed_aliases =
            std::erase_if(other_image.aliased_images, [image_id](const AliasedImage& other_alias) {
                return other_alias.id == image_id;
            });
        other_image.CheckAliasState();
        ASSERT_MSG(num_removed_aliases == 1, "Invalid number of removed aliases: {}",
                   num_removed_aliases);
    }
    for (const ImageId overlap_id : image.overlapping_images) {
        ImageBase& other_image = slot_images[overlap_id];
        [[maybe_unused]] const size_t num_removed_overlaps = std::erase_if(
            other_image.overlapping_images,
            [image_id](const ImageId other_overlap_id) { return other_overlap_id == image_id; });
        other_image.CheckBadOverlapState();
        ASSERT_MSG(num_removed_overlaps == 1, "Invalid number of removed overlapps: {}",
                   num_removed_overlaps);
    }
    for (const ImageViewId image_view_id : image_view_ids) {
        if (!immediate_delete) {
            sentenced_image_view.Push(std::move(slot_image_views[image_view_id]));
        }
        slot_image_views.erase(image_view_id);
    }
    if (!immediate_delete) {
        sentenced_images.Push(std::move(slot_images[image_id]));
    }
    slot_images.erase(image_id);

    alloc_images.erase(alloc_image_it);
    if (alloc_images.empty()) {
        image_allocs_table.erase(alloc_it);
        slot_image_allocs.erase(alloc_id);
    }
    for (size_t c : active_channel_ids) {
        auto& channel_info = channel_storage[c];
        if constexpr (ENABLE_VALIDATION) {
            std::ranges::fill(channel_info.graphics_image_view_ids, CORRUPT_ID);
            std::ranges::fill(channel_info.compute_image_view_ids, CORRUPT_ID);
        }
        channel_info.graphics_image_table.Invalidate();
        channel_info.compute_image_table.Invalidate();
    }
    has_deleted_images = true;
}

template <class P>
void TextureCache<P>::RemoveImageViewReferences(std::span<const ImageViewId> removed_views) {
    if (removed_views.empty()) {
        return;
    }
    const ImageViewInvalidation invalidation{removed_views};
    for (size_t c : active_channel_ids) {
        auto& channel_info = channel_storage[c];
        auto it = channel_info.image_views.begin();
        while (it != channel_info.image_views.end()) {
            if (invalidation.Contains(it->second)) {
                it = channel_info.image_views.erase(it);
            } else {
                ++it;
            }
        }
    }
}

template <class P>
void TextureCache<P>::RemoveFramebuffers(std::span<const ImageViewId> removed_views) {
    if (removed_views.empty()) {
        return;
    }
    const ImageViewInvalidation invalidation{removed_views};
    auto it = framebuffers.begin();
    while (it != framebuffers.end()) {
        const auto contains = [&](ImageViewId id) { return invalidation.Contains(id); };
        if (contains(it->first.depth_buffer_id) ||
            std::ranges::any_of(it->first.color_buffer_ids, contains)) {
            auto framebuffer_id = it->second;
            ASSERT(framebuffer_id);
            sentenced_framebuffers.Push(std::move(slot_framebuffers[framebuffer_id]));
            slot_framebuffers.erase(framebuffer_id);
            it = framebuffers.erase(it);
        } else {
            ++it;
        }
    }
}

template <class P>
void TextureCache<P>::MarkModification(ImageBase& image, ImageId image_id) noexcept {
    image.flags |= ImageFlagBits::GpuModified;
    image.modification_tick = ++modification_tick;
    NotifyNativeImageModification(image, image_id);
    UpdateNativeReadTracking(image, image_id);
}

template <class P>
void TextureCache<P>::SynchronizeAliases(ImageId image_id) {
    boost::container::small_vector<const AliasedImage*, 8> aliased_images;
    Image& image = slot_images[image_id];
    bool any_rescaled = True(image.flags & ImageFlagBits::Rescaled);
    bool any_modified = True(image.flags & ImageFlagBits::GpuModified);
    u64 most_recent_tick = image.modification_tick;
    for (const AliasedImage& aliased : image.aliased_images) {
        ImageBase& aliased_image = slot_images[aliased.id];
        if (image.modification_tick < aliased_image.modification_tick) {
            most_recent_tick = std::max(most_recent_tick, aliased_image.modification_tick);
            aliased_images.push_back(&aliased);
            any_rescaled |= True(aliased_image.flags & ImageFlagBits::Rescaled);
            any_modified |= True(aliased_image.flags & ImageFlagBits::GpuModified);
        }
    }
    if (aliased_images.empty()) {
        return;
    }
    const bool can_rescale = ImageCanRescale(image);
    if (any_rescaled) {
        if (can_rescale) {
            ScaleUp(image);
        } else {
            ScaleDown(image);
        }
    }
    image.modification_tick = most_recent_tick;
    NotifyNativeImageModification(image, image_id);
    if (any_modified) {
        image.flags |= ImageFlagBits::GpuModified;
        UpdateNativeReadTracking(image, image_id);
    }
    std::ranges::sort(aliased_images, [this](const AliasedImage* lhs, const AliasedImage* rhs) {
        const ImageBase& lhs_image = slot_images[lhs->id];
        const ImageBase& rhs_image = slot_images[rhs->id];
        return lhs_image.modification_tick < rhs_image.modification_tick;
    });
    const auto& resolution = videoSettings.resolution_info;
    for (const AliasedImage* const aliased : aliased_images) {
        if (!resolution.active || !any_rescaled) {
            CopyImage(image_id, aliased->id, aliased->copies);
            continue;
        }
        Image& aliased_image = slot_images[aliased->id];
        if (!can_rescale) {
            ScaleDown(aliased_image);
            CopyImage(image_id, aliased->id, aliased->copies);
            continue;
        }
        ScaleUp(aliased_image);
        CopyImage(image_id, aliased->id, aliased->copies);
    }
}

template <class P>
void TextureCache<P>::PrepareImage(ImageId image_id, bool is_modification, bool invalidate) {
    Image& image = slot_images[image_id];
    if (invalidate) {
        image.flags &= ~ImageFlagBits::CpuModified;
        // A clear immediately writes the image again. Preserve its GPU-dirty epoch instead
        // of unprotecting and reprotecting the same pages before MarkModification below.
        if (!is_modification) {
            image.flags &= ~ImageFlagBits::GpuModified;
            UpdateNativeReadTracking(image, image_id);
        }
        if (False(image.flags & ImageFlagBits::Tracked)) {
            TrackImage(image, image_id);
        }
    } else {
        RefreshContents(image, image_id);
        SynchronizeAliases(image_id);
    }
    if (is_modification) {
        MarkModification(image, image_id);
    }
    lru_cache.Touch(image.lru_index, frame_tick);
}

template <class P>
void TextureCache<P>::PrepareImageView(ImageViewId image_view_id, bool is_modification,
                                       bool invalidate) {
    if (!image_view_id) {
        return;
    }
    const ImageViewBase& image_view = slot_image_views[image_view_id];
    if (image_view.IsBuffer()) {
        return;
    }
    PrepareImage(image_view.image_id, is_modification, invalidate);
}

template <class P>
void TextureCache<P>::CopyImage(ImageId dst_id, ImageId src_id, std::vector<ImageCopy> copies) {
    Image& dst = slot_images[dst_id];
    Image& src = slot_images[src_id];
    const bool is_rescaled = True(src.flags & ImageFlagBits::Rescaled);
    if (is_rescaled) {
        ASSERT(True(dst.flags & ImageFlagBits::Rescaled));
        const bool both_2d{src.info.type == ImageType::e2D && dst.info.type == ImageType::e2D};
        const auto& resolution = videoSettings.resolution_info;
        for (auto& copy : copies) {
            copy.src_offset.x = resolution.ScaleUp(copy.src_offset.x);
            copy.dst_offset.x = resolution.ScaleUp(copy.dst_offset.x);
            copy.extent.width = resolution.ScaleUp(copy.extent.width);
            if (both_2d) {
                copy.src_offset.y = resolution.ScaleUp(copy.src_offset.y);
                copy.dst_offset.y = resolution.ScaleUp(copy.dst_offset.y);
                copy.extent.height = resolution.ScaleUp(copy.extent.height);
            }
        }
    }
    const auto dst_format_type = GetFormatType(dst.info.format);
    const auto src_format_type = GetFormatType(src.info.format);
    if (src_format_type == dst_format_type) {
        if constexpr (HAS_EMULATED_COPIES) {
            if (!runtime.CanImageBeCopied(dst, src)) {
                return runtime.EmulateCopyImage(dst, src, copies);
            }
        }
        return runtime.CopyImage(dst, src, copies);
    }
    UNIMPLEMENTED_IF(dst.info.type != ImageType::e2D);
    UNIMPLEMENTED_IF(src.info.type != ImageType::e2D);
    if (runtime.ShouldReinterpret(dst, src)) {
        return runtime.ReinterpretImage(dst, src, copies);
    }
    for (const ImageCopy& copy : copies) {
        UNIMPLEMENTED_IF(copy.dst_subresource.num_layers != 1);
        UNIMPLEMENTED_IF(copy.src_subresource.num_layers != 1);
        UNIMPLEMENTED_IF(copy.src_offset != Offset3D{});
        UNIMPLEMENTED_IF(copy.dst_offset != Offset3D{});

        const SubresourceBase dst_base{
            .level = copy.dst_subresource.base_level,
            .layer = copy.dst_subresource.base_layer,
        };
        const SubresourceBase src_base{
            .level = copy.src_subresource.base_level,
            .layer = copy.src_subresource.base_layer,
        };
        const SubresourceExtent dst_extent{.levels = 1, .layers = 1};
        const SubresourceExtent src_extent{.levels = 1, .layers = 1};
        const SubresourceRange dst_range{.base = dst_base, .extent = dst_extent};
        const SubresourceRange src_range{.base = src_base, .extent = src_extent};
        PixelFormat dst_format = dst.info.format;
        if (GetFormatType(src.info.format) == SurfaceType::DepthStencil &&
            GetFormatType(dst_format) == SurfaceType::ColorTexture &&
            BytesPerBlock(dst_format) == 4) {
            dst_format = PixelFormat::A8B8G8R8_UNORM;
        }
        const ImageViewInfo dst_view_info(ImageViewType::e2D, dst_format, dst_range);
        const ImageViewInfo src_view_info(ImageViewType::e2D, src.info.format, src_range);
        const auto [dst_framebuffer_id, dst_view_id] = RenderTargetFromImage(dst_id, dst_view_info);
        Framebuffer* const dst_framebuffer = &slot_framebuffers[dst_framebuffer_id];
        const ImageViewId src_view_id = FindOrEmplaceImageView(src_id, src_view_info);
        ImageView& dst_view = slot_image_views[dst_view_id];
        ImageView& src_view = slot_image_views[src_view_id];
        [[maybe_unused]] const Extent3D expected_size{
            .width = std::min(dst_view.size.width, src_view.size.width),
            .height = std::min(dst_view.size.height, src_view.size.height),
            .depth = std::min(dst_view.size.depth, src_view.size.depth),
        };
        const Extent3D scaled_extent = [is_rescaled, expected_size]() {
            if (!is_rescaled) {
                return expected_size;
            }
            const auto& resolution = videoSettings.resolution_info;
            return Extent3D{
                .width = resolution.ScaleUp(expected_size.width),
                .height = resolution.ScaleUp(expected_size.height),
                .depth = expected_size.depth,
            };
        }();
        UNIMPLEMENTED_IF(copy.extent != scaled_extent);

        runtime.ConvertImage(dst_framebuffer, dst_view, src_view);
    }
}

template <class P>
void TextureCache<P>::BindRenderTarget(ImageViewId* old_id, ImageViewId new_id) {
    if (*old_id == new_id) {
        return;
    }
    if (new_id) {
        const ImageViewBase& old_view = slot_image_views[new_id];
        if (True(old_view.flags & ImageViewFlagBits::PreemtiveDownload)) {
            const PendingDownload new_download{true, 0, old_view.image_id};
            uncommitted_downloads.emplace_back(new_download);
        }
    }
    *old_id = new_id;
}

template <class P>
std::pair<FramebufferId, ImageViewId> TextureCache<P>::RenderTargetFromImage(
    ImageId image_id, const ImageViewInfo& view_info) {
    const ImageViewId view_id = FindOrEmplaceImageView(image_id, view_info);
    const ImageBase& image = slot_images[image_id];
    const bool is_rescaled = True(image.flags & ImageFlagBits::Rescaled);
    const bool is_color = GetFormatType(image.info.format) == SurfaceType::ColorTexture;
    const ImageViewId color_view_id = is_color ? view_id : ImageViewId{};
    const ImageViewId depth_view_id = is_color ? ImageViewId{} : view_id;
    Extent3D extent = MipSize(image.info.size, view_info.range.base.level);
    if (is_rescaled) {
        const auto& resolution = videoSettings.resolution_info;
        extent.width = resolution.ScaleUp(extent.width);
        if (image.info.type == ImageType::e2D) {
            extent.height = resolution.ScaleUp(extent.height);
        }
    }
    const u32 num_samples = image.info.num_samples;
    const auto [samples_x, samples_y] = SamplesLog2(num_samples);
    const FramebufferId framebuffer_id = GetFramebufferId(RenderTargets{
        .color_buffer_ids = {color_view_id},
        .depth_buffer_id = depth_view_id,
        .size = {extent.width >> samples_x, extent.height >> samples_y},
        .is_rescaled = is_rescaled,
    });
    return {framebuffer_id, view_id};
}

template <class P>
bool TextureCache<P>::IsFullClear(ImageViewId id) {
    if (!id) {
        return true;
    }
    const ImageViewBase& image_view = slot_image_views[id];
    const ImageBase& image = slot_images[image_view.image_id];
    const Extent3D size = image_view.size;
    const auto& regs = maxwell3d->regs;
    const auto& scissor = regs.scissor_test[0];
    if (image.info.resources.levels > 1 || image.info.resources.layers > 1) {
        // Images with multiple resources can't be cleared in a single call
        return false;
    }
    if (regs.clear_control.use_scissor == 0) {
        // If scissor testing is disabled, the clear is always full
        return true;
    }
    // Make sure the clear covers all texels in the subresource
    return scissor.min_x == 0 && scissor.min_y == 0 && scissor.max_x >= size.width &&
           scissor.max_y >= size.height;
}

template <class P>
void TextureCache<P>::CreateChannel(struct Tegra::Control::ChannelState& channel) {
    VideoCommon::ChannelSetupCaches<TextureCacheChannelInfo>::CreateChannel(channel);
    const auto it = channel_map.find(channel.bind_id);
    auto* this_state = &channel_storage[it->second];
    const auto& this_as_ref = address_spaces[channel.memory_manager->GetID()];
    this_state->gpu_page_table_index = this_as_ref.storage_id * 2;
    this_state->gpu_page_table = &gpu_page_table_storage[this_as_ref.storage_id * 2];
    this_state->sparse_page_table = &gpu_page_table_storage[this_as_ref.storage_id * 2 + 1];
}

/// Bind a channel for execution.
template <class P>
void TextureCache<P>::OnGPUASRegister([[maybe_unused]] size_t map_id) {
    gpu_page_table_storage.emplace_back();
    gpu_page_table_storage.emplace_back();
}

} // namespace VideoCommon
