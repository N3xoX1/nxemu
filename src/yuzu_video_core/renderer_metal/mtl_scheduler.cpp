// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <pthread.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "yuzu_common/literals.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/settings.h"
#include "yuzu_common/spin_lock.h"
#include "video_settings.h"
#include "yuzu_common/thread.h"
#include "yuzu_video_core/metal_common/metal_device.h"
#include "yuzu_video_core/renderer_metal/mtl_scheduler.h"

namespace Metal {

using namespace Common::Literals;

// Stages an encoder-scoped barrier can name on a render encoder. StageAll is
// rejected, and only the validation layer tells you.
constexpr MTL::Stages RENDER_ENCODER_STAGES =
    static_cast<MTL::Stages>(MTL::StageVertex | MTL::StageFragment);

// Producing side: vertex only, Fragment is NOT legal and silently invalidates it.
constexpr MTL::Stages RENDER_ENCODER_AFTER_STAGES = MTL::StageVertex;

// Blit is in here because Metal 4 has no blit encoder, copies run on compute.
constexpr MTL::Stages COMPUTE_ENCODER_STAGES =
    static_cast<MTL::Stages>(MTL::StageDispatch | MTL::StageBlit);

RenderEncoder CommandContext::RequestRender(const RenderPassState& state) {
    return scheduler.WorkerRequestRender(state);
}

RenderEncoder CommandContext::CurrentRender() const {
    if (scheduler.encoder.type != EncoderType::Render) {
        return {};
    }
    // Tables were already bound on this render encoder by WorkerRequestRender.
    RenderEncoder wrapper{scheduler.encoder.mtl4_render.get()};
    wrapper.AttachArgumentTables(scheduler.mtl4_vertex_table.get(),
                                 scheduler.mtl4_fragment_table.get(),
                                 &scheduler.mtl4_inline_rings[scheduler.mtl4_allocator_index],
                                 &scheduler.mtl4_null_bindings);
    return wrapper;
}

MTL::Buffer* CommandContext::CurrentPassVisibilityBuffer() const {
    if (scheduler.encoder.type != EncoderType::Render) {
        return nullptr;
    }
    return scheduler.encoder.pass.visibility_buffer;
}

ComputeEncoder CommandContext::RequestCompute() {
    return scheduler.WorkerRequestCompute();
}

BlitEncoder CommandContext::RequestBlit(bool barrier) {
    return scheduler.WorkerRequestBlit(barrier);
}

void CommandContext::BarrierOpenBlitIfNeeded() {
    if (scheduler.encoder.type == EncoderType::Blit ||
        scheduler.encoder.type == EncoderType::Compute) {
        MTL4::ComputeCommandEncoder* const open =
            scheduler.encoder.type == EncoderType::Blit ? scheduler.encoder.mtl4_blit.get()
                                                        : scheduler.encoder.mtl4_compute.get();
        if (open != nullptr) {
            open->barrierAfterEncoderStages(COMPUTE_ENCODER_STAGES, COMPUTE_ENCODER_STAGES,
                                            MTL4::VisibilityOptionDevice);
        }
    }
}

void CommandContext::EndEncoding() {
    scheduler.WorkerEndEncoding();
}

CommandContext::RenderStateCache& CommandContext::RenderCache() const {
    return scheduler.encoder.render_cache;
}

bool CommandContext::NeedsVertexBarrierFor(bool reads_textures, bool reads_buffers) const noexcept {
    if (scheduler.encoder.render_cache.vertex_queue_barrier_done) {
        return false;
    }
    return (reads_textures && scheduler.render_pass_ended_since_vertex_barrier) ||
           (reads_buffers && scheduler.render_wrote_storage_since_vertex_barrier);
}

void CommandContext::MarkVertexBarrierDone() const noexcept {
    scheduler.encoder.render_cache.vertex_queue_barrier_done = true;
    scheduler.render_pass_ended_since_vertex_barrier = false;
    scheduler.render_wrote_storage_since_vertex_barrier = false;
}

void CommandContext::NoteRenderStorageWrite() const noexcept {
    scheduler.render_wrote_storage_since_vertex_barrier = true;
}

CommandContext::ViewportState& CommandContext::Viewports() const {
    return scheduler.encoder.viewport_state;
}

CommandContext::DynamicDrawState& CommandContext::DynamicState() const {
    return scheduler.encoder.dynamic_state;
}

CommandContext::PushConstantState& CommandContext::PushConstants() const {
    return scheduler.encoder.push_state;
}

void CommandContext::InvalidateRenderCache() const {
    scheduler.encoder.render_cache = RenderStateCache{};
}

MTL4::CommandBuffer* CommandContext::MTL4CommandBuffer() const {
    // The handle outlives the submission, so a null check alone is not enough.
    return scheduler.mtl4_committed ? nullptr : scheduler.mtl4_command_buffer.get();
}

void Scheduler::CommandChunk::ExecuteAll(CommandContext& context) {
    Command* command = first;
    while (command != nullptr) {
        Command* const next = command->GetNext();
        command->Execute(context);
        command->~Command();
        command = next;
    }
    submit = false;
    command_offset = 0;
    first = nullptr;
    last = nullptr;
}

Scheduler::Scheduler(const Device& device_)
    : device{device_}, master_semaphore{std::make_unique<MasterSemaphore>(device_)} {
    CreateMetal4Bindings();
    AcquireNewChunk();
    AllocateNewCommandBuffer();
    worker_thread = std::jthread([this](std::stop_token token) { WorkerThread(token); });
}

Scheduler::~Scheduler() {
    // MasterSemaphore's feedback handler captures `this`.
    Finish();

    worker_thread.request_stop();
    event_cv.notify_all();
    {
        std::scoped_lock lock{commit_mutex};
    }
    commit_cv.notify_all();
    worker_thread = {};

    SweepKeepAlive();

    // Unconditionally drain any remaining entries (e.g. abnormal shutdown / hung GPU)
    // so raw retained NS::Object pointers and residency entries are never leaked.
    std::scoped_lock lock{keep_alive_mutex};
    if (!keep_alive.empty()) {
        bool removed_any = false;
        std::unordered_map<const MTL::Allocation*, bool> seen;
        seen.reserve(keep_alive.size() * 2 + 8);
        for (const PendingRelease& pending : keep_alive) {
            if (pending.allocation != nullptr && seen.emplace(pending.allocation, true).second) {
                device.RemoveResidentAllocation(pending.allocation);
                removed_any = true;
            }
            if (pending.object != nullptr) {
                pending.object->release();
            }
        }
        keep_alive.clear();
        if (removed_any) {
            device.CommitResidency();
        }
    }
}

void Scheduler::AcquireNewChunk() {
    std::scoped_lock lock{reserve_mutex};
    if (chunk_reserve.empty()) {
        chunk = std::make_unique<CommandChunk>();
        return;
    }
    chunk = std::move(chunk_reserve.back());
    chunk_reserve.pop_back();
}

void Scheduler::CreateMetal4Bindings() {
    ScopedAutoreleasePool pool;
    NS::Error* error = nullptr;

    // MTL4 wants the bind counts up front, so these cover the widest set we bind.
    const auto make_table = [&](const char* label, u32 buffers, u32 textures,
                                u32 samplers) -> Owned<MTL4::ArgumentTable> {
        MTL4::ArgumentTableDescriptor* descriptor = MTL4::ArgumentTableDescriptor::alloc()->init();
        descriptor->setMaxBufferBindCount(buffers);
        descriptor->setMaxTextureBindCount(textures);
        descriptor->setMaxSamplerStateBindCount(samplers);
        // Defaults to false, which leaves every slot UNINITIALISED: a binding we
        // never write reads garbage as a resource id and faults the GPU silently.
        descriptor->setInitializeBindings(true);
        // Vertex attribute strides travel with the address on MTL4.
        descriptor->setSupportAttributeStrides(true);
        descriptor->setLabel(ToNSString(label));
        Owned<MTL4::ArgumentTable> table =
            Adopt(device.GetDevice()->newArgumentTable(descriptor, &error));
        descriptor->release();
        if (!table) {
            LOG_ERROR(Render_Metal, "Failed to create the {} argument table: {}", label,
                      DescribeError(error));
        }
        return table;
    };

    // Hard per-stage limits in Metal. Going over (samplers above 16 especially)
    // is only reported by the validation layer; the table is silently wrong.
    constexpr u32 MAX_TABLE_BUFFERS = 31;
    constexpr u32 MAX_TABLE_TEXTURES = 128;
    constexpr u32 MAX_TABLE_SAMPLERS = 16;

    mtl4_vertex_table =
        make_table("nxemu vertex args", MAX_TABLE_BUFFERS, MAX_TABLE_TEXTURES, MAX_TABLE_SAMPLERS);
    mtl4_fragment_table =
        make_table("nxemu fragment args", MAX_TABLE_BUFFERS, MAX_TABLE_TEXTURES, MAX_TABLE_SAMPLERS);
    mtl4_compute_table =
        make_table("nxemu compute args", MAX_TABLE_BUFFERS, MAX_TABLE_TEXTURES, MAX_TABLE_SAMPLERS);
    mtl4_upload_table = make_table("nxemu upload compute args", 4, 0, 0);

    constexpr u64 INLINE_RING_SIZE = 4_MiB;
    for (size_t i = 0; i < MAX_MTL4_ALLOCATORS; ++i) {
        mtl4_inline_buffers[i] = Adopt(device.GetDevice()->newBuffer(
            INLINE_RING_SIZE, MTL::ResourceStorageModeShared));
        if (!mtl4_inline_buffers[i]) {
            LOG_ERROR(Render_Metal, "Failed to allocate a Metal 4 inline constant ring");
            continue;
        }
        mtl4_inline_buffers[i]->setLabel(MTLSTR("nxemu mtl4 inline constants"));
        mtl4_inline_rings[i].buffer = mtl4_inline_buffers[i].get();
        mtl4_inline_rings[i].gpu_address = mtl4_inline_buffers[i]->gpuAddress();
        mtl4_inline_rings[i].mapped =
            static_cast<u8*>(mtl4_inline_buffers[i]->contents());
        mtl4_inline_rings[i].capacity = INLINE_RING_SIZE;
        mtl4_inline_rings[i].offset = 0;
        // Nothing is implicitly resident on MTL4. Set methods are not thread safe.
        device.AddResidentAllocation(mtl4_inline_buffers[i].get());
    }

    // Shaders load through raw device pointers, so an unbound buffer slot can't
    // sit at address 0. Sized for a strided read of a whole vertex batch.
    constexpr u64 NULL_BUFFER_SIZE = 1_MiB;
    mtl4_null_buffer = Adopt(device.GetDevice()->newBuffer(
        NULL_BUFFER_SIZE, MTL::ResourceStorageModeShared));
    if (mtl4_null_buffer) {
        mtl4_null_buffer->setLabel(MTLSTR("nxemu mtl4 null binding"));
        std::memset(mtl4_null_buffer->contents(), 0, NULL_BUFFER_SIZE);
        device.AddResidentAllocation(mtl4_null_buffer.get());
        mtl4_null_bindings.buffer = mtl4_null_buffer->gpuAddress();
    } else {
        LOG_ERROR(Render_Metal,
                  "Failed to allocate the Metal 4 null binding buffer; unbound buffer slots "
                  "will keep their previous contents");
    }

    device.CommitResidency();
}

void Scheduler::AllocateNewCommandBuffer() {
    ScopedAutoreleasePool mtl4_pool;

    // beginCommandBuffer recycles the driver's encoder context, so an encoder left
    // open here is the wiped-context crash in AGX::ComputeContext::endComputePass.
    if (encoder.type != EncoderType::None) {
        LOG_WARNING(Render_Metal, "Encoder still open at command buffer recycle; closing it");
        WorkerEndEncoding();
    }

    mtl4_allocator_index = (mtl4_allocator_index + 1) % MAX_MTL4_ALLOCATORS;
    Owned<MTL4::CommandAllocator>& allocator = mtl4_allocators[mtl4_allocator_index];
    if (!allocator) {
        allocator = Adopt(device.GetDevice()->newCommandAllocator());
        if (!allocator) {
            LOG_ERROR(Render_Metal, "Failed to create a Metal 4 command allocator");
            return;
        }
    } else {
        const u64 owner_tick = mtl4_allocator_ticks[mtl4_allocator_index];
        if (owner_tick != 0) {
            master_semaphore->Wait(owner_tick);
        }
        allocator->reset();
    }
    // The upload allocator rotates with the main one, covered by the wait above.
    if (Owned<MTL4::CommandAllocator>& upload = mtl4_upload_allocators[mtl4_allocator_index]) {
        upload->reset();
    }
    mtl4_allocator_ticks[mtl4_allocator_index] = 0;
    mtl4_inline_rings[mtl4_allocator_index].Reset();

    // A Metal 4 command buffer is a reusable handle, not a per-frame object.
    if (!mtl4_command_buffer) {
        mtl4_command_buffer = Adopt(device.GetDevice()->newCommandBuffer());
        if (!mtl4_command_buffer) {
            LOG_ERROR(Render_Metal, "Failed to create a Metal 4 command buffer");
            return;
        }
    }
    mtl4_command_buffer->beginCommandBuffer(allocator.get());
    mtl4_committed = false;
    mtl4_command_buffer->setLabel(MTLSTR("nxemu frame"));
    if (MTL::ResidencySet* const set = device.GetResidencySet()) {
        mtl4_command_buffer->useResidencySet(set);
    }
}

void Scheduler::DispatchWork() {
    std::scoped_lock lock{chunk_mutex};
    DispatchWorkLocked();
}

void Scheduler::DispatchWorkLocked() {
    if (chunk->Empty()) {
        return;
    }
    bool notify = false;
    {
        std::scoped_lock lock{queue_mutex};
        work_queue.push(std::move(chunk));
        queued_chunks.fetch_add(1, std::memory_order_release);
        notify = worker_sleeping.load(std::memory_order_relaxed);
    }
    if (notify) {
        event_cv.notify_all();
    }
    AcquireNewChunk();
}

void Scheduler::WaitWorker() {
    DispatchWork();

    // The execution lock also waits out the chunk already in flight.
    std::unique_lock lock{queue_mutex};
    if (!work_queue.empty()) {
        worker_waiters.fetch_add(1, std::memory_order_relaxed);
        event_cv.wait(lock, [this] { return work_queue.empty(); });
        worker_waiters.fetch_sub(1, std::memory_order_relaxed);
    }
    std::scoped_lock execution_lock{execution_mutex};
}


void Scheduler::NotifyRenderPassOpened(const RenderPassState& state) {
    std::scoped_lock lock{chunk_mutex};
    if (recorded_pass_active.load(std::memory_order_relaxed)) {
        pass_end_tick.fetch_add(1, std::memory_order_relaxed);
    }
    recorded_pass_active.store(true, std::memory_order_release);
    last_requested_pass = state;
    pass_wrote_depth.store(false, std::memory_order_relaxed);
    pass_drew.store(false, std::memory_order_relaxed);
    pass_written_attachments.store(0, std::memory_order_relaxed);
    pass_storage_writes.store(false, std::memory_order_relaxed);
    passes_since_flush.fetch_add(1, std::memory_order_relaxed);
    pass_serial.fetch_add(1, std::memory_order_relaxed);
}

void Scheduler::FlushIfPassBoundaryDue(const RenderPassState& state) {
    const u32 pass_limit = MaxPassesPerSubmission();
    if (passes_since_flush.load(std::memory_order_acquire) < pass_limit &&
        !flush_at_pass_boundary.load(std::memory_order_acquire)) {
        return;
    }
    bool should_flush = false;
    {
        std::unique_lock lock{chunk_mutex};
        const bool same_pass = recorded_pass_active.load(std::memory_order_relaxed) &&
                               last_requested_pass.IsCompatibleWith(state);
        if (!same_pass && (passes_since_flush.load(std::memory_order_relaxed) >= pass_limit ||
                           flush_at_pass_boundary.load(std::memory_order_relaxed))) {
            should_flush = true;
        }
    }
    if (should_flush) {
        Flush();
    }
}

void Scheduler::RequestRenderPassLocked(std::unique_lock<std::mutex>& lock,
                                        const RenderPassState& state, bool& needs_sweep) {
    bool active = recorded_pass_active.load(std::memory_order_relaxed);
    bool same_pass = active && last_requested_pass.IsCompatibleWith(state);

    // A pass boundary is the only place we can close a submission for size:
    // splitting one stores and reloads every attachment on a TBDR, ~16 MiB each way
    // for the 1080p depth targets Legends Z-A leans on. The check does NOT need a
    // pass to be open; gating on that let submissions hit 56 encoders and 16 ms
    // against a bound of 24.
    const u32 pass_limit = MaxPassesPerSubmission();
    if (!same_pass && !defer_boundary_flush.load(std::memory_order_acquire) &&
        (passes_since_flush.load(std::memory_order_relaxed) >= pass_limit ||
         flush_at_pass_boundary.load(std::memory_order_relaxed))) {
        FlushLocked(lock);
        lock.lock();
        needs_sweep = true;
        active = recorded_pass_active.load(std::memory_order_relaxed);
        same_pass = active && last_requested_pass.IsCompatibleWith(state);
    }

    if (active && !same_pass) {
        pass_end_tick.fetch_add(1, std::memory_order_relaxed);
    }
    if (!same_pass) {
        // A compatible request differs only in what IsCompatibleWith reads from the new state.
        last_requested_pass = state;
    }

    recorded_pass_active.store(true, std::memory_order_release);
    if (!same_pass) {
        pass_wrote_depth.store(false, std::memory_order_relaxed);
        pass_drew.store(false, std::memory_order_relaxed);
        pass_written_attachments.store(0, std::memory_order_relaxed);
        pass_storage_writes.store(false, std::memory_order_relaxed);
        // A depth clear is a load action, and the feedback gate only sees draws.
        if (state.depth_stencil.texture != nullptr &&
            state.depth_stencil.depth_load == LoadAction::Clear) {
            pass_wrote_depth.store(true, std::memory_order_relaxed);
        }
        // Cleared attachments no longer match memory either.
        u32 cleared_bits = 0;
        for (u32 index = 0; index < state.num_color; ++index) {
            if (state.color[index].texture != nullptr &&
                state.color[index].load_action == LoadAction::Clear) {
                cleared_bits |= 1u << index;
            }
        }
        if (state.depth_stencil.texture != nullptr &&
            (state.depth_stencil.depth_load == LoadAction::Clear ||
             state.depth_stencil.stencil_load == LoadAction::Clear)) {
            cleared_bits |= DEPTH_ATTACHMENT_BIT;
        }
        if (cleared_bits != 0) {
            pass_written_attachments.fetch_or(cleared_bits, std::memory_order_relaxed);
        }
        passes_since_flush.fetch_add(1, std::memory_order_relaxed);
        pass_serial.fetch_add(1, std::memory_order_relaxed);
        RecordLocked([state](CommandContext& context) { (void)context.RequestRender(state); });
    }
}

void Scheduler::RequestRenderPass(const RenderPassState& state) {
    bool needs_sweep = false;
    {
        std::unique_lock lock{chunk_mutex};
        RequestRenderPassLocked(lock, state, needs_sweep);
    }
    if (needs_sweep) {
        SweepKeepAlive();
    }
}

void Scheduler::WorkerBeginUpload() {
    if (mtl4_upload_open) {
        return;
    }
    Owned<MTL4::CommandAllocator>& allocator = mtl4_upload_allocators[mtl4_allocator_index];
    if (!allocator) {
        allocator = Adopt(device.GetDevice()->newCommandAllocator());
        if (!allocator) {
            LOG_ERROR(Render_Metal, "Failed to create a Metal 4 upload allocator");
            return;
        }
    }
    if (!mtl4_upload_buffer) {
        mtl4_upload_buffer = Adopt(device.GetDevice()->newCommandBuffer());
        if (!mtl4_upload_buffer) {
            LOG_ERROR(Render_Metal, "Failed to create the Metal 4 upload command buffer");
            return;
        }
        mtl4_upload_buffer->setLabel(MTLSTR("nxemu mtl4 uploads"));
    }
    mtl4_upload_buffer->beginCommandBuffer(allocator.get());
    if (MTL::ResidencySet* const set = device.GetResidencySet()) {
        mtl4_upload_buffer->useResidencySet(set);
    }
    mtl4_upload_encoder = Retain(mtl4_upload_buffer->computeCommandEncoder());
    if (!mtl4_upload_encoder) {
        LOG_ERROR(Render_Metal, "Failed to open the Metal 4 upload encoder");
        mtl4_upload_buffer->endCommandBuffer();
        return;
    }
    // Committed ahead of the main buffer, but the previous main buffers can still be
    // running: without a queue barrier these copies overwrite guest buffers those
    // commands are reading. Metal 4 does no hazard tracking across command buffers,
    // so the race only shows when the GPU is behind.
    mtl4_upload_encoder->barrierAfterQueueStages(MTL::StageAll, COMPUTE_ENCODER_STAGES,
                                                 MTL4::VisibilityOptionDevice);
    mtl4_upload_open = true;
    mtl4_upload_has_work = false;
    mtl4_upload_table_bound = false;
    upload_written_ranges.clear();
    upload_buffer_bounds.clear();
}

MTL4::CommandBuffer* Scheduler::WorkerFinishUpload() {
    if (!mtl4_upload_open) {
        return nullptr;
    }
    const bool had_work = mtl4_upload_has_work;
    if (mtl4_upload_encoder) {
        if (had_work) {
            // Only producer that never sets compute_or_blit_since_render_barrier,
            // so without this the next render encoder opens on StageFragment alone
            // and the tiler fetches vertex data this copy hasn't landed yet. Commit
            // order isn't completion order and Metal 4 does no hazard tracking.
            mtl4_upload_encoder->barrierAfterStages(COMPUTE_ENCODER_STAGES, MTL::StageAll,
                                                    MTL4::VisibilityOptionDevice);
        }
        mtl4_upload_encoder->endEncoding();
        mtl4_upload_encoder.reset();
    }
    mtl4_upload_buffer->endCommandBuffer();
    mtl4_upload_open = false;
    mtl4_upload_has_work = false;
    mtl4_upload_table_bound = false;
    return had_work ? mtl4_upload_buffer.get() : nullptr;
}


void Scheduler::RequestOutsideRenderPassOperationContext() {
    bool should_flush = false;
    {
        std::unique_lock lock{chunk_mutex};
        if (!recorded_pass_active.load(std::memory_order_relaxed)) {
            return;
        }
        pass_end_tick.fetch_add(1, std::memory_order_relaxed);
        recorded_pass_active.store(false, std::memory_order_release);
        pass_wrote_depth.store(false, std::memory_order_relaxed);
        pass_drew.store(false, std::memory_order_relaxed);
        pass_written_attachments.store(0, std::memory_order_relaxed);
        pass_storage_writes.store(false, std::memory_order_relaxed);
        RecordLocked([](CommandContext& context) { context.EndEncoding(); });
        should_flush = flush_at_pass_boundary.load(std::memory_order_relaxed) &&
                       !defer_boundary_flush.load(std::memory_order_acquire);
    }
    if (should_flush) {
        Flush();
    }
}

void Scheduler::FlushAtPassBoundary() {
    bool flush_now = false;
    {
        std::unique_lock lock{chunk_mutex};
        if (!recorded_pass_active.load(std::memory_order_relaxed)) {
            flush_now = true;
        } else {
            flush_at_pass_boundary.store(true, std::memory_order_release);
        }
    }
    if (flush_now) {
        Flush();
    }
}

void Scheduler::RequestCloseEncoders() {
    // Unconditional: RequestOutsideRenderPassOperationContext only tracks *render*
    // passes and can leave a compute or copy encoder open, which is fatal for
    // MetalFX and friends (AGX::ComputeContext::endComputePass faults).
    std::scoped_lock lock{chunk_mutex};
    if (recorded_pass_active.load(std::memory_order_relaxed)) {
        pass_end_tick.fetch_add(1, std::memory_order_relaxed);
    }
    recorded_pass_active.store(false, std::memory_order_release);
    pass_wrote_depth.store(false, std::memory_order_relaxed);
    pass_drew.store(false, std::memory_order_relaxed);
    pass_written_attachments.store(0, std::memory_order_relaxed);
    pass_storage_writes.store(false, std::memory_order_relaxed);
    RecordLocked([](CommandContext& context) {
        context.EndEncoding();

        // Ending the encoder does not order it, and MetalFX never gets the
        // encoder-open barrier. Compute and not render because a render encoder
        // can't name Fragment as a producer stage on TBDR.
        ComputeEncoder barrier = context.RequestCompute();
        if (barrier != nullptr) {
            barrier.barrierBeforeQueueStages(COMPUTE_ENCODER_STAGES, MTL::StageAll);
        }
        context.EndEncoding();
    });
}

void Scheduler::RequestFeedbackBarrier() {
    if (!pass_wrote_depth.load(std::memory_order_acquire)) {
        return;
    }
    RequestOutsideRenderPassOperationContext();
}

u64 Scheduler::FlushLocked(std::unique_lock<std::mutex>& lock,
                           std::function<void()> after_commit) {
    // chunk_mutex is held by caller; the tick is claimed under it: Flush runs on
    // several threads (guest cores flush for native reads), and a tick claimed outside
    // could be committed after a later one. The GPU would signal the later tick first,
    // and IsFree would release everything the earlier submission still has to read.
    const u64 signal_tick = master_semaphore->NextTick();

    const auto commit = [this, signal_tick,
                         after_commit = std::move(after_commit)](CommandContext& context) {
        context.EndEncoding();

        if (on_submit) {
            on_submit();
        }

        if (mtl4_command_buffer && !mtl4_committed) {
            // A dead queue refuses everything forever.
            if (master_semaphore->ConsumeDeadQueue()) {
                static_cast<void>(device.RecreateMTL4Queue());
            }
            // Let the queue drain after a fault. Metal starts refusing submissions
            // "for causing prior/excessive GPU errors" once enough pile up behind a
            // bad one, and that state is terminal. Use `signal_tick - 1`, NOT
            // CurrentTick() - 1: waiting on the worker for work only the worker can
            // submit deadlocks on the first fault.
            if (signal_tick > 1 && master_semaphore->ConsumeNewFault()) {
                master_semaphore->Wait(signal_tick - 1);
            }
            device.CommitResidency();

            // Uploads go out first in the SAME queue->commit({upload, main}) batch.
            // MTL4 executes buffers within a commit array in order.
            MTL4::CommandBuffer* const upload_buf = WorkerFinishUpload();
            const u64 carried = pending_signal_tick.load(std::memory_order_relaxed);
            master_semaphore->SignalAndCommitMTL4(
                device.GetMTL4Queue(), mtl4_command_buffer.get(), signal_tick,
                carried, encoder.opened, upload_buf);
            encoder.opened = SubmissionContents{};
            pending_signal_tick.store(0, std::memory_order_release);
            // The allocator stays busy until the GPU drains.
            mtl4_allocator_ticks[mtl4_allocator_index] = signal_tick;
            mtl4_committed = true;
            if (after_commit) {
                after_commit();
            }
            const u64 done_tick = std::max(signal_tick, carried);
            {
                std::scoped_lock commit_lock{commit_mutex};
                u64 prev = committed_tick.load(std::memory_order_relaxed);
                while (prev < done_tick &&
                       !committed_tick.compare_exchange_weak(prev, done_tick,
                                                             std::memory_order_release,
                                                             std::memory_order_relaxed)) {
                }
            }
            commit_cv.notify_all();
        } else {
            // We claimed a tick with nothing to attach it to. Leaving it strands
            // every later Wait on a value nothing signals.
            pending_signal_tick.store(
                std::max(pending_signal_tick.load(std::memory_order_relaxed), signal_tick),
                std::memory_order_release);
            LOG_WARNING(Render_Metal,
                        "No Metal 4 command buffer for tick {}; folding it into the next",
                        signal_tick);
            if (after_commit) {
                after_commit();
            }
        }
    };

    // A concurrent DispatchWork between the record and the MarkSubmit would mark a
    // different, empty chunk.
    RecordLocked(commit);
    chunk->MarkSubmit();
    if (recorded_pass_active.load(std::memory_order_relaxed)) {
        pass_end_tick.fetch_add(1, std::memory_order_relaxed);
    }
    recorded_pass_active.store(false, std::memory_order_release);
    pass_wrote_depth.store(false, std::memory_order_relaxed);
    pass_drew.store(false, std::memory_order_relaxed);
    pass_written_attachments.store(0, std::memory_order_relaxed);
    pass_storage_writes.store(false, std::memory_order_relaxed);
    flush_at_pass_boundary.store(false, std::memory_order_relaxed);
    passes_since_flush.store(0, std::memory_order_relaxed);
    written_buffers.clear();
    all_buffers_written = false;
    DispatchWorkLocked();
    lock.unlock();
    return signal_tick;
}

bool Scheduler::IsCurrentRenderPass(const RenderPassState& state) const noexcept {
    std::scoped_lock lock{chunk_mutex};
    return recorded_pass_active.load(std::memory_order_relaxed) &&
           last_requested_pass.IsCompatibleWith(state);
}

void Scheduler::MarkBufferWrite(const MTL::Buffer* buffer) {
    static constexpr size_t MAX_WRITTEN_BUFFERS = 64;
    if (buffer == nullptr) {
        return;
    }
    std::scoped_lock lock{chunk_mutex};
    if (all_buffers_written ||
        std::ranges::find(written_buffers, buffer) != written_buffers.end()) {
        return;
    }
    if (written_buffers.size() == MAX_WRITTEN_BUFFERS) {
        all_buffers_written = true;
        return;
    }
    written_buffers.push_back(buffer);
}

bool Scheduler::IsBufferWritten(const MTL::Buffer* buffer) const noexcept {
    if (buffer == nullptr) {
        return false;
    }
    std::scoped_lock lock{chunk_mutex};
    return all_buffers_written ||
           std::ranges::find(written_buffers, buffer) != written_buffers.end();
}

bool Scheduler::HasWrittenBuffers() const noexcept {
    std::scoped_lock lock{chunk_mutex};
    return all_buffers_written || !written_buffers.empty();
}

u64 Scheduler::Flush(std::function<void()> after_commit) {
    SweepKeepAlive();
    std::unique_lock lock{chunk_mutex};
    return FlushLocked(lock, std::move(after_commit));
}

void Scheduler::Finish() {
    Wait(Flush({}));
}

void Scheduler::WaitCommitted(u64 tick) {
    if (committed_tick.load(std::memory_order_acquire) >= tick) {
        return;
    }
    std::unique_lock lock{commit_mutex};
    commit_cv.wait(lock, [this, tick] {
        return committed_tick.load(std::memory_order_acquire) >= tick ||
               worker_thread.get_stop_token().stop_requested();
    });
}

void Scheduler::Wait(u64 tick) {
    if (tick == 0) {
        return;
    }
    // Without this every wait falls through to WaitCommitted() below, and the callers
    // are hot enough to re-sync the whole pipeline.
    if (master_semaphore->IsFree(tick)) {
        return;
    }
    if (tick >= master_semaphore->CurrentTick()) {
        // Nothing has been submitted that would ever signal this tick.
        Flush();
    } else if (committed_tick.load(std::memory_order_acquire) < tick &&
               pending_signal_tick.load(std::memory_order_acquire) != 0) {
        // A carried tick sits below CurrentTick(), so the test above misses it.
        Flush();
    }
    WaitCommitted(tick);
    master_semaphore->Wait(tick);
}

void Scheduler::KeepAlive(NS::Object* object, const MTL::Allocation* allocation) {
    if (object == nullptr) {
        return;
    }
    object->retain();
    std::scoped_lock lock{keep_alive_mutex};
    // +1 tick margin (matching StagingBufferPool::TICK_MARGIN): a concurrent Flush()
    // between KeepAlive() and the following Record() can bump CurrentTick() by 1,
    // so stamping CurrentTick() + 1 guarantees residency stays pinned until that
    // submission also completes.
    keep_alive.push_back(PendingRelease{object, allocation, master_semaphore->CurrentTick() + 1});
}

void Scheduler::SweepKeepAlive() {
    std::scoped_lock lock{keep_alive_mutex};
    if (keep_alive.empty()) {
        return;
    }

    // Partition in O(N): collect allocations still in flight first, then remove
    // residency only for retired allocations not present in the in-flight set.
    const auto first_free = std::stable_partition(
        keep_alive.begin(), keep_alive.end(),
        [this](const PendingRelease& pending) { return !master_semaphore->IsFree(pending.tick); });
    if (first_free == keep_alive.end()) {
        return;
    }

    sweep_in_flight.clear();
    const size_t needed = static_cast<size_t>(first_free - keep_alive.begin()) * 2 + 8;
    if (sweep_in_flight.bucket_count() < needed) {
        sweep_in_flight.reserve(needed);
    }
    for (auto it = keep_alive.begin(); it != first_free; ++it) {
        if (it->allocation != nullptr) {
            sweep_in_flight.insert(it->allocation);
        }
    }
    for (auto it = first_free; it != keep_alive.end(); ++it) {
        if (it->allocation != nullptr && sweep_in_flight.insert(it->allocation).second) {
            device.RemoveResidentAllocation(it->allocation);
        }
        it->object->release();
    }
    keep_alive.erase(first_free, keep_alive.end());
    // Removals are published on MetalWorker at the next submission CommitResidency(),
    // avoiding a synchronous IOGPUResourceGroupUpdateResources kernel trap on the GPU thread.
}

namespace {
/// Adaptive spin window before sleeping (matches Vulkan #721): uses Common::ThreadPause
/// instead of std::this_thread::yield() to avoid hundreds of swtch_pri kernel traps.
constexpr auto WORKER_SPIN_WINDOW = std::chrono::microseconds{50};
} // Anonymous namespace

void Scheduler::WorkerThread(std::stop_token stop_token) {
    Common::SetCurrentThreadName("MetalWorker");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    CommandContext context{*this};
    u32 quick_idles = 0;

    const auto TryPopQueue{[this](auto& work) -> bool {
        if (work_queue.empty()) {
            return false;
        }
        work = std::move(work_queue.front());
        work_queue.pop();
        queued_chunks.fetch_sub(1, std::memory_order_relaxed);
        if (work_queue.empty() && worker_waiters.load(std::memory_order_relaxed) > 0) {
            event_cv.notify_all();
        }
        return true;
    }};

    while (!stop_token.stop_requested()) {
        std::unique_ptr<CommandChunk> work;

        // The recording thread hands over a chunk every few dozen draws. Sleeping in
        // between makes each hand-off a wake-up system call on that thread, so spin
        // briefly with ThreadPause (no syscall) while recent chunks arrived that fast.
        if (queued_chunks.load(std::memory_order_acquire) == 0) {
            const auto idle_start = std::chrono::steady_clock::now();
            if (std::popcount(quick_idles & 0xFU) >= 3) {
                const auto deadline = idle_start + WORKER_SPIN_WINDOW;
                while (queued_chunks.load(std::memory_order_acquire) == 0 &&
                       !stop_token.stop_requested()) {
                    for (u32 i = 0; i < 16; ++i) {
                        Common::ThreadPause();
                    }
                    if (std::chrono::steady_clock::now() >= deadline) {
                        break;
                    }
                }
            }
            if (queued_chunks.load(std::memory_order_acquire) == 0) {
                std::unique_lock lock{queue_mutex};
                if (!TryPopQueue(work)) {
                    worker_sleeping.store(true, std::memory_order_relaxed);
                    event_cv.wait(lock, stop_token, [&] { return TryPopQueue(work); });
                    worker_sleeping.store(false, std::memory_order_relaxed);
                }
                const bool quick =
                    std::chrono::steady_clock::now() - idle_start <= WORKER_SPIN_WINDOW;
                quick_idles = (quick_idles << 1) | (quick ? 1U : 0U);
                if (stop_token.stop_requested()) {
                    break;
                }
                static_cast<void>(std::exchange(lock, std::unique_lock{execution_mutex}));
                const bool has_submit = work->HasSubmit();
                {
                    ScopedAutoreleasePool pool;
                    work->ExecuteAll(context);
                }
                if (has_submit) {
                    AllocateNewCommandBuffer();
                }
                std::scoped_lock reserve_lock{reserve_mutex};
                constexpr size_t MAX_RESERVED_CHUNKS = 64;
                if (chunk_reserve.size() < MAX_RESERVED_CHUNKS) {
                    chunk_reserve.emplace_back(std::move(work));
                }
                continue;
            }
            quick_idles = (quick_idles << 1) | 1U;
        }

        {
            std::unique_lock lock{queue_mutex};
            if (!TryPopQueue(work)) {
                worker_sleeping.store(true, std::memory_order_relaxed);
                event_cv.wait(lock, stop_token, [&] { return TryPopQueue(work); });
                worker_sleeping.store(false, std::memory_order_relaxed);
            }
            if (stop_token.stop_requested()) {
                break;
            }

            // Take the execution lock before releasing the queue lock, so a
            // WaitWorker() seeing an empty queue can't race ahead of this chunk.
            static_cast<void>(std::exchange(lock, std::unique_lock{execution_mutex}));

            const bool has_submit = work->HasSubmit();

            {
                // Encoders come back autoreleased; the pool is per chunk so they
                // don't pile up for the life of the thread.
                ScopedAutoreleasePool pool;
                work->ExecuteAll(context);
            }

            if (has_submit) {
                AllocateNewCommandBuffer();
            }
        }

        std::scoped_lock reserve_lock{reserve_mutex};
        constexpr size_t MAX_RESERVED_CHUNKS = 64;
        if (chunk_reserve.size() < MAX_RESERVED_CHUNKS) {
            chunk_reserve.emplace_back(std::move(work));
        }
    }

    WorkerEndEncoding();
}

RenderEncoder Scheduler::WorkerRequestRender(const RenderPassState& state) {
    if (!mtl4_command_buffer || mtl4_committed) {
        return {};
    }
    // A spurious restart is a tile store plus reload on Apple hardware.
    if (encoder.type == EncoderType::Render && encoder.pass.IsCompatibleWith(state)) {
        // Orders vertex -> {vertex, fragment} and that's all it can do; real
        // fragment-side dependencies have to end the encoder instead.
        encoder.mtl4_render->barrierAfterEncoderStages(RENDER_ENCODER_AFTER_STAGES,
                                                       RENDER_ENCODER_STAGES,
                                                       MTL4::VisibilityOptionDevice);

        // The argument tables are scheduler-wide while this cache is per encoder,
        // so anything binding during the pass overwrites slots it thinks it owns.
        // Preserve vertex_queue_barrier_done across compatible re-entry of the same encoder.
        const bool had_vertex_barrier = encoder.render_cache.vertex_queue_barrier_done;
        encoder.render_cache = CommandContext::RenderStateCache{};
        encoder.render_cache.vertex_queue_barrier_done = had_vertex_barrier;

        RenderEncoder wrapper{encoder.mtl4_render.get()};
        wrapper.AttachArgumentTables(mtl4_vertex_table.get(), mtl4_fragment_table.get(),
                                     &mtl4_inline_rings[mtl4_allocator_index],
                                     &mtl4_null_bindings);
        return wrapper;
    }
    WorkerEndEncoding();

    MTL4::RenderPassDescriptor* mtl4_descriptor = BuildMTL4RenderPassDescriptor(state);
    encoder.mtl4_render = Retain(mtl4_command_buffer->renderCommandEncoder(mtl4_descriptor));
    if (!encoder.mtl4_render) {
        LOG_ERROR(Render_Metal, "Failed to create a Metal 4 render command encoder");
        return {};
    }
    encoder.type = EncoderType::Render;
    ++encoder.opened.render_encoders;
    encoder.pass = state;
    encoder.render_cache = CommandContext::RenderStateCache{};

    // Biggest target this submission writes. Pixel count is a rough proxy for
    // cost, but we have no per-encoder timing to do better.
    if (const u64 pixels = u64{state.render_area_width} * state.render_area_height;
        pixels > u64{encoder.opened.pass_width} * encoder.opened.pass_height) {
        encoder.opened.pass_width = state.render_area_width;
        encoder.opened.pass_height = state.render_area_height;
        encoder.opened.pass_samples = state.sample_count;
        encoder.opened.color_format =
            state.num_color > 0 && state.color[0].texture
                ? static_cast<u32>(state.color[0].texture->pixelFormat())
                : 0;
        encoder.opened.depth_format =
            state.depth_stencil.texture
                ? static_cast<u32>(state.depth_stencil.texture->pixelFormat())
                : 0;
    }

    // Metal 4 does NO hazard tracking, and a missed dependency is a silent
    // wrong-pixel bug, not a validation error. Fragment alone: naming Vertex kills
    // the tiling overlap on every one of ~160 encoder opens a frame. The gap is a
    // *vertex* shader sampling a texture a prior pass wrote; widen back to
    // RENDER_ENCODER_STAGES if it shows up.
    const bool barrier_covers_vertex =
        compute_or_blit_since_render_barrier || render_wrote_storage_since_vertex_barrier;
    const MTL::Stages consumers =
        barrier_covers_vertex ? RENDER_ENCODER_STAGES : MTL::StageFragment;
    compute_or_blit_since_render_barrier = false;
    if (barrier_covers_vertex) {
        render_pass_ended_since_vertex_barrier = false;
        render_wrote_storage_since_vertex_barrier = false;
    }
    encoder.mtl4_render->barrierAfterQueueStages(MTL::StageAll, consumers,
                                                 MTL4::VisibilityOptionDevice);
    encoder.render_cache.vertex_queue_barrier_done = barrier_covers_vertex;

    RenderEncoder wrapper{encoder.mtl4_render.get()};
    wrapper.SetArgumentTables(mtl4_vertex_table.get(), mtl4_fragment_table.get(),
                              &mtl4_inline_rings[mtl4_allocator_index], &mtl4_null_bindings);
    return wrapper;
}

ComputeEncoder Scheduler::WorkerRequestCompute() {
    if (!mtl4_command_buffer || mtl4_committed) {
        return {};
    }
    // A copy encoder IS a compute encoder, so an open one takes dispatches
    // directly. Ending it to open an identical encoder costs a queue-scope drain
    // for nothing (Legends Z-A interleaves ~340 of each a frame).
    if (encoder.type == EncoderType::Compute || encoder.type == EncoderType::Blit) {
        MTL4::ComputeCommandEncoder* const open = encoder.type == EncoderType::Compute
                                                      ? encoder.mtl4_compute.get()
                                                      : encoder.mtl4_blit.get();
        // Metal 4 does not order dispatches inside one compute encoder. On an M1 a
        // second dispatch reproducibly read the pre-write value of what the first
        // wrote. Serializes independent dispatches too, until per-resource tracking.
        open->barrierAfterEncoderStages(COMPUTE_ENCODER_STAGES, COMPUTE_ENCODER_STAGES,
                                        MTL4::VisibilityOptionDevice);
        // Only call setArgumentTable when switching from a copy/blit encoder that
        // never bound mtl4_compute_table; an already-open compute encoder has it bound.
        ComputeEncoder wrapper{open};
        if (encoder.type == EncoderType::Compute) {
            wrapper.AttachArgumentTable(mtl4_compute_table.get(),
                                        &mtl4_inline_rings[mtl4_allocator_index],
                                        &mtl4_null_bindings);
        } else {
            wrapper.SetArgumentTable(mtl4_compute_table.get(),
                                     &mtl4_inline_rings[mtl4_allocator_index], &mtl4_null_bindings);
        }
        return wrapper;
    }
    WorkerEndEncoding();
    encoder.mtl4_compute = Retain(mtl4_command_buffer->computeCommandEncoder());
    if (!encoder.mtl4_compute) {
        LOG_ERROR(Render_Metal, "Failed to create a Metal 4 compute command encoder");
        return {};
    }
    encoder.type = EncoderType::Compute;
    ++encoder.opened.compute_encoders;
    compute_or_blit_since_render_barrier = true;

    encoder.mtl4_compute->barrierAfterQueueStages(MTL::StageAll, COMPUTE_ENCODER_STAGES,
                                                  MTL4::VisibilityOptionDevice);

    ComputeEncoder wrapper{encoder.mtl4_compute.get()};
    wrapper.SetArgumentTable(mtl4_compute_table.get(),
                             &mtl4_inline_rings[mtl4_allocator_index], &mtl4_null_bindings);
    return wrapper;
}

BlitEncoder Scheduler::WorkerRequestBlit(bool barrier) {
    if (!mtl4_command_buffer || mtl4_committed) {
        return {};
    }
    if (encoder.type == EncoderType::Blit || encoder.type == EncoderType::Compute) {
        MTL4::ComputeCommandEncoder* const open = encoder.type == EncoderType::Blit
                                                      ? encoder.mtl4_blit.get()
                                                      : encoder.mtl4_compute.get();
        if (barrier || encoder.type == EncoderType::Compute) {
            open->barrierAfterEncoderStages(COMPUTE_ENCODER_STAGES, COMPUTE_ENCODER_STAGES,
                                            MTL4::VisibilityOptionDevice);
        }
        return BlitEncoder{open};
    }
    WorkerEndEncoding();
    encoder.mtl4_blit = Retain(mtl4_command_buffer->computeCommandEncoder());
    if (!encoder.mtl4_blit) {
        LOG_ERROR(Render_Metal, "Failed to create a Metal 4 copy (compute) encoder");
        return {};
    }
    encoder.type = EncoderType::Blit;
    ++encoder.opened.blit_encoders;
    compute_or_blit_since_render_barrier = true;

    encoder.mtl4_blit->barrierAfterQueueStages(MTL::StageAll, COMPUTE_ENCODER_STAGES,
                                               MTL4::VisibilityOptionDevice);

    return BlitEncoder{encoder.mtl4_blit.get()};
}

void Scheduler::WorkerEndEncoding() {
    // Residency belongs to the encoder: whatever was declared is forgotten here.

    switch (encoder.type) {
    case EncoderType::Render:
        if (encoder.mtl4_render) {
            encoder.mtl4_render->endEncoding();
        }
        encoder.mtl4_render = {};
        encoder.pass = {};
        render_pass_ended_since_vertex_barrier = true;
        break;
    case EncoderType::Compute:
        if (encoder.mtl4_compute) {
            encoder.mtl4_compute->endEncoding();
        }
        encoder.mtl4_compute = {};
        break;
    case EncoderType::Blit:
        if (encoder.mtl4_blit) {
            encoder.mtl4_blit->endEncoding();
        }
        encoder.mtl4_blit = {};
        break;
    case EncoderType::None:
        return;
    }
    encoder.type = EncoderType::None;

}

} // namespace Metal
