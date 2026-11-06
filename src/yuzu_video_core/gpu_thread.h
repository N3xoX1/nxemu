// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>

#include "yuzu_common/bounded_threadsafe_queue.h"
#include "yuzu_common/polyfill_thread.h"
#include "yuzu_video_core/framebuffer_config.h"
#include "yuzu_video_core/dma_pusher.h"
#include "yuzu_video_core/gpu.h"

namespace Tegra {
struct FramebufferConfig;
namespace Control {
class Scheduler;
}
} // namespace Tegra

namespace Core {
namespace Frontend {
class GraphicsContext;
}
class System;
} // namespace Core

namespace VideoCore {
class RasterizerInterface;
class RendererBase;
} // namespace VideoCore

namespace VideoCommon::GPUThread {

/// Command to signal to the GPU thread that a command list is ready for processing
struct SubmitListCommand final {
    explicit SubmitListCommand(s32 channel_, Tegra::CommandList&& entries_)
        : channel{channel_}, entries{std::move(entries_)} {}

    s32 channel;
    Tegra::CommandList entries;
};

/// Command to signal to the GPU thread to flush a region
struct FlushRegionCommand final {
    explicit constexpr FlushRegionCommand(DAddr addr_, u64 size_) : addr{addr_}, size{size_} {}

    DAddr addr;
    u64 size;
};

/// Command to signal to the GPU thread to invalidate a region
struct InvalidateRegionCommand final {
    explicit constexpr InvalidateRegionCommand(DAddr addr_, u64 size_) : addr{addr_}, size{size_} {}

    DAddr addr;
    u64 size;
};

/// Command to signal to the GPU thread to flush and invalidate a region
struct FlushAndInvalidateRegionCommand final {
    explicit constexpr FlushAndInvalidateRegionCommand(DAddr addr_, u64 size_)
        : addr{addr_}, size{size_} {}

    DAddr addr;
    u64 size;
};

/// Command to make the gpu look into pending requests
struct GPUTickCommand final {};

using CommandData =
    std::variant<std::monostate, SubmitListCommand, FlushRegionCommand, InvalidateRegionCommand,
                 FlushAndInvalidateRegionCommand, GPUTickCommand>;

struct CommandDataContainer {
    CommandDataContainer() = default;

    explicit CommandDataContainer(CommandData&& data_, u64 next_fence_, bool block_)
        : data{std::move(data_)}, fence{next_fence_}, block(block_) {}

    CommandData data;
    u64 fence{};
    bool block{};
};

/**
 * Work queue from the decode thread to the GPU thread, see DESIGN-gpu-split.md. The decode
 * thread runs DmaPusher and the Maxwell3D decode side, everything that touches the rasterizer
 * runs on the GPU thread in queue order. Without the decode thread the work runs at once.
 */
class BackQueue final {
public:
    using Work = std::function<void()>;

    bool IsThreaded() const {
        return threaded;
    }

    /// Decode thread: queues work. Runs it at once without the decode thread.
    void Push(Work&& work);

    /// Decode thread: waits until the GPU thread has run all queued work
    void WaitIdle();

    /// Decode thread: runs `func` while the GPU thread is idle and can't start new work
    template <typename Func>
    void RunExclusive(Func&& func) {
        if (!threaded) {
            func();
            return;
        }
        WaitIdle();
        std::scoped_lock lock{*execution_mutex};
        func();
    }

    // Used by ThreadManager and the GPU thread

    /// GPU thread: waits for work and takes all queued items, false
    /// when woken without work
    bool PopWait(std::deque<Work>& out, std::stop_token stop_token, const std::atomic<bool>& wake);
    void FinishMany(u64 count);
    void Notify();
    void Stop();

private:
    friend class ThreadManager;

    bool threaded{};
    std::recursive_mutex* execution_mutex{};
    std::mutex mutex;
    std::condition_variable_any cv;
    std::condition_variable_any idle_cv;
    std::deque<Work> items;
    u64 pushed{};
    u64 done{};
    bool stopping{};
    /// The GPU thread sleeps in PopWait, under `mutex`
    bool back_waiting{};
    /// Mirrors of the counters, so WaitIdle can return without the lock
    std::atomic<u64> pushed_spin{};
    std::atomic<u64> done_spin{};
};

/// Struct used to synchronize the GPU thread
struct SynchState final {
    using CommandQueue = Common::MPSCQueue<CommandDataContainer>;
    // Mapping notifications may download images and record renderer commands.
    // They must not race the GPU command/composite producer.
    std::recursive_mutex execution_mutex;
    /// Held by the decode thread while it runs a command
    std::recursive_mutex decode_mutex;
    BackQueue back;
    std::mutex write_lock;
    CommandQueue queue;
    u64 last_fence{};
    std::atomic<u64> signaled_fence{};
    std::condition_variable_any cv;
    std::atomic<bool> composite_ready{};

    void NotifyComposite() {
        if (!composite_ready.exchange(true, std::memory_order_release)) {
            if (back.IsThreaded()) {
                back.Notify();
            } else {
                queue.NotifyConsumer();
            }
        }
    }
};

/// Class used to manage the GPU thread
class ThreadManager final {
public:
    explicit ThreadManager(Tegra::GPU & gpu_, bool is_async_);
    ~ThreadManager();

    /// Creates and starts the GPU thread.
    void StartThread(VideoCore::RendererBase& renderer, Core::Frontend::GraphicsContext& context,
                     Tegra::Control::Scheduler& scheduler);

    /// Stops and joins the GPU thread, if it was started.
    void StopThread();

    /// Push GPU command entries to be processed
    void SubmitList(s32 channel, Tegra::CommandList&& entries);

    /// Notify rasterizer that any caches of the specified region should be flushed to Switch memory
    void FlushRegion(DAddr addr, u64 size);

    /// Notify rasterizer that any caches of the specified region should be invalidated
    void InvalidateRegion(DAddr addr, u64 size);

    /// Notify rasterizer that any caches of the specified region should be flushed and invalidated
    void FlushAndInvalidateRegion(DAddr addr, u64 size);

    void TickGPU();

    /// Returns true when the GPU thread has executed every command pushed so far
    bool IsIdle() const;

    /// Excludes the decode thread and the GPU thread, after the GPU thread has run all the
    /// work the decode thread queued.
    Tegra::ExecutionLock LockExecution();

    BackQueue& Back() {
        return state->back;
    }

    std::weak_ptr<SynchState> Wakeup() const { return state; }

    bool TakeCompositeReady() {
        return state->composite_ready.exchange(false, std::memory_order_acquire);
    }

private:
    /// Pushes a command to be executed by the GPU thread
    u64 PushCommand(CommandData&& command_data, bool block = false);

    Tegra::GPU & gpu;
    const bool is_async;
    VideoCore::RasterizerInterface* rasterizer = nullptr;

    std::shared_ptr<SynchState> state{std::make_shared<SynchState>()};
    std::jthread thread;
    std::jthread decode_thread;
};

} // namespace VideoCommon::GPUThread
