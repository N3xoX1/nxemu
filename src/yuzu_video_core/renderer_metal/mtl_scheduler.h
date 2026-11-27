// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

#include <boost/container/small_vector.hpp>
#include <span>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "yuzu_common/alignment.h"
#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/polyfill_thread.h"
#include "yuzu_video_core/metal_common/metal_wrapper.h"
#include "yuzu_video_core/renderer_metal/mtl_encoder.h"
#include "yuzu_video_core/renderer_metal/mtl_master_semaphore.h"
#include "yuzu_video_core/renderer_metal/mtl_render_pass_state.h"

namespace Metal {

class Device;

/// Metal only allows one open encoder at a time per command buffer.
enum class EncoderType {
    None,
    Render,
    Compute,
    Blit,
};

/// A command asks for the encoder it needs and we close whatever was open first, so no call
/// site has to know the rule.
class CommandContext {
public:
    explicit CommandContext(class Scheduler& scheduler_) : scheduler{scheduler_} {}

    [[nodiscard]] RenderEncoder RequestRender(const RenderPassState& state);

    /// The render encoder that is open right now, or null.
    [[nodiscard]] RenderEncoder CurrentRender() const;

    [[nodiscard]] ComputeEncoder RequestCompute();
    [[nodiscard]] BlitEncoder RequestBlit(bool barrier = true);
    /// Emits an intra-encoder barrier only if a blit or compute encoder is
    /// already open. No-op in a render pass or outside any encoder.
    void BarrierOpenBlitIfNeeded();

    void EndEncoding();

    /// For the few things that encode into a command buffer instead of an encoder (MetalFX).
    [[nodiscard]] MTL4::CommandBuffer* MTL4CommandBuffer() const;

    /// All of it encoder state, so reset it whenever an encoder is opened or re-entered.
    struct RenderStateCache {
        static constexpr u32 MAX_BUFFERS = 31;
        static constexpr u32 MAX_VIEWPORTS = 16;

        MTL::RenderPipelineState* pipeline{};
        MTL::DepthStencilState* depth_stencil{};
        std::array<u64, MAX_BUFFERS> vertex_addr{};
        u64 fragment_addr{};
        u64 fragment_table_addr{};

        /// ViewportState::generation applied to this encoder, 0 for none.
        u64 viewport_generation{};
        /// DynamicDrawState::generation applied to this encoder, 0 for none.
        u64 dynamic_generation{};
        /// PushConstantState::generation applied to fragment/vertex stages, 0 for none.
        u64 fragment_push_generation{};
        u64 vertex_push_generation{};

        u32 cull_mode{~0u};
        u32 winding{~0u};
        u32 depth_clip{~0u};
        u32 fill_mode{~0u};
        u32 stencil_front{~0u};
        u32 stencil_back{~0u};
        std::array<f32, 3> depth_bias{};
        std::array<f32, 4> blend_color{};
        /// Visibility slot the encoder is counting into, or ~0 for disabled.
        u32 visibility_slot{~0u};
        bool depth_bias_valid{};
        bool blend_color_valid{};
        bool vertex_queue_barrier_done{};
        bool has_drawn{};
    };

    [[nodiscard]] RenderStateCache& RenderCache() const;

    /// Returns true if a StageFragment -> StageVertex queue barrier is required
    /// before a draw whose vertex stage reads textures/images or storage buffers.
    [[nodiscard]] bool NeedsVertexBarrierFor(bool reads_textures, bool reads_buffers) const noexcept;
    void MarkVertexBarrierDone() const noexcept;
    void NoteRenderStorageWrite() const noexcept;

    /// The viewports and scissors draws use, set by a command only when they change so
    /// draws don't carry them. Outlives encoders, unlike RenderStateCache.
    struct ViewportState {
        std::array<MTL::Viewport, RenderStateCache::MAX_VIEWPORTS> viewports{};
        std::array<MTL::ScissorRect, RenderStateCache::MAX_VIEWPORTS> scissors{};
        u32 count{1};
        u64 generation{1};
    };

    /// Infrequently changing dynamic render state (stencil refs, depth bias, blend color),
    /// recorded only on change so per-draw lambdas stay compact. Outlives encoders.
    struct DynamicDrawState {
        u32 stencil_front{0};
        u32 stencil_back{0};
        std::array<f32, 3> depth_bias{};
        std::array<f32, 4> blend_color{};
        u64 generation{1};
    };

    /// Graphics push-constant payload (render area, rescaling masks, attachment reads),
    /// recorded only on change. Outlives encoders.
    struct PushConstantState {
        u32 fragment_attachment_reads{0};
        u32 down_factor_bits{0x3F800000u}; // 1.0f
        std::array<f32, 4> render_area{};
        std::array<u32, 6> rescaling_masks{};
        u64 generation{1};
    };

    [[nodiscard]] ViewportState& Viewports() const;
    [[nodiscard]] DynamicDrawState& DynamicState() const;
    [[nodiscard]] PushConstantState& PushConstants() const;

    /// Ask an encoder that was opened without a visibility buffer to count and the driver faults.
    [[nodiscard]] MTL::Buffer* CurrentPassVisibilityBuffer() const;

    /// Needed after anything that binds through CurrentRender() without keeping the cache up
    /// to date (the clear and blit helpers).
    void InvalidateRenderCache() const;

    [[nodiscard]] Scheduler& OwningScheduler() const noexcept {
        return scheduler;
    }

private:
    Scheduler& scheduler;
};

/// Records GPU work on the calling thread as chunked lambdas and replays it on a worker
/// thread, driving the encoder state machine.
class Scheduler : Common::NonCopyable, Common::NonMoveable {
    friend class CommandContext;

public:
    explicit Scheduler(const Device& device);
    ~Scheduler();

    /// @p after_commit runs on the worker right after the commit, for work that has to be
    /// ordered behind this submission and not just recorded before it. signalDrawable is why.
    u64 Flush(std::function<void()> after_commit = {});

    /// Flushes now when no render pass is open, otherwise at the end of the open one: a
    /// submission boundary inside a pass stores and reloads every attachment on a tiler.
    void FlushAtPassBoundary();

    void Finish();

    void WaitWorker();

    void DispatchWork();

    /// Recorded, so it takes effect in submission order and not right away.
    void RequestRenderPass(const RenderPassState& state);

    /// Atomically opens/continues @p state and invokes @p record_body under a single
    /// chunk_mutex lock (callers should use RecordLocked inside @p record_body).
    /// Prevents concurrent threads (e.g. NCE FlushNativeRegion) from closing the
    /// render encoder or flushing between RequestRenderPass and the draw command.
    template <typename Fn>
    void RecordInRenderPass(const RenderPassState& state, Fn&& record_body) {
        bool needs_sweep = false;
        {
            std::unique_lock lock{chunk_mutex};
            RequestRenderPassLocked(lock, state, needs_sweep);
            record_body(*this);
        }
        if (needs_sweep) {
            SweepKeepAlive();
        }
    }

    /// Closes the submission now when the pass that @p state would open is due to close it.
    /// Called before a draw binds its resources, so the buffer cache marks their usage
    /// with the tick of the submission the draw actually lands in.
    void FlushIfPassBoundaryDue(const RenderPassState& state);

    /// While held, a boundary flush (pass limit or FlushAtPassBoundary) waits for the next
    /// boundary: a draw holds it from binding its resources until its pass is requested.
    void DeferBoundaryFlush(bool defer) noexcept {
        defer_boundary_flush.store(defer, std::memory_order_release);
    }

    void RequestOutsideRenderPassOperationContext();

    /// Closes any open encoder without looking at the render-pass mirror. See the definition.
    void RequestCloseEncoders();

    /// Ends the pass so a texture that is also an attachment can be sampled, but only if a draw
    /// wrote depth since the last break. Each break stores and reloads every attachment.
    void RequestFeedbackBarrier();

    /// Pass the state too: a stale last_requested_pass answers for an encoder the worker no
    /// longer has.
    void NotifyRenderPassOpened(const RenderPassState& state);

    void MarkDepthWritten() noexcept {
        pass_wrote_depth.store(true, std::memory_order_release);
    }

    /// Attachment bits for MarkPassDrawn: bit i is colour attachment i.
    static constexpr u32 DEPTH_ATTACHMENT_BIT = 1u << 8;
    static constexpr u32 ALL_ATTACHMENTS = ~0u;

    /// @p written_attachments says which attachments the draw may have modified, so a later
    /// draw sampling an untouched one keeps the pass. Defaults to all of them.
    void MarkPassDrawn(u32 written_attachments = ALL_ATTACHMENTS,
                       bool storage_writes = true) noexcept {
        pass_drew.store(true, std::memory_order_release);
        pass_written_attachments.fetch_or(written_attachments, std::memory_order_release);
        if (storage_writes) {
            pass_storage_writes.store(true, std::memory_order_release);
        }
    }

    /// Before the first draw the attachment still matches the texture behind it, so a feedback
    /// break is pure cost.
    [[nodiscard]] bool PassDrew() const noexcept {
        return pass_drew.load(std::memory_order_acquire);
    }

    /// Attachments whose tile contents may differ from memory, see MarkPassDrawn.
    [[nodiscard]] u32 PassWrittenAttachments() const noexcept {
        return pass_written_attachments.load(std::memory_order_acquire);
    }

    /// Not deferred despite the name, that got reverted, it measured no better. Attachment RMW
    /// is safe, Apple orders load/store per fragment in primitive order.
    void RequestDeferredFragmentBarrier() {
        // No write to order against yet, and the break still costs a full attachment store and
        // reload. Legends Z-A fires ~80 of these a frame against 1080p targets. Attachment
        // writes are ordered by the feedback-loop test, so only storage writes need the break.
        if (!pass_drew.load(std::memory_order_acquire) ||
            !pass_storage_writes.load(std::memory_order_acquire)) {
            return;
        }
        RequestOutsideRenderPassOperationContext();
    }

    [[nodiscard]] bool IsRenderPassActive() const noexcept {
        return recorded_pass_active.load(std::memory_order_acquire);
    }

    /// Returns true when a render pass compatible with @p state is currently open.
    [[nodiscard]] bool IsCurrentRenderPass(const RenderPassState& state) const noexcept;

    /// Notes that commands recorded on the current submission write to @p buffer.
    void MarkBufferWrite(const MTL::Buffer* buffer);

    /// Returns true when the current submission writes to @p buffer. Commands recorded on
    /// the upload command buffer run before these writes and would read stale contents.
    [[nodiscard]] bool IsBufferWritten(const MTL::Buffer* buffer) const noexcept;

    /// Returns true when any buffer has been written in the current submission.
    [[nodiscard]] bool HasWrittenBuffers() const noexcept;

    /// Counts the render passes ended so far, on the recording side. Attachment
    /// writes made before the current value are stored to memory.
    [[nodiscard]] u64 RenderPassEndTick() const noexcept {
        return pass_end_tick.load(std::memory_order_acquire);
    }

    /// Adaptive per-submission render-pass ceiling: starts high to cut GPU commit
    /// overhead and upload StageAll bubbles on fast titles (SMO), and steps down
    /// automatically if a heavy title (Legends Z-A) triggers a GPU watchdog timeout.
    static constexpr u32 MIN_PASSES_PER_SUBMISSION = 16;
    static constexpr u32 MAX_PASSES_PER_SUBMISSION = 64;

    [[nodiscard]] u32 MaxPassesPerSubmission() const noexcept {
        return max_passes_per_submission.load(std::memory_order_relaxed);
    }
    void SetMaxPassesPerSubmission(u32 limit) noexcept {
        max_passes_per_submission.store(
            std::clamp(limit, MIN_PASSES_PER_SUBMISSION, MAX_PASSES_PER_SUBMISSION),
            std::memory_order_relaxed);
    }

    /// The lambda lives inline in a chunk arena, so keep it small and trivially relocatable.
    template <typename T>
        requires std::is_invocable_v<T, CommandContext&>
    void Record(T&& command) {
        std::scoped_lock lock{chunk_mutex};
        RecordLocked(command);
    }

    using RenderEncoderRef = RenderEncoder;
    using ComputeEncoderRef = ComputeEncoder;
    using BlitEncoderRef = BlitEncoder;

    /// Record with chunk_mutex already held, see the comment in Flush.
    template <typename T>
        requires std::is_invocable_v<T, CommandContext&>
    void RecordLocked(T&& command) {
        if (chunk->Record(command)) {
            return;
        }
        DispatchWorkLocked();
        (void)chunk->Record(command);
    }

    /// Bumped each time a render pass is opened rather than continued.
    [[nodiscard]] u64 PassSerial() const noexcept {
        return pass_serial.load(std::memory_order_acquire);
    }

    [[nodiscard]] u64 CurrentTick() const noexcept {
        return master_semaphore->CurrentTick();
    }

    [[nodiscard]] bool IsFree(u64 tick) const noexcept {
        return master_semaphore->IsFree(tick);
    }

    /// Blocks until the GPU reaches @p tick, flushing first if it hasn't been submitted yet.
    void Wait(u64 tick);

    /// Metal 4 command buffers do NOT retain the resources their commands touch, so anything
    /// transient a recorded command refers to is a UAF without this.
    void KeepAlive(NS::Object* object, const MTL::Allocation* allocation = nullptr);

    [[nodiscard]] MasterSemaphore& GetMasterSemaphore() const noexcept {
        return *master_semaphore;
    }

    void RegisterOnSubmit(std::function<void()>&& func) {
        on_submit = std::move(func);
    }

private:
    class Command {
    public:
        virtual ~Command() = default;
        virtual void Execute(CommandContext& context) const = 0;

        Command* GetNext() const noexcept {
            return next;
        }

        void SetNext(Command* next_) noexcept {
            next = next_;
        }

    private:
        Command* next = nullptr;
    };

    template <typename T>
    class TypedCommand final : public Command {
    public:
        explicit TypedCommand(T&& command_) : command{std::move(command_)} {}
        ~TypedCommand() override = default;

        TypedCommand(TypedCommand&&) = delete;
        TypedCommand& operator=(TypedCommand&&) = delete;

        void Execute(CommandContext& context) const override {
            command(context);
        }

    private:
        T command;
    };

    class CommandChunk final {
    public:
        void ExecuteAll(CommandContext& context);

        template <typename T>
        bool Record(T& command) {
            using FuncType = TypedCommand<T>;
            static_assert(sizeof(FuncType) < sizeof(data), "Lambda is too large");

            command_offset = Common::AlignUp(command_offset, alignof(FuncType));
            if (command_offset > sizeof(data) - sizeof(FuncType)) {
                return false;
            }
            Command* const current_last = last;
            last = new (data.data() + command_offset) FuncType(std::move(command));

            if (current_last) {
                current_last->SetNext(last);
            } else {
                first = last;
            }
            command_offset += sizeof(FuncType);
            return true;
        }

        void MarkSubmit() noexcept {
            submit = true;
        }

        [[nodiscard]] bool Empty() const noexcept {
            return command_offset == 0;
        }

        [[nodiscard]] bool HasSubmit() const noexcept {
            return submit;
        }

    private:
        Command* first = nullptr;
        Command* last = nullptr;

        size_t command_offset = 0;
        bool submit = false;
        alignas(std::max_align_t) std::array<u8, 0x8000> data{};
    };

    /// Retained, not raw: an encoder stays open across chunks while the worker's autorelease
    /// pool is per chunk.
    struct EncoderState {
        EncoderType type{EncoderType::None};
        /// There is no MTL4 blit encoder at all, so `mtl4_blit` is compute-typed on purpose.
        Owned<MTL4::RenderCommandEncoder> mtl4_render{};
        Owned<MTL4::ComputeCommandEncoder> mtl4_compute{};
        Owned<MTL4::ComputeCommandEncoder> mtl4_blit{};
        RenderPassState pass{};
        CommandContext::RenderStateCache render_cache{};
        CommandContext::ViewportState viewport_state{};
        CommandContext::DynamicDrawState dynamic_state{};
        CommandContext::PushConstantState push_state{};
        /// Worker-thread state, reset at each commit.
        SubmissionContents opened{};
    };

    void WorkerThread(std::stop_token stop_token);

    void AcquireNewChunk();

    void DispatchWorkLocked();
    void AllocateNewCommandBuffer();

public:
    /// Emits a Blit->Blit intra-encoder barrier inside the Metal 4 upload
    /// encoder ONLY when [@p begin, @p end) overlaps an earlier upload to the
    /// same @p dst_buffer in this submission. Uses per-buffer bounding intervals
    /// so monotonic ring-buffer uploads take O(1) with zero barriers.
    void EnsureUploadOrder(MTL4::ComputeCommandEncoder* encoder, const MTL::Buffer* dst_buffer,
                           u64 begin, u64 end) {
        if (dst_buffer == nullptr || begin >= end) {
            return;
        }
        UploadWriteRange* bounds = nullptr;
        for (UploadWriteRange& b : upload_buffer_bounds) {
            if (b.buffer == dst_buffer) {
                bounds = &b;
                break;
            }
        }
        if (bounds == nullptr) {
            upload_buffer_bounds.push_back(UploadWriteRange{dst_buffer, begin, end});
            upload_written_ranges.push_back(UploadWriteRange{dst_buffer, begin, end});
            return;
        }
        // O(1) fast path: strictly outside every previous write to this buffer.
        if (begin >= bounds->end || end <= bounds->begin) {
            bounds->begin = std::min(bounds->begin, begin);
            bounds->end = std::max(bounds->end, end);
            upload_written_ranges.push_back(UploadWriteRange{dst_buffer, begin, end});
            return;
        }
        bool overlaps = false;
        for (const UploadWriteRange& prev : upload_written_ranges) {
            if (prev.buffer == dst_buffer && begin < prev.end && prev.begin < end) {
                overlaps = true;
                break;
            }
        }
        if (overlaps) {
            encoder->barrierAfterEncoderStages(MTL::StageBlit, MTL::StageBlit,
                                               MTL4::VisibilityOptionDevice);
            upload_written_ranges.clear();
            upload_buffer_bounds.clear();
            upload_buffer_bounds.push_back(UploadWriteRange{dst_buffer, begin, end});
        } else {
            bounds->begin = std::min(bounds->begin, begin);
            bounds->end = std::max(bounds->end, end);
        }
        upload_written_ranges.push_back(UploadWriteRange{dst_buffer, begin, end});
    }

    /// Records a staging upload onto the upload command buffer, so it doesn't evict the open
    /// render pass (one open encoder is per command buffer, not per queue). Only safe for copies
    /// the buffer cache marks can_reorder_upload: MTL4 runs the two buffers in commit order, so
    /// this lands ahead of every draw in that submission. Templated so captures live inline in
    /// CommandChunk without heap-allocating std::function.
    template <typename T>
        requires std::is_invocable_v<T, Scheduler&, MTL4::ComputeCommandEncoder*>
    void RecordUpload(T&& command) {
        Record([cmd = std::forward<T>(command)](CommandContext& context) {
            Scheduler& self = context.OwningScheduler();
            self.WorkerBeginUpload();
            if (!self.mtl4_upload_encoder) {
                return;
            }
            if (self.mtl4_upload_dispatched) {
                // Order prior upload compute reads (e.g. IndexPass::Widen) before a
                // subsequent upload copy overwrites a dynamic source buffer.
                self.mtl4_upload_encoder->barrierAfterEncoderStages(
                    MTL::StageDispatch, MTL::StageBlit, MTL4::VisibilityOptionDevice);
                self.mtl4_upload_dispatched = false;
            }
            cmd(self, self.mtl4_upload_encoder.get());
            self.mtl4_upload_has_work = true;
        });
    }

    /// Dispatches a compute kernel onto the upload command buffer so it runs after
    /// all prior submissions (e.g. reading visibility results from an earlier tick)
    /// and before the current main command buffer, WITHOUT closing the open render
    /// pass. Only valid when every GPU resource read by the kernel was written in a
    /// strictly earlier submission (tick < CurrentTick()) or uploaded via RecordUpload.
    template <typename T>
        requires std::is_invocable_v<T, ComputeEncoder&>
    void RecordUploadCompute(T&& command) {
        Record([cmd = std::forward<T>(command)](CommandContext& context) {
            Scheduler& self = context.OwningScheduler();
            self.WorkerBeginUpload();
            if (!self.mtl4_upload_encoder || !self.mtl4_upload_table) {
                return;
            }
            if (!self.upload_written_ranges.empty()) {
                self.mtl4_upload_encoder->barrierAfterEncoderStages(
                    MTL::StageBlit, MTL::StageDispatch, MTL4::VisibilityOptionDevice);
                self.upload_written_ranges.clear();
                self.upload_buffer_bounds.clear();
            }
            ComputeEncoder encoder{self.mtl4_upload_encoder.get()};
            if (!self.mtl4_upload_table_bound) {
                encoder.SetArgumentTable(self.mtl4_upload_table.get(),
                                         &self.mtl4_inline_rings[self.mtl4_allocator_index],
                                         &self.mtl4_null_bindings);
                self.mtl4_upload_table_bound = true;
            } else {
                encoder.AttachArgumentTable(self.mtl4_upload_table.get(),
                                            &self.mtl4_inline_rings[self.mtl4_allocator_index],
                                            &self.mtl4_null_bindings);
            }
            cmd(encoder);
            self.mtl4_upload_has_work = true;
            self.mtl4_upload_dispatched = true;
        });
    }

private:
    void RequestRenderPassLocked(std::unique_lock<std::mutex>& lock, const RenderPassState& state,
                                 bool& needs_sweep);
    u64 FlushLocked(std::unique_lock<std::mutex>& lock, std::function<void()> after_commit = {});
    [[nodiscard]] MTL4::CommandBuffer* WorkerFinishUpload();
    void WorkerBeginUpload();

    RenderEncoder WorkerRequestRender(const RenderPassState& state);
    ComputeEncoder WorkerRequestCompute();

    BlitEncoder WorkerRequestBlit(bool barrier = true);
    void WorkerEndEncoding();

    const Device& device;

    std::unique_ptr<MasterSemaphore> master_semaphore;

    /// MTL4 splits buffer from allocator: the allocator owns the memory commands encode into
    /// and is reset once the GPU is done with it, the buffer is a reusable handle.
    Owned<MTL4::CommandBuffer> mtl4_command_buffer{};

    /// See RecordUpload. Kept out of the main buffer's encoder state machine so the render
    /// encoder stays open across an upload.
    struct UploadWriteRange {
        const MTL::Buffer* buffer;
        u64 begin;
        u64 end;
    };
    Owned<MTL4::CommandBuffer> mtl4_upload_buffer{};
    Owned<MTL4::ComputeCommandEncoder> mtl4_upload_encoder{};
    std::vector<UploadWriteRange> upload_written_ranges;
    std::vector<UploadWriteRange> upload_buffer_bounds;
    bool mtl4_upload_open{false};
    bool mtl4_upload_has_work{false};
    bool mtl4_upload_table_bound{false};
    bool mtl4_upload_dispatched{false};
    bool mtl4_committed{true};

    /// Compute and blit encoders write the vertex/index buffers and generated textures, so only
    /// after one does a render encoder's vertex stage have to wait. Worker only.
    bool compute_or_blit_since_render_barrier{true};
    /// Worker only: a prior render pass ended since the last StageVertex barrier.
    bool render_pass_ended_since_vertex_barrier{false};
    /// Worker only: a render draw with storage writes ran since the last StageVertex barrier.
    bool render_wrote_storage_since_vertex_barrier{false};

    /// Allocators round-robin and only reset once the GPU has finished everything encoded from
    /// them, which the parallel tick array records. This is submission depth, not frame depth.
    /// Heavy titles (e.g. TOTK) submit ~20 command buffers per frame, so 32 slots keep ~1.5-2
    /// frames in flight without stalling the worker thread on allocator recycling.
    static constexpr size_t MAX_MTL4_ALLOCATORS = 32;
    std::array<Owned<MTL4::CommandAllocator>, MAX_MTL4_ALLOCATORS> mtl4_allocators{};
    /// An allocator backs one command buffer at a time, so uploads can't share the main ring.
    std::array<Owned<MTL4::CommandAllocator>, MAX_MTL4_ALLOCATORS> mtl4_upload_allocators{};
    std::array<u64, MAX_MTL4_ALLOCATORS> mtl4_allocator_ticks{};
    size_t mtl4_allocator_index{0};

    /// Argument tables, the MTL4 replacement for setVertexBuffer and friends.
    Owned<MTL4::ArgumentTable> mtl4_vertex_table{};
    Owned<MTL4::ArgumentTable> mtl4_fragment_table{};
    Owned<MTL4::ArgumentTable> mtl4_compute_table{};
    Owned<MTL4::ArgumentTable> mtl4_upload_table{};

    /// One ring per allocator, so a ring is only rewound once the GPU has finished reading it.
    std::array<Owned<MTL::Buffer>, MAX_MTL4_ALLOCATORS> mtl4_inline_buffers{};
    std::array<InlineConstantRing, MAX_MTL4_ALLOCATORS> mtl4_inline_rings{};

    /// Zero-filled, always-resident memory an unbound buffer slot points at. The argument tables
    /// outlive every encoder, so a slot we never write keeps the last resource it got.
    Owned<MTL::Buffer> mtl4_null_buffer{};
    NullBindings mtl4_null_bindings{};

    void CreateMetal4Bindings();

    void SweepKeepAlive();

    struct PendingRelease {
        NS::Object* object;
        /// Non-null when the object is also in the device residency set.
        const MTL::Allocation* allocation;
        u64 tick;
    };
    std::mutex keep_alive_mutex;
    std::vector<PendingRelease> keep_alive;
    std::unordered_set<const MTL::Allocation*> sweep_in_flight;
    EncoderState encoder{};

    /// Tick claimed by a Flush that found no command buffer, folded into the next commit so no
    /// Wait is stranded on it. Single writer, so no RMW needed.
    std::atomic<u64> pending_signal_tick{};
    /// Highest tick the worker thread has actually committed to the Metal queue.
    /// Scheduler::Wait waits on this instead of WaitWorker() so waiting for an already-
    /// submitted tick never drains newer chunks or contends on execution_mutex.
    std::atomic<u64> committed_tick{0};
    std::mutex commit_mutex;
    std::condition_variable commit_cv;

    void WaitCommitted(u64 tick);

    /// See RequestFeedbackBarrier. Atomic because recording helpers and concurrent
    /// NCE/applet threads can read/reset these flags without sharing a single lock.
    std::atomic<bool> pass_wrote_depth{false};
    std::atomic<u32> pass_written_attachments{0};
    std::atomic<bool> pass_storage_writes{false};
    std::atomic<bool> flush_at_pass_boundary{false};
    /// See PassDrew.
    std::atomic<bool> pass_drew{false};

    /// Mirrors the encoder state on the recording thread, so callers can ask whether a pass is
    /// open without synchronising with the worker. Mutated under chunk_mutex.
    std::atomic<bool> recorded_pass_active{false};
    std::atomic<u64> pass_end_tick{0};

    /// So RequestRenderPass can tell a new encoder from a request the worker folds into the
    /// open one.
    RenderPassState last_requested_pass{};

    std::atomic<u32> passes_since_flush{0};
    std::atomic<u32> max_passes_per_submission{MAX_PASSES_PER_SUBMISSION};
    std::atomic<bool> defer_boundary_flush{false};
    std::atomic<u64> pass_serial{0};

    std::unique_ptr<CommandChunk> chunk;
    /// Guards `chunk` itself. Taking queue_mutex here instead would mean holding it across
    /// every command's construction.
    mutable std::mutex chunk_mutex;
    std::vector<const MTL::Buffer*> written_buffers;
    bool all_buffers_written{false};
    std::function<void()> on_submit;

    std::queue<std::unique_ptr<CommandChunk>> work_queue;
    /// Chunks in work_queue, readable without queue_mutex: the worker spins on it briefly
    /// before sleeping, so most hand-offs find it awake and wake no one.
    std::atomic<u32> queued_chunks{0};
    std::atomic<bool> worker_sleeping{false};
    std::atomic<u32> worker_waiters{0};
    std::vector<std::unique_ptr<CommandChunk>> chunk_reserve;

    std::mutex execution_mutex;
    std::mutex reserve_mutex;
    std::mutex queue_mutex;
    std::condition_variable_any event_cv;
    std::jthread worker_thread;
};

} // namespace Metal
