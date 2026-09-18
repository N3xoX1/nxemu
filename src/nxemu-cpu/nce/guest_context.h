// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>

#include "nce/arm_nce_asm_definitions.h"
#include "yuzu_common/common_types.h"
#include <nxemu-module-spec/cpu.h>

namespace Core {

class ArmNce;

struct HostContext {
    alignas(16) std::array<u64, 12> host_saved_regs{};
    alignas(16) std::array<u128, 8> host_saved_vregs{};
    u64 host_sp{};
    void* host_tpidr_el0{};
};

struct GuestContext {
    std::array<u64, 31> cpu_registers{};
    u64 sp{};
    u64 pc{};
    u32 fpcr{};
    u32 fpsr{};
    std::array<u128, 32> vector_registers{};
    u32 pstate{};
    alignas(16) HostContext host_ctx{};
    u64 tpidrro_el0{};
    u64 tpidr_el0{};
    std::atomic<u64> esr_el1{};
    u32 nzcv{};
    u32 svc{};
    IMemory * memory{};
    ArmNce * parent{};
};

// Verify assembly offsets.
static_assert(offsetof(GuestContext, sp) == GuestContextSp);
static_assert(offsetof(GuestContext, host_ctx) == GuestContextHostContext);
static_assert(offsetof(HostContext, host_sp) == HostContextSpTpidrEl0);
static_assert(offsetof(HostContext, host_tpidr_el0) - 8 == HostContextSpTpidrEl0);
static_assert(offsetof(HostContext, host_tpidr_el0) == HostContextTpidrEl0);
static_assert(offsetof(HostContext, host_saved_regs) == HostContextRegs);
static_assert(offsetof(HostContext, host_saved_vregs) == HostContextVregs);

} // namespace Core
