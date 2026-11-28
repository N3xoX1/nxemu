// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include "yuzu_video_core/metal_common/metal_wrapper.h"

namespace Metal {

class Device;

/// What a submission held, so a fault report can say what it was writing into
/// and not just that one happened.
struct SubmissionContents {
    u32 render_encoders{};
    u32 compute_encoders{};
    u32 blit_encoders{};

    /// Largest render target the submission wrote, standing in for its most
    /// expensive pass. We get no per-encoder timing, MTL4 counter heaps fault on
    /// AGX G13. Width is zero if it opened no render encoder, formats are
    /// MTL::PixelFormat values.
    u32 pass_width{};
    u32 pass_height{};
    u32 pass_samples{1};
    u32 color_format{};
    u32 depth_format{};
};

/// One-line description of the render target in @p contents, e.g.
/// "1920x1080 BGRA8+D32FS8". Shared with the scheduler's encoder report so both
/// name a target the same way.
[[nodiscard]] std::string DescribeTarget(const SubmissionContents& contents);

/// Tracks GPU progress as a monotonically increasing tick, backed by an
/// MTLSharedEvent. Ticks are handed out by NextTick() and signalled by the GPU
/// in submission order, so reaching a tick implies every earlier one is done.
class MasterSemaphore : Common::NonCopyable, Common::NonMoveable {
public:
    explicit MasterSemaphore(const Device& device);
    ~MasterSemaphore();

    /// The tick that will be assigned to the next submission.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return current_tick.load(std::memory_order_acquire);
    }

    /// The most recent tick we know the GPU has passed. May lag reality until
    /// Refresh() is called. Never reports a tick the GPU has not reached.
    [[nodiscard]] u64 KnownGpuTick() const noexcept {
        return gpu_tick.load(std::memory_order_acquire);
    }

    /// Whether the GPU has finished the work associated with a tick.
    [[nodiscard]] bool IsFree(u64 tick) const noexcept {
        return KnownGpuTick() >= tick;
    }

    /// How many submissions the GPU watchdog has killed. Each one discarded the
    /// work it held, so the frame is torn as well as late.
    [[nodiscard]] u32 TimeoutCount() const noexcept {
        return timeouts.load(std::memory_order_relaxed);
    }

    /// True once the queue has refused enough submissions in a row that it will not
    /// run anything again. Metal says Code=5 "Ignored (for causing prior/excessive
    /// GPU errors)" with a 0.00 ms run time, and it is terminal, the queue has to be
    /// rebuilt. A healthy queue never ignores one at all. Consuming clears the streak.
    [[nodiscard]] bool ConsumeDeadQueue() noexcept {
        // Clear only on a hit. This runs once per commit, so exchanging every time
        // reset the streak faster than refusals piled up and recovery never fired.
        if (ignored_streak.load(std::memory_order_acquire) < DEAD_QUEUE_STREAK) {
            return false;
        }
        ignored_streak.store(0, std::memory_order_release);
        return true;
    }

    /// Installed by the renderer, called from Wait once a tick has been stuck long
    /// enough to count as a hang. Runs on whichever thread is blocked, which is the
    /// point, a queue stuck on an unpresented drawable never lets the worker reach
    /// the code that would repair it. Must be safe to call repeatedly, any thread.
    void SetStallHandler(std::function<void()> handler) {
        on_stall = std::move(handler);
    }

    /// True once per page fault, for the submitter to react to. A fault is not
    /// always fatal, the same three faults give zero `SubmissionsIgnored` and no
    /// hang when the queue is shallow, but take 32 neighbours down and kill the
    /// queue when it is deep. That is submission pressure, so the submitter drains
    /// instead of piling more work on.
    [[nodiscard]] bool ConsumeNewFault() noexcept {
        const u32 seen = faults.load(std::memory_order_acquire);
        return acknowledged_faults.exchange(seen, std::memory_order_acq_rel) != seen;
    }

    /// Claims the current tick and advances the counter.
    [[nodiscard]] u64 NextTick() noexcept {
        return current_tick.fetch_add(1, std::memory_order_release);
    }

    /// Polls the shared event and updates the known GPU tick. Cheap, safe to call
    /// from any thread.
    void Refresh();

    /// Blocks until the GPU reaches @p tick.
    void Wait(u64 tick);

    /// Closes @p command_buffer, commits it (optionally preceded by @p upload_buffer
    /// in the same queue commit), and signals @p host_tick after it.
    /// @p carried_tick is a tick that never got a command buffer of its own and
    /// must be signalled along with this one; zero when there is none.
    void SignalAndCommitMTL4(MTL4::CommandQueue* queue, MTL4::CommandBuffer* command_buffer,
                             u64 host_tick, u64 carried_tick = 0, SubmissionContents contents = {},
                             MTL4::CommandBuffer* upload_buffer = nullptr);

    [[nodiscard]] MTL::SharedEvent* GetEvent() const noexcept {
        return event.get();
    }

private:
    /// Monotonically raises gpu_tick to @p value. Concurrent Refresh() calls
    /// from different threads can observe values out of order, so never let it
    /// move backwards.
    void AdvanceGpuTick(u64 value) noexcept;

    Owned<MTL::SharedEvent> event;

    std::function<void()> on_stall;   ///< See SetStallHandler.
    std::atomic<u32> timeouts{0};     ///< Submissions the GPU watchdog killed.
    std::atomic<u32> faults{0};       ///< Submissions killed by a GPU page fault.
    /// Faults the submitter has already reacted to; see ConsumeNewFault.
    std::atomic<u32> acknowledged_faults{0};
    /// Consecutive submissions the queue refused outright. Reset by any
    /// submission that runs, so a lone fault does not trip recovery.
    std::atomic<u32> ignored_streak{0};
    /// Low enough to rebuild a frame or two after the queue gives up. A burst of
    /// neighbours killed by one fault will not qualify, those arrive together and
    /// are followed by submissions that run.
    static constexpr u32 DEAD_QUEUE_STREAK = 16;
    std::atomic<u64> gpu_tick{0};     ///< Highest tick known to be complete.
    std::atomic<u64> current_tick{1}; ///< Next tick to hand out.
    std::mutex callback_mutex;
    std::condition_variable callback_cv;
    std::atomic<u32> pending_callbacks{0}; ///< In-flight MTL4 commit feedback handlers.
    std::atomic<bool> shutting_down{false};
};

} // namespace Metal
