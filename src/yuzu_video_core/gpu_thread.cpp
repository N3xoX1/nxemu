// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
#include <chrono>
#include <unistd.h>
#include <vector>
#include "yuzu_common/android/performance_hint.h"
#endif

#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/scope_exit.h"
#include "yuzu_common/settings.h"
#include "yuzu_common/thread.h"
#include "frontend/graphics_context.h"
#include "yuzu_video_core/control/scheduler.h"
#include "yuzu_video_core/dma_pusher.h"
#include "yuzu_video_core/gpu.h"
#include "yuzu_video_core/gpu_thread.h"
#include "yuzu_video_core/renderer_base.h"
#include "video_settings.h"

namespace VideoCommon::GPUThread {

namespace {
constexpr size_t MaxWriteRanges = 1024;

#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
class GpuAdpfTracker final {
public:
    class ScopedWork final {
    public:
        explicit ScopedWork(GpuAdpfTracker& tracker_) : tracker{tracker_} {
            tracker.performance_hint.BeginWork();
        }
        ~ScopedWork() {
            tracker.performance_hint.EndWork();
        }
        ScopedWork(const ScopedWork&) = delete;
        ScopedWork& operator=(const ScopedWork&) = delete;

    private:
        GpuAdpfTracker& tracker;
    };

    GpuAdpfTracker(Tegra::GPU& gpu_, SynchState& state_) : gpu{gpu_}, state{state_} {
        state.adpf_active.store(performance_hint.IsAvailable(), std::memory_order_relaxed);
        gpu.SetFrameCompleteHook(
            [this](std::chrono::nanoseconds budget) { FrameComplete(budget); });
    }

    ~GpuAdpfTracker() {
        gpu.SetFrameCompleteHook({});
        state.adpf_active.store(false, std::memory_order_relaxed);
    }

    // Outside execution_mutex and after waking waiters: the hint call is a Binder IPC
    void ReportCompletedFrames() {
        for (const CompletedFrame& frame : completed_frames) {
            Report(frame);
        }
        completed_frames.clear();
    }

private:
    struct CompletedFrame {
        std::chrono::nanoseconds budget;
        std::chrono::nanoseconds draw_work;
        std::chrono::nanoseconds decode_work;
    };

    // A composite can run in the middle of a batch, so the frame's work ends here
    void FrameComplete(std::chrono::nanoseconds budget) {
        performance_hint.EndWork();
        completed_frames.push_back(
            {budget, performance_hint.TakeWorkDuration(),
             std::chrono::nanoseconds{state.decode_work_ns.exchange(0, std::memory_order_relaxed)}});
        performance_hint.BeginWork();
    }

    void Report(const CompletedFrame& frame) {
        if (frame.budget <= std::chrono::nanoseconds{}) {
            performance_hint.ReportFrameDuration({}, {});
            state.adpf_active.store(performance_hint.IsAvailable(), std::memory_order_relaxed);
            return;
        }
        if (decode_tid == 0) {
            decode_tid = state.decode_tid.load(std::memory_order_acquire);
            if (decode_tid > 0) {
                performance_hint.AddThread(decode_tid);
            }
        }
        std::chrono::nanoseconds actual = frame.draw_work;
        if (decode_tid > 0 && performance_hint.HasThread(decode_tid)) {
            actual = std::max(frame.draw_work, frame.decode_work);
        }
        if (actual <= std::chrono::nanoseconds{} ||
            (actual > std::chrono::milliseconds{250} && actual > frame.budget * 4)) {
            performance_hint.ReportFrameDuration({}, frame.budget);
        } else {
            performance_hint.ReportFrameDuration(actual, frame.budget);
        }
        state.adpf_active.store(performance_hint.IsAvailable(), std::memory_order_relaxed);
    }

    Tegra::GPU& gpu;
    SynchState& state;
    Common::Android::PerformanceHintSession performance_hint;
    std::vector<CompletedFrame> completed_frames;
    int32_t decode_tid{};
};

class DecodeWorkTimer final {
public:
    explicit DecodeWorkTimer(SynchState& state_)
        : state{state_}, active{state.adpf_active.load(std::memory_order_relaxed)} {
        if (active) {
            start = std::chrono::steady_clock::now();
            Common::Android::PerformanceHintWait::Take();
        }
    }
    ~DecodeWorkTimer() {
        if (!active) {
            return;
        }
        const std::chrono::nanoseconds elapsed =
            std::chrono::steady_clock::now() - start - Common::Android::PerformanceHintWait::Take();
        if (elapsed > std::chrono::nanoseconds{}) {
            state.decode_work_ns.fetch_add(elapsed.count(), std::memory_order_relaxed);
        }
    }
    DecodeWorkTimer(const DecodeWorkTimer&) = delete;
    DecodeWorkTimer& operator=(const DecodeWorkTimer&) = delete;

private:
    SynchState& state;
    const bool active;
    std::chrono::steady_clock::time_point start;
};
#endif
} // namespace

void BackQueue::Push(Work&& work, Writes writes) {
    if (!threaded) {
        work();
        return;
    }
    bool wake;
    {
        std::scoped_lock lock{mutex};
        items.push_back(std::move(work));
        ++pushed;
        if (writes == Writes::Anywhere) {
            anywhere_until_push = pushed;
        }
        pushed_spin.store(pushed, std::memory_order_release);
        // Wake the GPU thread only when it sleeps
        wake = back_waiting;
    }
    if (wake) {
        cv.notify_one();
    }
}

void BackQueue::WaitIdle() {
    if (!threaded) {
        return;
    }
    // Only the decode thread pushes, and the other callers hold the decode mutex
    const u64 target = pushed_spin.load(std::memory_order_relaxed);
    auto idle = [this, target] { return done_spin.load(std::memory_order_acquire) >= target; };
    if (!idle()) {
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
        const Common::Android::PerformanceHintWait hint_wait;
#endif
        std::unique_lock lock{mutex};
        idle_cv.wait(lock, [this] { return done == pushed || stopping; });
    }
    ClearWrites();
}

void BackQueue::AddWrites(const WriteRanges& ranges) {
    if (!threaded) {
        return;
    }
    // The ranges belong to the next push, which callers make right after adding them
    const u64 push = pushed_spin.load(std::memory_order_relaxed) + 1;
    for (const auto& [begin, end] : ranges) {
        // Merging into the last write keeps the push order, as it gets the latest push
        if (queued_writes.size() > first_write && begin <= queued_writes.back().end &&
            end >= queued_writes.back().begin) {
            QueuedWrite& last = queued_writes.back();
            last.begin = std::min(last.begin, begin);
            last.end = std::max(last.end, end);
            last.push = push;
        } else {
            queued_writes.push_back({begin, end, push});
        }
        writes_begin = std::min(writes_begin, begin);
        writes_end = std::max(writes_end, end);
        last_write_push = push;
    }
    if (queued_writes.size() - first_write <= MaxWriteRanges) {
        return;
    }
    DropFinishedWrites(run_spin.load(std::memory_order_acquire));
    if (queued_writes.size() - first_write <= MaxWriteRanges * 3 / 4) {
        return;
    }
    queued_writes.erase(queued_writes.begin(), queued_writes.begin() + first_write);
    first_write = 0;
    std::ranges::sort(queued_writes, {}, &QueuedWrite::begin);
    size_t last = 0;
    for (size_t i = 1; i < queued_writes.size(); ++i) {
        if (queued_writes[i].begin <= queued_writes[last].end) {
            queued_writes[last].end = std::max(queued_writes[last].end, queued_writes[i].end);
            queued_writes[last].push = std::max(queued_writes[last].push, queued_writes[i].push);
        } else {
            queued_writes[++last] = queued_writes[i];
        }
    }
    queued_writes.resize(last + 1);
    if (queued_writes.size() > MaxWriteRanges * 3 / 4) {
        ClearWrites();
        anywhere_until_push = push;
        return;
    }
    // Back in push order, so finished writes leave from the front
    std::ranges::stable_sort(queued_writes, {}, &QueuedWrite::push);
    write_bounds_stale = true;
}

bool BackQueue::MayWrite(DAddr begin, DAddr end) {
    const u64 finished = run_spin.load(std::memory_order_acquire);
    if (finished < anywhere_until_push) {
        return true;
    }
    DropFinishedWrites(finished);
    if (first_write == queued_writes.size() || end <= writes_begin || writes_end <= begin) {
        return false;
    }
    if (write_bounds_stale) {
        UpdateWriteBounds();
        if (end <= writes_begin || writes_end <= begin) {
            return false;
        }
    }
    return std::any_of(queued_writes.begin() + first_write, queued_writes.end(),
                       [begin, end](const QueuedWrite& write) {
                           return begin < write.end && end > write.begin;
                       });
}

bool BackQueue::MayWriteAny() {
    const u64 finished = run_spin.load(std::memory_order_acquire);
    if (finished < anywhere_until_push) {
        return true;
    }
    DropFinishedWrites(finished);
    return first_write != queued_writes.size();
}

void BackQueue::DropFinishedWrites(u64 finished) {
    if (finished >= last_write_push) {
        if (!queued_writes.empty()) {
            const u64 anywhere = anywhere_until_push;
            ClearWrites();
            anywhere_until_push = anywhere;
        }
        return;
    }
    // The last write has not finished, so this stops before the end
    const size_t first = first_write;
    while (queued_writes[first_write].push <= finished) {
        ++first_write;
    }
    if (first_write == first) {
        return;
    }
    write_bounds_stale = true;
    if (first_write >= 256 && first_write * 2 >= queued_writes.size()) {
        queued_writes.erase(queued_writes.begin(), queued_writes.begin() + first_write);
        first_write = 0;
    }
}

void BackQueue::ClearWrites() {
    queued_writes.clear();
    first_write = 0;
    last_write_push = 0;
    writes_begin = ~DAddr{};
    writes_end = 0;
    write_bounds_stale = false;
    anywhere_until_push = 0;
}

void BackQueue::UpdateWriteBounds() {
    writes_begin = ~DAddr{};
    writes_end = 0;
    for (size_t i = first_write; i < queued_writes.size(); ++i) {
        writes_begin = std::min(writes_begin, queued_writes[i].begin);
        writes_end = std::max(writes_end, queued_writes[i].end);
    }
    write_bounds_stale = false;
}

bool BackQueue::PopWait(std::deque<Work>& out, std::stop_token stop_token,
                        const std::atomic<bool>& wake, const std::atomic<bool>& service) {
    std::unique_lock lock{mutex};
    const auto woken = [&wake, &service] {
        return wake.load(std::memory_order_acquire) || service.load(std::memory_order_acquire);
    };
    if (items.empty() && !stopping && !woken()) {
        back_waiting = true;
        cv.wait(lock, stop_token, [this, &woken] { return !items.empty() || stopping || woken(); });
        back_waiting = false;
    }
    if (items.empty()) {
        return false;
    }
    // Take everything queued, so the GPU thread runs it under one execution lock
    std::swap(out, items);
    return true;
}

void BackQueue::MarkRun() {
    run_spin.store(run_spin.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

void BackQueue::FinishMany(u64 count) {
    bool idle;
    {
        std::scoped_lock lock{mutex};
        done += count;
        done_spin.store(done, std::memory_order_release);
        run_spin.store(done, std::memory_order_release);
        idle = done == pushed;
    }
    if (idle) {
        idle_cv.notify_all();
    }
}

void BackQueue::Notify() {
    {
        std::scoped_lock lock{mutex};
    }
    cv.notify_one();
}

void BackQueue::Stop() {
    {
        std::scoped_lock lock{mutex};
        stopping = true;
    }
    cv.notify_all();
    idle_cv.notify_all();
}

static void SignalFence(SynchState& state, u64 fence, bool block) {
    state.signaled_fence.store(fence);
    if (block) {
        // We have to lock the write_lock to ensure that the condition_variable wait not get a
        // race between the check and the lock itself.
        std::scoped_lock lk{state.write_lock};
        state.cv.notify_all();
    }
}

static void ServiceWaits(SynchState& state, VideoCore::RasterizerInterface* rasterizer) {
    // Cleared first: a request made after this wakes the thread again
    state.service_ready.store(false, std::memory_order_relaxed);
    rasterizer->ServiceInPlaceWaits();
}

/// GPU thread with a decode thread: runs the queued work
static void RunDrawThread(std::stop_token stop_token, Tegra::GPU& gpu,
                          VideoCore::RendererBase& renderer,
                          Core::Frontend::GraphicsContext& context, SynchState& state) {
    Common::SetCurrentThreadName("GPU");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    gpu.RegisterHostThread();
    VideoCore::gpu_thread_role = VideoCore::GpuThreadRole::Draw;

    Core::Frontend::GraphicsContext::Scoped current_context = context.Acquire();
    VideoCore::RasterizerInterface* const rasterizer = renderer.ReadRasterizer();
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
    GpuAdpfTracker adpf{gpu, state};
#endif
    std::deque<BackQueue::Work> batch;
    while (!stop_token.stop_requested()) {
        {
            std::scoped_lock execution_lock{state.execution_mutex};
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
            const GpuAdpfTracker::ScopedWork tracked_work{adpf};
#endif
            ServiceWaits(state, rasterizer);
            gpu.ProcessPendingComposite();
        }
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
        adpf.ReportCompletedFrames();
#endif
        if (!state.back.PopWait(batch, stop_token, state.composite_ready, state.service_ready)) {
            continue;
        }
        const u64 count = batch.size();
        {
            std::scoped_lock execution_lock{state.execution_mutex};
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
            const GpuAdpfTracker::ScopedWork tracked_work{adpf};
#endif
            for (BackQueue::Work& work : batch) {
                // On exit the guest memory is unmapped before the GPU stops
                if (stop_token.stop_requested()) {
                    break;
                }
                work();
                rasterizer->ReleaseDrawLocks();
                state.back.MarkRun();
                rasterizer->ServiceInPlaceWaits();
            }
            batch.clear();
            gpu.ProcessPendingComposite();
        }
        state.back.FinishMany(count);
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
        adpf.ReportCompletedFrames();
#endif
    }
}

/// Decode thread: runs DmaPusher and the decode side of the engines
static void RunDecodeThread(std::stop_token stop_token, Tegra::GPU& gpu,
                            VideoCore::RasterizerInterface* rasterizer,
                            Tegra::Control::Scheduler& scheduler, SynchState& state) {
    Common::SetCurrentThreadName("GPUDecode");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    gpu.RegisterHostThread();
    VideoCore::gpu_thread_role = VideoCore::GpuThreadRole::Decode;
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
    state.decode_tid.store(static_cast<int32_t>(gettid()), std::memory_order_release);
#endif

    CommandDataContainer next;
    while (!stop_token.stop_requested()) {
        if (!state.queue.PopWait(next, stop_token, [] { return false; })) {
            continue;
        }
        if (stop_token.stop_requested()) {
            break;
        }
        std::scoped_lock decode_lock{state.decode_mutex};
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
        DecodeWorkTimer work_timer{state};
#endif
        if (SubmitListCommand* submit_list = std::get_if<SubmitListCommand>(&next.data)) {
            scheduler.Push(submit_list->channel, std::move(submit_list->entries));
        } else if (std::holds_alternative<GPUTickCommand>(next.data)) {
            state.back.Push([&gpu] { gpu.TickWork(); });
        } else if (const FlushRegionCommand* flush = std::get_if<FlushRegionCommand>(&next.data)) {
            state.back.Push([rasterizer, addr = flush->addr, size = flush->size] {
                rasterizer->FlushRegion(addr, size);
            });
        } else if (const InvalidateRegionCommand* invalidate =
                       std::get_if<InvalidateRegionCommand>(&next.data)) {
            state.back.Push([rasterizer, addr = invalidate->addr, size = invalidate->size] {
                rasterizer->OnCacheInvalidation(addr, size);
            });
        } else {
            ASSERT(false);
        }
        state.back.Push(
            [&state, fence = next.fence, block = next.block] { SignalFence(state, fence, block); },
            BackQueue::Writes::Nothing);
    }
}

/// Runs the GPU thread
static void RunThread(std::stop_token stop_token, Tegra::GPU & gpu, VideoCore::RendererBase & renderer, Core::Frontend::GraphicsContext & context, Tegra::Control::Scheduler & scheduler, SynchState & state)
{
    Common::SetCurrentThreadName("GPU");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    gpu.RegisterHostThread();
    VideoCore::gpu_thread_role = VideoCore::GpuThreadRole::Draw;

    auto current_context = context.Acquire();
    VideoCore::RasterizerInterface* const rasterizer = renderer.ReadRasterizer();
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
    GpuAdpfTracker adpf{gpu, state};
#endif

    CommandDataContainer next;

    while (!stop_token.stop_requested())
    {
        {
            std::scoped_lock execution_lock{state.execution_mutex};
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
            const GpuAdpfTracker::ScopedWork tracked_work{adpf};
#endif
            ServiceWaits(state, rasterizer);
            gpu.ProcessPendingComposite();
        }
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
        adpf.ReportCompletedFrames();
#endif
        const bool has_command = state.queue.PopWait(next, stop_token, [&] {
            return state.composite_ready.load(std::memory_order_acquire) ||
                   state.service_ready.load(std::memory_order_acquire);
        });
        if (!has_command && !stop_token.stop_requested()) {
            {
                std::scoped_lock execution_lock{state.execution_mutex};
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
                const GpuAdpfTracker::ScopedWork tracked_work{adpf};
#endif
                gpu.ProcessPendingComposite();
            }
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
            adpf.ReportCompletedFrames();
#endif
            continue;
        }
        if (stop_token.stop_requested())
        {
            break;
        }
        {
            std::scoped_lock execution_lock{state.execution_mutex};
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
            const GpuAdpfTracker::ScopedWork tracked_work{adpf};
#endif
            if (auto * submit_list = std::get_if<SubmitListCommand>(&next.data))
            {
                scheduler.Push(submit_list->channel, std::move(submit_list->entries));
            }
            else if (std::holds_alternative<GPUTickCommand>(next.data))
            {
                gpu.TickWork();
            }
            else if (const auto * flush = std::get_if<FlushRegionCommand>(&next.data))
            {
                rasterizer->FlushRegion(flush->addr, flush->size);
            }
            else if (const auto * invalidate = std::get_if<InvalidateRegionCommand>(&next.data))
            {
                rasterizer->OnCacheInvalidation(invalidate->addr, invalidate->size);
            }
            else
            {
                ASSERT(false);
            }
            rasterizer->ReleaseDrawLocks();
            gpu.ProcessPendingComposite();
        }
        state.signaled_fence.store(next.fence);
        if (next.block)
        {
            // We have to lock the write_lock to ensure that the condition_variable wait not get a
            // race between the check and the lock itself.
            std::scoped_lock lk{state.write_lock};
            state.cv.notify_all();
        }
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
        adpf.ReportCompletedFrames();
#endif
    }
}

ThreadManager::ThreadManager(Tegra::GPU & gpu_, bool is_async_) :
    gpu{gpu_},
    is_async{is_async_}
{
}

ThreadManager::~ThreadManager() = default;

void ThreadManager::StartThread(VideoCore::RendererBase & renderer, Core::Frontend::GraphicsContext & context, Tegra::Control::Scheduler & scheduler)
{
    {
        std::scoped_lock lk{state->write_lock};
        state->stopping = false;
    }
    state->queue.ResetClosed();
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
    state->decode_tid.store(0, std::memory_order_relaxed);
    state->decode_work_ns.store(0, std::memory_order_relaxed);
    state->adpf_active.store(false, std::memory_order_relaxed);
#endif
    rasterizer = renderer.ReadRasterizer();
    if (is_async)
    {
        state->back.threaded = true;
        thread = std::jthread(RunDrawThread, std::ref(gpu), std::ref(renderer), std::ref(context), std::ref(*state));
        decode_thread = std::jthread(RunDecodeThread, std::ref(gpu), rasterizer, std::ref(scheduler), std::ref(*state));
        return;
    }
    thread = std::jthread(RunThread, std::ref(gpu), std::ref(renderer), std::ref(context), std::ref(scheduler), std::ref(*state));
}

Tegra::ExecutionLock ThreadManager::LockExecution()
{
    Tegra::ExecutionLock lock;
    lock.decode = std::unique_lock{state->decode_mutex};
    // Queued work still uses the current mappings
    state->back.WaitIdle();
    lock.execution = std::unique_lock{state->execution_mutex};
    return lock;
}

void ThreadManager::StopThread()
{
    if (rasterizer != nullptr)
    {
        rasterizer->StopInPlaceWaits();
    }
    // Wake any producer blocked on a full command queue before acquiring write_lock
    // or stopping the consumer thread.
    state->queue.Close();
    {
        std::scoped_lock lk{state->write_lock};
        state->stopping = true;
    }
    state->cv.notify_all();
    if (decode_thread.joinable())
    {
        // The GPU thread keeps running the queued work the decode thread may be waiting on
        decode_thread.request_stop();
        decode_thread.join();
        thread.request_stop();
        state->back.Stop();
    }
    if (thread.joinable())
    {
        thread.request_stop();
        thread.join();
    }
}

void ThreadManager::SubmitList(s32 channel, Tegra::CommandList && entries)
{
    PushCommand(SubmitListCommand(channel, std::move(entries)));
}

void ThreadManager::FlushRegion(DAddr addr, u64 size)
{
    if (!is_async)
    {
        // Always flush with synchronous GPU mode
        PushCommand(FlushRegionCommand(addr, size));
        return;
    }
    if (!Settings::IsGPULevelExtreme())
    {
        return;
    }
    u64 fence = gpu.RequestFlush(addr, size);
    TickGPU();
    gpu.WaitForSyncOperation(fence);
}

void ThreadManager::TickGPU()
{
    PushCommand(GPUTickCommand());
}

bool ThreadManager::IsIdle() const
{
    std::scoped_lock lk{state->write_lock};
    return state->signaled_fence.load(std::memory_order_acquire) == state->last_fence;
}

void ThreadManager::InvalidateRegion(DAddr addr, u64 size)
{
    rasterizer->OnCacheInvalidation(addr, size);
}

void ThreadManager::FlushAndInvalidateRegion(DAddr addr, u64 size)
{
    // Skip flush on asynch mode, as FlushAndInvalidateRegion is not used for anything too important
    rasterizer->OnCacheInvalidation(addr, size);
}

u64 ThreadManager::PushCommand(CommandData && command_data, bool block)
{
    if (!is_async)
    {
        // In synchronous GPU mode, block the caller until the command has executed
        block = true;
    }

    std::unique_lock lk(state->write_lock);
    if (state->stopping)
    {
        return state->last_fence;
    }
    const u64 fence{++state->last_fence};
    if (!state->queue.EmplaceWait(std::move(command_data), fence, block))
    {
        return fence;
    }

    if (block)
    {
        state->cv.wait(lk, [this, fence] {
            return state->stopping || fence <= state->signaled_fence.load(std::memory_order_relaxed);
        });
    }

    return fence;
}

} // namespace VideoCommon::GPUThread
