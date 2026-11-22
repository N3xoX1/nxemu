// SPDX-FileCopyrightText: Copyright 2026 NXEmu Contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace Tegra::Engines {
class Maxwell3D {
public:
    std::vector<std::uint32_t>* current_macro_params{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> calls;
    bool mutate_on_macro_send{};

    void CallMethod(std::uint32_t method, std::uint32_t value, bool) {
        calls.emplace_back(method, value);
        if (mutate_on_macro_send && method >= 0xE00) {
            mutate_on_macro_send = false;
            // Mimic recursive ProcessMacro invalidating the caller's argument storage.
            current_macro_params->assign(4096, 0xDEADBEEFU);
        }
    }

    std::uint32_t GetRegisterValue(std::uint32_t) const {
        return 0;
    }
};
} // namespace Tegra::Engines
