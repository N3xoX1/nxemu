// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/engines/kepler_memory.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/memory_manager.h"
#include "yuzu_video_core/rasterizer_interface.h"

namespace Tegra::Engines {

KeplerMemory::KeplerMemory(MemoryManager& memory_manager)
    : upload_state{memory_manager, regs.upload} {}

KeplerMemory::~KeplerMemory() = default;

void KeplerMemory::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    upload_state.BindRasterizer(rasterizer_);

    execution_mask.reset();
    execution_mask[KEPLERMEMORY_REG_INDEX(exec)] = true;
    execution_mask[KEPLERMEMORY_REG_INDEX(data)] = true;
}

void KeplerMemory::ConsumeSinkImpl() {
    for (auto [method, value] : method_sink) {
        regs.reg_array[method] = value;
    }
    method_sink.clear();
}

void KeplerMemory::CallMethod(u32 method, u32 method_argument, bool) {
    ASSERT_MSG(method < Regs::NUM_REGS,
               "Invalid KeplerMemory register, increase the size of the Regs structure");

    regs.reg_array[method] = method_argument;

    switch (method) {
    case KEPLERMEMORY_REG_INDEX(exec): {
        upload_state.ProcessExec(regs.exec.linear != 0);
        break;
    }
    case KEPLERMEMORY_REG_INDEX(data): {
        upload_state.ProcessData(method_argument);
        break;
    }
    }
}

void KeplerMemory::CallMultiMethod(u32 method, const u32* base_start, u32 amount,
                                   u32 methods_pending) {
    switch (method) {
    case KEPLERMEMORY_REG_INDEX(data):
        upload_state.ProcessData(base_start, amount);
        return;
    default:
        for (u32 i = 0; i < amount; i++) {
            CallMethod(method, base_start[i], methods_pending - i <= 1);
        }
        break;
    }
}

} // namespace Tegra::Engines
