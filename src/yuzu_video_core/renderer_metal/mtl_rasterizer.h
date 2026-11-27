// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "yuzu_common/common_types.h"
#include "yuzu_video_core/control/channel_state_cache.h"
#include "yuzu_video_core/engines/maxwell_dma.h"
#include "yuzu_video_core/gpu_write_history.h"
#include "yuzu_video_core/metal_common/metal_memory_allocator.h"
#include "yuzu_video_core/rasterizer_interface.h"
#include "yuzu_video_core/renderer_metal/mtl_argument_buffer.h"
#include "yuzu_video_core/renderer_metal/mtl_transform_feedback.h"
#include "yuzu_video_core/renderer_metal/mtl_blit_image.h"
#include "yuzu_video_core/renderer_metal/mtl_buffer_cache.h"
#include "yuzu_video_core/renderer_metal/mtl_fence_manager.h"
#include "yuzu_video_core/renderer_metal/mtl_pipeline_cache.h"
#include "yuzu_video_core/renderer_metal/mtl_render_pass_state.h"
#include "yuzu_video_core/renderer_metal/mtl_staging_buffer_pool.h"
#include "yuzu_video_core/renderer_metal/mtl_texture_cache.h"
#include "yuzu_video_core/renderer_vulkan/descriptor_lookup.h"

namespace Core {
class System;
}

namespace Metal {
class Device;
class Scheduler;

class AccelerateDMA final : public Tegra::Engines::AccelerateDMAInterface {
public:
    explicit AccelerateDMA(BufferCache& buffer_cache, TextureCache& texture_cache,
                           Scheduler& scheduler);

    bool BufferCopy(GPUVAddr start_address, GPUVAddr end_address, u64 amount) override;
    bool BufferClear(GPUVAddr src_address, u64 amount, u32 value) override;
    bool ImageToBuffer(const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::ImageOperand& src,
                       const Tegra::DMA::BufferOperand& dst) override;
    bool BufferToImage(const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::BufferOperand& src,
                       const Tegra::DMA::ImageOperand& dst) override;

private:
    /// Don't just return false here. The engine's fallback goes through guest
    /// memory, and a render target the GPU never wrote back reads as nothing.
    template <bool IS_IMAGE_UPLOAD>
    bool DmaBufferImageCopy(const Tegra::DMA::ImageCopy& copy_info,
                            const Tegra::DMA::BufferOperand& buffer_operand,
                            const Tegra::DMA::ImageOperand& image_operand);

    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    Scheduler& scheduler;
};

/// Metal rasterizer. A draw whose pipeline fails to build is skipped rather
/// than issued with wrong state.
class RasterizerMetal final : public VideoCore::RasterizerInterface,
                              protected VideoCommon::ChannelSetupCaches<VideoCommon::ChannelInfo> {
public:
    explicit RasterizerMetal(Tegra::GPU& gpu,
                             Tegra::MaxwellDeviceMemoryManager& device_memory,
                             const Device& device, MemoryAllocator& memory_allocator,
                             StagingBufferPool& staging_pool, Scheduler& scheduler);
    ~RasterizerMetal() override;

    void Draw(bool is_indexed, u32 instance_count) override;
    bool DrawIndirect() override;
    void DrawTexture() override;
    void Clear(u32 layer_count) override;
    void DispatchCompute() override;
    void ResetCounter(VideoCommon::QueryType type) override;
    void Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
               VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) override;
    void BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr, u32 size) override;
    void DisableGraphicsUniformBuffer(size_t stage, u32 index) override;
    bool PrefetchUniform(size_t stage, u32 index, DAddr device_addr, u32 max_size,
                         VideoCommon::UniformPrefetch& prefetch) override;
    u64 UniformPrefetchPosition() override;
    void ApplyUniformPrefetch(size_t stage, u32 index,
                              const VideoCommon::UniformPrefetch& prefetch) override;
    void ResetUniformPrefetch(u64 position) override;
    void FlushAll() override;
    void FlushNativeRegion(DAddr addr, u64 size) override;
    bool MayFlushSharedPage(DAddr addr, u64 size) override;
    bool MayHaveComputeWrites(DAddr addr, u64 size) override;
    u64 ComputeWriteEpoch() override;
    VideoCore::SharedPageAccess ObserveSharedPage(DAddr addr, u64 size) override;
    bool AwaitsProtectedDownload(DAddr addr, u64 size) override;
    bool AwaitsProtectedBufferDownload(DAddr addr, u64 size) override;
    void WaitProtectedBufferDownloads(DAddr addr, u64 size) override;
    bool HasPendingResult(DAddr addr, u64 size) override;
    bool CommitReadbacksForEarlySignal() override;
    void FlushRegion(DAddr addr, u64 size,
                     VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    bool MustFlushRegion(DAddr addr, u64 size,
                         VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    void InvalidateRegion(DAddr addr, u64 size,
                          VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    void InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) override;
    void OnCacheInvalidation(PAddr addr, u64 size) override;
    u64 OnCacheInvalidationAhead(PAddr addr, u64 size) override;
    void OnCoarseCacheInvalidation(PAddr addr, u64 size) override;
    bool OnCPUWrite(PAddr addr, u64 size) override;
    bool IsReadCachedOnlyForQueryResults(DAddr addr, u64 size) const override;
    RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override;
    void InvalidateGPUCache() override;
    void UnmapMemory(DAddr addr, u64 size) override;
    void ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size,
                         bool preserve_contents = false) override;
    void SignalFence(std::function<void()>&& func) override;
    void SignalMemoryOperationFence(std::function<void()>&& func) override;
    void WaitForFence() override;
    void SyncOperation(std::function<void()>&& func) override;
    void SignalSyncPoint(u32 value) override;
    void SignalReference() override;
    void ReleaseFences(bool force) override;
    void FlushAndInvalidateRegion(
        DAddr addr, u64 size, VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    void WaitForIdle() override;
    void FragmentBarrier() override;
    void TiledCacheBarrier() override;
    void FlushCommands() override;
    void TickFrame() override;

    bool AccelerateConditionalRendering() override;
    bool HasDrawTransformFeedback() override;
    bool AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                               const Tegra::Engines::Fermi2D::Surface& dst,
                               const Tegra::Engines::Fermi2D::Config& copy_config) override;
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override;
    void AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                  std::span<const u8> memory) override;
    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback,
                           std::string_view content_identity = {},
                           std::string_view game_path = {}, std::string_view game_context = {},
                           VideoCore::ShaderImportRequest* import = nullptr) override;
    void InitializeChannel(Tegra::Control::ChannelState& channel) override;
    void BindChannel(Tegra::Control::ChannelState& channel) override;
    void ReleaseChannel(s32 channel_id) override;

    /// Hands recorded work to the worker and commits mid-frame. Without this a
    /// guest thread waiting on a fence waits out the whole frame's command list.
    void FlushWork();

    /// Null when @p config has not been rendered yet. Optionally reports the
    /// view's UNSCALED guest extent, what a guest-space crop is normalised to.
    [[nodiscard]] MTL::Texture* GetFramebufferTexture(const Tegra::FramebufferConfig& config,
                                                      u32* unscaled_width = nullptr,
                                                      u32* unscaled_height = nullptr);

    struct FrameDraws {
        u32 total{};
        u32 skipped{};
        bool new_render_targets{};
    };
    [[nodiscard]] FrameDraws TakeFrameDraws();

private:
    /// Occlusion counts. Metal has no query pool: a pass names one visibility buffer and each
    /// draw accumulates into the 8-byte slot its encoder's visibility mode picks, overwritten
    /// (not accumulated) across passes. Every counting range (a pass between two resets or
    /// queries) gets its own slot from a ring, and a query sums its ranges once the GPU is done.
    /// Once a title has counted, every pass names the buffer, so toggling counting no longer
    /// ends the pass.
    void EnsureVisibilityBuffer();
    void BeginPassVisibility(RenderPassState& pass);
    /// The slot this draw counts into, a new one when the pass or the counter range changed.
    [[nodiscard]] u32 CountingSlot();
    /// Publishes the queries whose submissions completed; with wait, all of them.
    void ResolveOcclusionQueries(bool wait);
    /// Publishes the pending answers that overlap [gpu_addr, gpu_addr + size), waiting for them.
    void ResolveOcclusionQueriesAt(GPUVAddr gpu_addr, u64 size);
    /// Invalidates pending occlusion queries overlapping a CPU write / invalidation / unmap.
    void InvalidatePendingOcclusion(DAddr addr, u64 size);

    struct PendingOcclusion {
        u64 tick;
        u64 first_slot;
        u64 end_slot;
        GPUVAddr gpu_addr;
        Tegra::MemoryManager* memory;
        bool has_timeout;
        DAddr device_addr = 0;
        /// Its pages are kept unreadable by the guest CPU until the result is written.
        bool protected_result = false;
        /// True when guest CPU/DMA/newer query overwrote this address before retirement.
        bool invalidated = false;
    };
    void ProtectResult(DAddr addr, u64 size, s32 delta);
    /// Latest submission tick of a pending occlusion result overlapping the range, or 0.
    u64 PendingOcclusionTickAt(DAddr addr, u64 size);
    /// Pages holding pending results, lock-free for the CPU fault path, with their counts.
    VideoCommon::AtomicPageBits<34, 12> m_protected_result_pages;
    std::unordered_map<u64, u32> m_protected_result_counts;
    std::mutex m_protected_result_mutex;
    static constexpr u32 VISIBILITY_SLOTS = 16384;
    static constexpr u32 NO_VISIBILITY_SLOT = ~0u;
    Owned<MTL::Buffer> m_visibility_buffer;
    std::vector<u64> m_visibility_slot_tick;
    std::deque<PendingOcclusion> m_pending_occlusion;
    std::atomic<u32> m_pending_occlusion_count{0};
    std::atomic<u64> m_last_occlusion_tick{0};
    std::atomic<bool> m_occlusion_needs_fence{false};
    std::mutex m_occlusion_mutex;
    std::mutex m_occlusion_resolve_mutex;
    u64 m_visibility_next{};      ///< Monotonic, the ring slot is this modulo VISIBILITY_SLOTS.
    u64 m_visibility_first{};     ///< First slot of the current counter range.
    u64 m_visibility_current{~0ull}; ///< Slot draws count into now, ~0 when none.
    u64 m_visibility_current_pass{};
    u32 m_visibility_pass_slot{NO_VISIBILITY_SLOT};
    bool m_occlusion_used{};
    bool m_zpass_enabled{};

    /// Active GPU-evaluated conditional rendering predicate (avoids synchronous
    /// m_scheduler.Wait() on ProcessQueryCondition while occlusion results are
    /// still in flight on the GPU).
    struct GpuPredicate {
        bool active{false};
        bool cpu_resolved{false};
        bool cpu_result{true};
        const void* engine{};
        Tegra::Engines::Maxwell3D::Regs::RenderEnable::Mode mode{
            Tegra::Engines::Maxwell3D::Regs::RenderEnable::Mode::True};
        GPUVAddr condition_addr{};
        bool from_buffer{false};
        DAddr predicate_cpu_addr{};
        u32 predicate_bytes{};
        bool a_from_gpu{false};
        bool a_is_64bit{true};
        u64 a_first_slot{};
        u64 a_end_slot{};
        u64 a_cpu_value{};
        bool b_from_gpu{false};
        bool b_is_64bit{true};
        u64 b_first_slot{};
        u64 b_end_slot{};
        u64 b_cpu_value{};
        u64 wait_tick{};
    };
    GpuPredicate m_gpu_predicate{};
    [[nodiscard]] bool EvaluateGpuPredicateOnCpu();
    [[nodiscard]] bool ResolveBufferPredicate(MTL::Buffer*& out_buffer, u32& out_offset);

    /// Distinct from m_draw_counter: resetting that one here would make
    /// FlushCommands drop a buffer that does have work in it.
    u32 m_flush_work_counter{};
    u32 m_dispatch_checks{};
    /// Work units per submission, halved whenever the GPU watchdog kills one.
    u32 m_work_to_commit{512};
    u32 m_seen_timeouts{};
    /// Wall-clock time and GPU tick of the last watchdog kill or bound growth.
    /// Growing back requires both real elapsed time without a kill and completed
    /// GPU submissions, so draw-heavy titles settle instead of bouncing back in
    /// milliseconds.
    std::chrono::steady_clock::time_point m_last_watchdog_event{};
    u64 m_watchdog_gpu_tick{};
    static constexpr auto WATCHDOG_GROW_COOLDOWN = std::chrono::seconds{10};
    static constexpr u64 WATCHDOG_GROW_MIN_GPU_TICKS = 64;
    /// Encoded GPU work beyond the draws themselves (capture kernels, index
    /// expansions, gathers), so the cadence follows the command buffer.
    u32 m_recorded_gpu_work{};
    /// Submission the FlushWork counters belong to.
    u64 m_flush_work_tick{};

    /// Clearing more than the guest asked for destroys contents it expected to keep.
    struct ClearAspects {
        bool color{};
        bool depth{};
        bool stencil{};
        u32 color_target{};
        /// A clear is a load action, which would cover every layer of the pass.
        u32 layer{};
        u32 layer_count{1};
        /// A load action has no channel mask, so a partial clear must be drawn.
        u32 color_mask{0xf};
        /// The guest's stencil write mask, for the same reason.
        u32 stencil_mask{0xff};
        /// Non-null when the clear is predicated on the GPU via conditional indirect args.
        MTL::Buffer* indirect_buffer{};
        u32 indirect_offset{};
    };

    struct IndirectDraw {
        MTL::Buffer* buffer{};
        u32 offset{};
        u32 stride{};
        /// Entry count resolved on the GPU. See IndexPass::PatchIndirectCount.
        bool include_count{};
        /// The "indirect buffer" is a transform-feedback BYTE count, not draw
        /// arguments. See IndexPass::MakeByteCountDrawArgs.
        bool is_byte_count{};
        u32 count{};
        u32 instance_mask{0xFFFFFFFFu};
    };

    /// @p indirect is null for an ordinary draw.
    void DrawImpl(bool is_indexed, u32 instance_count, const IndirectDraw* indirect);

    /// False when there is nothing to draw into.
    bool UpdateRenderTargets(bool is_clear, const ClearAspects& aspects);
    void FlushRegionImpl(DAddr addr, u64 size, VideoCommon::CacheType which, bool native_read);
    /// Points aliased colour slots that `written` leaves alone at throwaway
    /// memoryless targets, so only the slot that writes stores to the image.
    void ScratchUnwrittenAliases(RenderPassState& pass, u32 written);

    void UpdatePipelineState();

    /// What UpdatePipelineState reads besides the registers its dirty flag covers.
    struct PipelineStateInputs {
        const void* engine{};
        const void* framebuffer{};
        u64 rt_dirty_generation{};
        u32 topology{};
        std::array<u32, 32> stream_words{};
        std::array<u32, 8> rt_formats{};

        [[nodiscard]] bool operator==(const PipelineStateInputs&) const noexcept = default;
    };

    struct StageResources {
        StagingBufferRef argument_buffer{};
        StagingBufferRef table_argument_buffer{};
        /// The draw must be dropped: the encoder still holds the previous
        /// draw's buffer, so rendering anyway samples another object's data.
        bool resources_failed{};
        /// Fragment stage: the shader reads color attachments through framebuffer
        /// fetch and wants its push constant block, with this selector, bound.
        bool reads_attachments{};
        u32 attachment_reads{~0U};
        /// Resolution scaling push constants (matches RescalingLayout word layout:
        /// 4 texture mask words + 2 image mask words).
        bool uses_rescaling{};
        std::array<u32, 6> rescaling_masks{};
        /// Vertex stage: true when at least one bound texture/image was written by
        /// the GPU (so a StageFragment -> StageVertex barrier is needed).
        bool reads_gpu_texture{};
    };

    /// Writes one argument buffer PER STAGE - never one shared: SPIRV-Cross
    /// numbers `[[id(N)]]` from zero independently for each stage.
    /// Picks the color attachment each pixel local read of the fragment shader takes
    /// through framebuffer fetch, and nulls the view it would have sampled instead.
    template <typename Reads>
    u32 SelectAttachmentReads(const Shader::Info& fragment, const Reads& reads,
                              std::span<VideoCommon::ImageViewInOut> views, u32 scratch);

    /// The colour attachments a draw with `pipeline` may modify.
    [[nodiscard]] u32 WrittenColorAttachments(const GraphicsPipeline& pipeline) const;

    /// The aliased colour slots a draw writing `written` renders into scratch.
    [[nodiscard]] u32 AliasScratch(u32 num_color, u32 written) const;

    struct VtgStageBindings {
        bool is_indexed{};
        bool is_indirect{};
        u32 indirect_instance_mask{0xFFFFFFFFu};
        u32 vertex_count{};
        u32 instance_count{};
        u32 base_vertex{};
        u32 first_index{};
        u32 base_instance{};
        u32 index_bytes{};

        bool runs{};
        u32 vertex_invocations{};
        u32 geometry_invocations{};
        u32 output_vertices{};
        u32 output_indices{};
        u32 total_primitives{};
        u32 indirect_capacity{};
        StagingBufferRef info_buffer;
        StagingBufferRef vertex_out;
        StagingBufferRef geometry_out;
        StagingBufferRef index_out;
        MTL::Buffer* indirect_args_buffer{};
        u32 indirect_args_offset{};
        u64 index_size{};
        bool emits_points{};
        StagingBufferRef passthrough_arg_buffer;
    };

    struct VtgDispatch {
        StagingBufferRef vertex_arg_buffer;
        MTL::Buffer* index_buffer{};
        u64 index_gpu_address{};
        u32 index_offset{};
        u32 index_count{};
        MTL::Buffer* indirect_draw_buffer{};
        u32 indirect_draw_offset{};
        MTL::PrimitiveType primitive{MTL::PrimitiveTypeTriangleStrip};
    };

    void ConfigureGraphicsResources(const GraphicsPipeline& pipeline, bool is_indexed,
                                    std::array<StageResources, NUM_PIPELINE_STAGES>& out,
                                    bool descriptors_synced = false,
                                    VtgStageBindings* vtg_bindings = nullptr);

    [[nodiscard]] bool RecordVtgDispatch(
        GraphicsPipeline& pipeline, const VtgEmulation& vtg,
        const VtgStageBindings& bindings,
        const std::array<StageResources, NUM_PIPELINE_STAGES>& stages,
        VtgDispatch& out);

    struct RuntimeImageWriteState {
        StagingBufferRef mask{};
        u32 mask_words{};
        bool has_candidates{};
        std::vector<VideoCommon::ImageViewId> views;
    };

    void ConfigureComputeResources(
        const ComputePipeline& pipeline, StagingBufferRef& out_argument_buffer,
        StagingBufferRef& out_table_argument_buffer, std::array<u32, 6>& out_rescaling_masks,
        std::array<u32, BufferCache::MAX_WRITE_TRACK_SLOTS * 2>& out_write_tracking,
        u32& out_write_tracking_bytes, RuntimeImageWriteState& out_runtime_image_writes);

    Tegra::GPU& m_gpu;
    Tegra::MaxwellDeviceMemoryManager& m_device_memory;
    const Device& m_device;
    MemoryAllocator& m_memory_allocator;
    Scheduler& m_scheduler;
    StagingBufferPool& m_staging_pool;

    /// Declared before the caches, which hold a reference to it.
    BlitImageHelper m_blit_image;

    TextureCacheRuntime m_texture_cache_runtime;
    TextureCache m_texture_cache;

    BufferCacheRuntime m_buffer_cache_runtime;
    BufferCache m_buffer_cache;

    PipelineCache m_pipeline_cache;
    /// Gathers the captured varyings into the guest's feedback buffers. The
    /// draw still renders after.
    bool RecordXfbCapture(GraphicsPipeline& pipeline, const GeometryEmulation& xfb,
                          Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology,
                          bool is_indexed, u32 count, u32 first_index, u32 base_vertex,
                          u32 num_instances,
                          const std::array<StageResources, NUM_PIPELINE_STAGES>& stages,
                          std::span<const BufferBinding> vertex_bindings,
                          u32 enabled_vertex_slots);

    ArgumentBufferBuilder m_argument_buffer;
    TransformFeedback m_transform_feedback;

    /// Cumulative like the hardware counters, per stream since the guest asks
    /// each of the four separately (subreport picks it).
    std::array<u64, 4> m_xfb_primitives{};
    /// Base vertex and base instance of the draw being recorded.
    std::array<u32, 2> m_capture_draw_base{};
    std::array<u64, 4> m_xfb_bytes{};
    bool m_xfb_session_active{};
    std::array<u64, 4> m_xfb_session_addr{};
    std::array<u32, 4> m_xfb_session_start{};
    Owned<MTL::Buffer> m_xfb_emitted_buffer;
    std::array<bool, 4> m_xfb_vtg_pending{};
    std::array<u64, 4> m_xfb_vtg_tick{};
    std::array<u32, 4> m_xfb_vtg_stride{};
    std::array<u32, 4> m_xfb_vtg_verts_per_prim{};

    void BeginOrSyncXfbSession();
    void ResolvePendingVtgXfbCounters(u32 stream);

    /// One captured varying to move to guest memory, in bytes.
    struct XfbCopy {
        u32 src_offset;
        u32 dst_offset;
        u32 components;
    };
    /// A capture recorded this frame, also written to guest RAM at TickFrame.
    struct PendingXfbGather {
        /// Borrowed from TransformFeedback's segment pool, not owned, so a
        /// capture never pins a fresh allocation in the residency set.
        MTL::Buffer* capture{};
        u64 tick{};
        u32 capture_stride;
        u32 vertices;
        struct Destination {
            u64 guest_address;
            /// Bytes the guest made available at guest_address. The write-back
            /// is clamped to it, an overrun lands in the next mesh.
            u64 guest_size;
            u32 out_stride;
            boost::container::small_vector<XfbCopy, 16> copies;
        };
        boost::container::small_vector<Destination, 4> destinations;
    };
    std::vector<PendingXfbGather> m_pending_xfb;

    enum class XfbDrain {
        /// Retire what the GPU has already finished. Never blocks.
        WhenComplete,
        /// Wait, then scatter into guest RAM. For anything that makes a capture
        /// visible to the guest.
        ForGuestRead,
        /// As ForGuestRead, at frame end. TickFrame relies on the wait to throttle.
        Throttle,
    };

    /// The guest-RAM gather this exists for is off (see XFB_WRITEBACK_ENABLED), so
    /// fences, queries and sync points no longer wait; that cost ~170 pipeline
    /// drains a frame.
    void DrainPendingXfb(XfbDrain drain);

    NullQueryCache m_query_cache;
    FenceManager m_fence_manager;

    /// So FlushCommands() can avoid committing an empty command buffer.
    u32 m_draw_counter{};
    /// True once any draw in the current frame wrote storage from the vertex stage (e.g. skinning).
    bool m_frame_had_vertex_storage_writes{};
    /// Submission tick of the last recorded draw/clear/dispatch. If a fence already
    /// flushed this tick, FlushCommands() skips a redundant empty submission.
    u64 m_last_draw_tick{};

    /// Opened just before the draw, so uploads can't close the encoder first.
    RenderPassState m_render_pass{};
    bool m_has_render_pass{};
    /// Framebuffer::ColorAliasMask of the pass in m_render_pass.
    u32 m_color_alias_mask{};
    std::array<u32, 8> m_color_alias_groups{};
    PipelineStateInputs m_pipeline_state_inputs{};
    const Framebuffer* m_current_framebuffer{};
    u64 m_rt_dirty_generation{};

    /// Slots ScratchUnwrittenAliases sent to scratch last, kept for draws that
    /// write none of a group so they do not split the pass.
    u32 m_alias_scratch{};
    /// The viewports and scissors last recorded for the worker, see DrawImpl.
    std::array<MTL::Viewport, 16> m_recorded_viewports{};
    std::array<MTL::ScissorRect, 16> m_recorded_scissors{};
    u32 m_recorded_viewport_count{};
    bool m_viewports_recorded{};
    /// Dynamic draw state last recorded for the worker (emitted only on change).
    u32 m_recorded_stencil_front{};
    u32 m_recorded_stencil_back{};
    std::array<f32, 3> m_recorded_depth_bias{};
    std::array<f32, 4> m_recorded_blend_color{};
    bool m_dynamic_recorded{};
    /// Push constant payload last recorded for the worker (emitted only on change).
    u32 m_recorded_attachment_reads{};
    u32 m_recorded_down_factor_bits{0x3F800000u};
    std::array<f32, 4> m_recorded_render_area{};
    std::array<u32, 6> m_recorded_rescaling_masks{};
    bool m_push_recorded{};
    /// Cached viewport/scissor inputs so unchanged draws skip the 16-slot recompute.
    const void* m_cached_vp_engine{};
    s64 m_cached_vp_area_width{};
    s64 m_cached_vp_area_height{};
    f32 m_cached_vp_scale{};
    f32 m_cached_vp_depth_offset{};
    bool m_cached_vp_any_scissor{};
    /// Render pass serial & attachment write mask already reported to TextureCache.
    u64 m_pass_marked_rt_serial{~0ull};
    u32 m_pass_marked_rt_mask{};
    const Framebuffer* m_alias_framebuffer{};

    AccelerateDMA m_accelerate_dma;
    /// Payloads waiting behind a fence, see Query.
    std::atomic<u32> m_payloads_held{0};
    /// Submission whose draws had their vertex buffer usage marked.
    u64 m_vertex_usage_tick{};
    /// Reused per-draw descriptor scratch vectors and sampler memoizer (matches Vulkan's
    /// GraphicsPipeline::sampler_lookup) so large bindless arrays (e.g. RE Engine 929-2644
    /// textures/stage) avoid repeated heap reallocations and redundant guest-memory TSC lookups.
    std::vector<VideoCommon::ImageViewInOut> m_scratch_views;
    std::vector<VideoCommon::SamplerId> m_scratch_samplers;
    Vulkan::DrawDescriptorCache<VideoCommon::SamplerId, 256> m_sampler_cache;
    /// Per-tick cache for Set 1 bindless texture/sampler tables so draws sharing
    /// the same guest TIC/TSC table skip rebuilding and re-hashing thousands of
    /// entries on every draw.
    struct DirectTableCacheEntry {
        u64 tick{};
        u64 view_generation{};
        u64 signature{};
        u32 total_views{};
        u32 runtime_sampler_count{};
        u32 rescaled_mask{};
        bool is_rescaling{};
        bool is_compute{};
        StagingBufferRef table_buffer{};
        std::vector<VideoCommon::ImageViewId> view_ids;
        std::vector<VideoCommon::SamplerId> sampler_ids;
    };
    std::array<DirectTableCacheEntry, 8> m_direct_table_cache{};
    u32 m_direct_table_cache_next{};
    /// Per-tick cbuf-level cache for large Set 0 combined texture+sampler arrays
    /// (e.g. RE Engine's 256-928 element probe/decal arrays). Matching raw cbuf bytes
    /// lets Phase 1 push only the deduplicated unique TIC/TSC handles through
    /// FillGraphicsImageViews + CheckFeedbackLoop + GetGraphicsSamplerId, skipping
    /// >95% of redundant handle reads and view lookups while staying 100% exact on
    /// silent TIC/TSC mutations, CpuModified uploads, and feedback loops.
    struct LargeSet0ArrayCacheEntry {
        u64 tick{};
        u64 view_generation{};
        GPUVAddr cbuf_addr{};
        u32 cbuf_offset{};
        u32 stride{};
        u32 count{};
        u32 shift_left{};
        u32 mask{};
        u32 handle_or_const{};
        Shader::TextureType type{};
        bool is_multisample{};
        bool via_header_index{};
        bool is_rescaling{};
        bool desc_rescaled{};
        u64 slots_hash{};
        std::vector<u32> raw_handles;
        std::vector<u32> unique_tic_indices;
        std::vector<u32> unique_tsc_indices;
        std::vector<u16> elem_to_unique_tic;
        std::vector<u16> elem_to_unique_tsc;
        std::vector<VideoCommon::ImageViewId> unique_view_ids;
        std::vector<VideoCommon::SamplerId> unique_sampler_ids;
        std::vector<u64> slots;
    };
    std::array<LargeSet0ArrayCacheEntry, 16> m_large_set0_cache{};
    u32 m_large_set0_cache_next{};
    /// Submits queued readback copies promptly when leaving a CPU-read render target.
    void CommitPendingReadbacks();
    u32 m_frame_draws{};
    u32 m_frame_skipped_draws{};
};

} // namespace Metal
