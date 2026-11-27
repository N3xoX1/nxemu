// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>

#include "yuzu_video_core/renderer_metal/mtl_fence_manager.h"
#include "yuzu_video_core/renderer_metal/mtl_scheduler.h"

namespace Metal {

bool NullQueryCache::HasUncommittedFlushes() const noexcept {
    if (scheduler == nullptr || last_occlusion_tick == nullptr ||
        pending_occlusion_count == nullptr || occlusion_needs_fence == nullptr) {
        return false;
    }
    if (pending_occlusion_count->load(std::memory_order_acquire) == 0 ||
        !occlusion_needs_fence->load(std::memory_order_acquire)) {
        return false;
    }
    const u64 tick = last_occlusion_tick->load(std::memory_order_acquire);
    // Stay true both while tick is still unsubmitted (tick >= CurrentTick()) and while an
    // earlier Flush() already submitted it (tick < CurrentTick()) but the GPU has not yet
    // retired it (!IsFree(tick)). Otherwise FenceManager stubs the fence and runs
    // ResolveOcclusionQueries(false) before completion, dropping the callback and stranding
    // m_pending_occlusion + PROT_NONE.
    return tick != 0 && (tick >= scheduler->CurrentTick() || !scheduler->IsFree(tick));
}

void NullQueryCache::CommitAsyncFlushes() noexcept {
    if (occlusion_needs_fence != nullptr) {
        occlusion_needs_fence->store(false, std::memory_order_release);
    }
}

InnerFence::InnerFence(Scheduler& scheduler_, bool is_stubbed_)
    : FenceBase{is_stubbed_}, scheduler{scheduler_} {}

InnerFence::~InnerFence() = default;

void InnerFence::Queue() {
    if (is_stubbed) {
        return;
    }
    // Take the tick Flush() hands back, not one sampled before it. Flush() runs
    // from several threads, so a CurrentTick() we sampled ourselves can be eaten
    // by another thread's flush and signal this fence early.
    wait_tick = scheduler.Flush();
}

bool InnerFence::IsSignaled() const {
    if (is_stubbed) {
        return true;
    }
    return scheduler.IsFree(wait_tick);
}

void InnerFence::Wait() {
    if (is_stubbed) {
        return;
    }
    scheduler.Wait(wait_tick);
}

FenceManager::FenceManager(VideoCore::RasterizerInterface& rasterizer_, Tegra::GPU& gpu_,
                           TextureCache& texture_cache_, BufferCache& buffer_cache_,
                           NullQueryCache& query_cache_, Scheduler& scheduler_)
    : GenericFenceManager{rasterizer_, gpu_, texture_cache_, buffer_cache_, query_cache_},
      scheduler{scheduler_} {}

Fence FenceManager::CreateFence(bool is_stubbed) {
    return std::make_shared<InnerFence>(scheduler, is_stubbed);
}

void FenceManager::QueueFence(Fence& fence) {
    fence->Queue();
}

bool FenceManager::IsFenceSignaled(Fence& fence) const {
    return fence->IsSignaled();
}

void FenceManager::WaitFence(Fence& fence) {
    fence->Wait();
}

} // namespace Metal
