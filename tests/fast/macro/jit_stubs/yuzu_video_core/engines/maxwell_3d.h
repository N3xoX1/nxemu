// SPDX-FileCopyrightText: Copyright 2026 NXEmu Contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace Tegra::Engines {
// Fake GPU engine shared by the interpreter and the JIT. The JIT reads front_regs directly.
class Maxwell3D {
public:
    struct Regs {
        static constexpr std::size_t NUM_REGS = 0xE00;
        std::array<std::uint32_t, NUM_REGS> reg_array{};
    };
    Regs front_regs{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> calls;

    void CallMethod(std::uint32_t method, std::uint32_t value, bool) {
        calls.emplace_back(method, value);
    }

    std::uint32_t GetFrontRegisterValue(std::uint32_t method) const {
        return front_regs.reg_array[method];
    }
};
} // namespace Tegra::Engines
