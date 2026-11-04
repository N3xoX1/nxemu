// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <bitset>
#include <limits>
#include <span>
#include <vector>

#include "yuzu_common/common_types.h"
#include "yuzu_video_core/command_header.h"

namespace Tegra::Engines {

enum class EngineTypes : u32 {
    KeplerCompute,
    Maxwell3D,
    Fermi2D,
    MaxwellDMA,
    KeplerMemory,
};

class EngineInterface {
public:
    virtual ~EngineInterface() = default;

    /// Write the value to the register identified by method.
    virtual void CallMethod(u32 method, u32 method_argument, bool is_last_call) = 0;

    /// Write multiple values to the register identified by method.
    virtual void CallMultiMethod(u32 method, const u32* base_start, u32 amount,
                                 u32 methods_pending) = 0;

    /// Queue a non-executable register write, preserving command order.
    virtual void PushMethod(u32 method, u32 argument) {
        method_sink.emplace_back(method, argument);
    }

    /// The caller must stop the run before any executable method.
    virtual void PushMethodRun(u32 method, std::span<const CommandHeader> arguments) {
        for (const auto& argument : arguments) {
            method_sink.emplace_back(method++, argument.argument);
        }
    }

    void ConsumeSink() {
        if (method_sink.empty()) {
            return;
        }
        ConsumeSinkImpl();
    }

    std::bitset<std::numeric_limits<u16>::max()> execution_mask{};
    std::vector<std::pair<u32, u32>> method_sink{};
    bool current_dirty{};
    GPUVAddr current_dma_segment;

protected:
    virtual void ConsumeSinkImpl() {
        for (auto [method, value] : method_sink) {
            CallMethod(method, value, true);
        }
        method_sink.clear();
    }
};

} // namespace Tegra::Engines
