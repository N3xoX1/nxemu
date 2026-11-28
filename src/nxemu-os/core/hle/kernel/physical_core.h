// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <yuzu_common/common_funcs.h>

#include <nxemu-module-spec/cpu.h>

namespace Kernel {
class KernelCore;
class KThread;
} // namespace Kernel

namespace Core {
class System;
} // namespace Core

namespace Kernel
{

class PhysicalCore
{
public:
    PhysicalCore(KernelCore & kernel, uint32_t core_index);
    ~PhysicalCore();

    YUZU_NON_COPYABLE(PhysicalCore);
    YUZU_NON_MOVEABLE(PhysicalCore);

    // Execute guest code running on the given thread.
    void RunThread(KThread* thread);

    // Copy context from thread to current core.
    void LoadContext(const KThread* thread);
    void LoadSvcArguments(const KProcess & process, const uint64_t (&args)[8]);

    // Copy context from current core to thread.
    void SaveContext(KThread* thread) const;
    void SaveSvcArguments(KProcess & process, uint64_t (&args)[8]) const;

    // Copy floating point status registers to the target thread.
    void CloneFpuStatus(KThread* dst) const;

    // Log backtrace of current processor state.
    void LogBacktrace();

    // Wait for an interrupt.
    void Idle();

    // Interrupt this core.
    void Interrupt();

    // Clear this core's interrupt.
    void ClearInterrupt();

    // Check if this core is interrupted.
    bool IsInterrupted() const;

    uint32_t CoreIndex() const
    {
        return m_core_index;
    }

private:
    KernelCore& m_kernel;
    const uint32_t m_core_index;

    std::mutex m_guard;
    std::condition_variable m_on_interrupt;
    ICpuCore * m_cpucore{};
    KThread * m_current_thread{};
    std::atomic_bool m_is_interrupted{};
    bool m_is_single_core{};
};

} // namespace Kernel
