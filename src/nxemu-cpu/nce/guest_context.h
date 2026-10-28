// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#if defined(__APPLE__)
#include <pthread.h>
#endif

#include "nce/arm_nce_asm_definitions.h"
#include "yuzu_common/common_types.h"
#include <nxemu-module-spec/cpu.h>

namespace Core {

class ArmNce;

// Host page size. The patch section is aligned to it, so it gets its own host pages.
#if defined(__APPLE__)
constexpr u64 NceHostPageSize = 0x4000;
#else
constexpr u64 NceHostPageSize = 0x1000;
#endif

#if defined(__APPLE__)
// The Darwin kernel owns TPIDR_EL0 (it holds the CPU number), so the thread parameters are
// kept in a thread specific data slot, which Darwin stores at TPIDRRO_EL0 + key * 8.
inline pthread_key_t ThreadParametersKey() {
    static const pthread_key_t key = [] {
        pthread_key_t new_key{};
        pthread_key_create(&new_key, nullptr);
        return new_key;
    }();
    return key;
}

inline u32 ThreadParametersSlotOffset() {
    return static_cast<u32>(ThreadParametersKey() * sizeof(void*));
}
#endif

struct HostContext {
    alignas(16) std::array<u64, 12> host_saved_regs{};
    alignas(16) std::array<u128, 8> host_saved_vregs{};
    u64 host_sp{};
    void* host_tpidr_el0{};
    u32 host_fpcr{};
    u32 host_fpsr{};
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
static_assert(offsetof(HostContext, host_fpcr) == HostContextFpcrFpsr);
static_assert(offsetof(HostContext, host_fpsr) - 4 == HostContextFpcrFpsr);
static_assert(offsetof(HostContext, host_saved_regs) == HostContextRegs);
static_assert(offsetof(HostContext, host_saved_vregs) == HostContextVregs);

} // namespace Core
