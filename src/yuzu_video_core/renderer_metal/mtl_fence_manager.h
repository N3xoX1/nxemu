// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <memory>

#include "yuzu_video_core/fence_manager.h"
#include "yuzu_video_core/renderer_metal/mtl_buffer_cache.h"
#include "yuzu_video_core/renderer_metal/mtl_texture_cache.h"

namespace Tegra {
class GPU;
}

namespace VideoCore {
class RasterizerInterface;
}

namespace Metal {

class Scheduler;

/// A fence over a scheduler tick. A stubbed fence records nothing and reports
/// signalled immediately, which is how FenceManager avoids submitting for most
/// guest fence signals.
class InnerFence : public VideoCommon::FenceBase {
public:
    explicit InnerFence(Scheduler& scheduler, bool is_stubbed);
    ~InnerFence();

    void Queue();

    [[nodiscard]] bool IsSignaled() const;

    void Wait();

private:
    Scheduler& scheduler;
    u64 wait_tick = 0;
};

using Fence = std::shared_ptr<InnerFence>;

/// Lightweight query-cache bridge for FenceManager. Tracks pending occlusion queries
/// that still need a real GPU fence to submit and/or wait for completion before
/// ResolveOcclusionQueries(false) runs.
class NullQueryCache {
public:
    void BindOcclusionState(const Scheduler* scheduler_,
                            const std::atomic<u64>* last_occlusion_tick_,
                            const std::atomic<u32>* pending_occlusion_count_,
                            std::atomic<bool>* occlusion_needs_fence_) noexcept {
        scheduler = scheduler_;
        last_occlusion_tick = last_occlusion_tick_;
        pending_occlusion_count = pending_occlusion_count_;
        occlusion_needs_fence = occlusion_needs_fence_;
    }

    [[nodiscard]] bool ShouldWaitAsyncFlushes() const noexcept {
        return false;
    }
    [[nodiscard]] bool HasUncommittedFlushes() const noexcept;
    [[nodiscard]] bool HasUnfencedReports() const noexcept {
        return HasUncommittedFlushes();
    }
    static void PrepareAsyncFlushes() {}
    static void PopAsyncFlushes() {}
    void CommitAsyncFlushes() noexcept;

private:
    const Scheduler* scheduler{};
    const std::atomic<u64>* last_occlusion_tick{};
    const std::atomic<u32>* pending_occlusion_count{};
    std::atomic<bool>* occlusion_needs_fence{};
};

struct FenceManagerParams {
    using FenceType = Fence;
    using BufferCacheType = BufferCache;
    using TextureCacheType = TextureCache;
    using QueryCacheType = NullQueryCache;

    static constexpr bool HAS_ASYNC_CHECK = true;
};

using GenericFenceManager = VideoCommon::FenceManager<FenceManagerParams>;

class FenceManager final : public GenericFenceManager {
public:
    explicit FenceManager(VideoCore::RasterizerInterface& rasterizer, Tegra::GPU& gpu,
                          TextureCache& texture_cache, BufferCache& buffer_cache,
                          NullQueryCache& query_cache, Scheduler& scheduler);

protected:
    Fence CreateFence(bool is_stubbed) override;
    void QueueFence(Fence& fence) override;
    bool IsFenceSignaled(Fence& fence) const override;
    void WaitFence(Fence& fence) override;

private:
    Scheduler& scheduler;
};

} // namespace Metal
