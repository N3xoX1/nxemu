// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

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

void BackQueue::Push(Work&& work) {
    if (!threaded) {
        work();
        return;
    }
    bool wake;
    {
        std::scoped_lock lock{mutex};
        items.push_back(std::move(work));
        ++pushed;
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
    // Only this thread pushes, so `target` is every item queued so far. The acquire load
    // pairs with the release store in FinishMany: the GPU thread's writes are visible.
    const u64 target = pushed_spin.load(std::memory_order_relaxed);
    auto idle = [this, target] { return done_spin.load(std::memory_order_acquire) >= target; };
    if (idle()) {
        return;
    }
    std::unique_lock lock{mutex};
    idle_cv.wait(lock, [this] { return done == pushed || stopping; });
}

bool BackQueue::PopWait(std::deque<Work>& out, std::stop_token stop_token,
                        const std::atomic<bool>& wake) {
    std::unique_lock lock{mutex};
    if (items.empty() && !stopping && !wake.load(std::memory_order_acquire)) {
        back_waiting = true;
        cv.wait(lock, stop_token, [this, &wake] {
            return !items.empty() || stopping || wake.load(std::memory_order_acquire);
        });
        back_waiting = false;
    }
    if (items.empty()) {
        return false;
    }
    // Take everything queued, so the GPU thread runs it under one execution lock
    std::swap(out, items);
    return true;
}

void BackQueue::FinishMany(u64 count) {
    bool idle;
    {
        std::scoped_lock lock{mutex};
        done += count;
        done_spin.store(done, std::memory_order_release);
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

/// GPU thread with a decode thread: runs the queued work
static void RunDrawThread(std::stop_token stop_token, Tegra::GPU& gpu,
                          VideoCore::RendererBase& renderer,
                          Core::Frontend::GraphicsContext& context, SynchState& state) {
    Common::SetCurrentThreadName("GPU");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    gpu.RegisterHostThread();

    auto current_context = context.Acquire();
    std::deque<BackQueue::Work> batch;
    while (!stop_token.stop_requested()) {
        {
            std::scoped_lock execution_lock{state.execution_mutex};
            gpu.ProcessPendingComposite();
        }
        if (!state.back.PopWait(batch, stop_token, state.composite_ready)) {
            continue;
        }
        const u64 count = batch.size();
        {
            std::scoped_lock execution_lock{state.execution_mutex};
            VideoCore::RasterizerInterface* const rasterizer = renderer.ReadRasterizer();
            for (BackQueue::Work& work : batch) {
                work();
                rasterizer->ReleaseDrawLocks();
            }
            batch.clear();
            gpu.ProcessPendingComposite();
        }
        state.back.FinishMany(count);
    }
}

/// Decode thread: runs DmaPusher and the decode side of the engines
static void RunDecodeThread(std::stop_token stop_token, Tegra::GPU& gpu,
                            VideoCore::RasterizerInterface* rasterizer,
                            Tegra::Control::Scheduler& scheduler, SynchState& state) {
    Common::SetCurrentThreadName("GPUDecode");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    gpu.RegisterHostThread();

    CommandDataContainer next;
    while (!stop_token.stop_requested()) {
        if (!state.queue.PopWait(next, stop_token, [] { return false; })) {
            continue;
        }
        if (stop_token.stop_requested()) {
            break;
        }
        std::scoped_lock decode_lock{state.decode_mutex};
        if (auto* submit_list = std::get_if<SubmitListCommand>(&next.data)) {
            scheduler.Push(submit_list->channel, std::move(submit_list->entries));
        } else if (std::holds_alternative<GPUTickCommand>(next.data)) {
            state.back.Push([&gpu] { gpu.TickWork(); });
        } else if (const auto* flush = std::get_if<FlushRegionCommand>(&next.data)) {
            state.back.Push([rasterizer, addr = flush->addr, size = flush->size] {
                rasterizer->FlushRegion(addr, size);
            });
        } else if (const auto* invalidate = std::get_if<InvalidateRegionCommand>(&next.data)) {
            state.back.Push([rasterizer, addr = invalidate->addr, size = invalidate->size] {
                rasterizer->OnCacheInvalidation(addr, size);
            });
        } else {
            ASSERT(false);
        }
        state.back.Push([&state, fence = next.fence, block = next.block] {
            SignalFence(state, fence, block);
        });
    }
}

/// Runs the GPU thread
static void RunThread(std::stop_token stop_token, Tegra::GPU & gpu, VideoCore::RendererBase & renderer, Core::Frontend::GraphicsContext & context, Tegra::Control::Scheduler & scheduler, SynchState & state)
{
    Common::SetCurrentThreadName("GPU");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    gpu.RegisterHostThread();

    auto current_context = context.Acquire();
    VideoCore::RasterizerInterface* const rasterizer = renderer.ReadRasterizer();

    CommandDataContainer next;

    while (!stop_token.stop_requested())
    {
        {
            std::scoped_lock execution_lock{state.execution_mutex};
            gpu.ProcessPendingComposite();
        }
        const bool has_command = state.queue.PopWait(next, stop_token, [&] {
            return state.composite_ready.load(std::memory_order_acquire);
        });
        if (!has_command && !stop_token.stop_requested()) {
            std::scoped_lock execution_lock{state.execution_mutex};
            gpu.ProcessPendingComposite();
            continue;
        }
        if (stop_token.stop_requested())
        {
            break;
        }
        {
            std::scoped_lock execution_lock{state.execution_mutex};
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
    rasterizer = renderer.ReadRasterizer();
    if (is_async)
    {
        state->back.threaded = true;
        state->back.execution_mutex = &state->execution_mutex;
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
    if (decode_thread.joinable())
    {
        decode_thread.request_stop();
        thread.request_stop();
        state->back.Stop();
        decode_thread.join();
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
    const u64 fence{++state->last_fence};
    state->queue.EmplaceWait(std::move(command_data), fence, block);

    if (block)
    {
        Common::CondvarWait(state->cv, lk, thread.get_stop_token(), [this, fence] {
            return fence <= state->signaled_fence.load(std::memory_order_relaxed);
        });
    }

    return fence;
}

} // namespace VideoCommon::GPUThread
