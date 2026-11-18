// SPDX-FileCopyrightText: Copyright 2026 NXEmu Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <thread>

#include "yuzu_common/common_types.h"

namespace Vulkan::G2 {

/// Stage thread of the draw pipeline (G2 step 3f, see DESIGN-draw-pipeline.md). The buffer
/// stage runs on the GPU thread and the texture stage on a thread of its own. Objects both
/// stages use without a lock of their own (the scheduler, the staging pool, the compute pass
/// descriptor queue) are used by the buffer stage only after the texture stage finished the
/// work queued before.
struct StageDrain {
    bool (*drain)(void*) = nullptr;
    void* context = nullptr;
    std::thread::id texture_thread;
    std::thread::id draw_thread;
};

inline StageDrain stage_drain;

/// On any thread but the texture stage thread (the GPU thread, the decode thread, guest
/// threads), waits for the texture stage thread to finish its queued work
inline void DrainIfBufferStage() {
    if (stage_drain.drain) [[unlikely]] {
        if (std::this_thread::get_id() != stage_drain.texture_thread) {
            stage_drain.drain(stage_drain.context);
        }
    }
}

/// Only the thread that draws needs the queued draws done before it takes the texture cache
/// lock; the lock is enough for the other threads
inline void DrainIfDrawThread() {
    if (stage_drain.drain) [[unlikely]] {
        if (std::this_thread::get_id() == stage_drain.draw_thread) {
            stage_drain.drain(stage_drain.context);
        }
    }
}

} // namespace Vulkan::G2
