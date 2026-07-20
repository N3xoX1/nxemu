// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <yuzu_common/yuzu_assert.h>
#include "arm_dynarmic.h"

Dynarmic::HaltReason TranslateDynarmicHaltReason(CpuHaltReason hr)
{
    switch (hr)
    {
    case CpuHaltReason::StepThread: return Dynarmic::HaltReason::Step;
    case CpuHaltReason::CacheInvalidation: return Dynarmic::HaltReason::CacheInvalidation;
    case CpuHaltReason::DataAbort: return Dynarmic::HaltReason::MemoryAbort;
    case CpuHaltReason::BreakLoop: return Dynarmic::HaltReason::UserDefined2;
    case CpuHaltReason::SupervisorCall: return Dynarmic::HaltReason::UserDefined3;
    case CpuHaltReason::SupervisorCallBreakLoop: return (Dynarmic::HaltReason::UserDefined3 | Dynarmic::HaltReason::UserDefined2);
    case CpuHaltReason::InstructionBreakpoint: return Dynarmic::HaltReason::UserDefined4;
    case CpuHaltReason::PrefetchAbort: return Dynarmic::HaltReason::UserDefined6;
    case CpuHaltReason::PrefetchAbortBreakLoop: return (Dynarmic::HaltReason::UserDefined6 | Dynarmic::HaltReason::UserDefined2);
    }
    UNIMPLEMENTED();
    return Dynarmic::HaltReason::Step;
}

CpuHaltReason TranslateHaltReason(Dynarmic::HaltReason hr)
{
    switch (hr)
    {
    case Dynarmic::HaltReason::Step: return CpuHaltReason::StepThread;
    case Dynarmic::HaltReason::CacheInvalidation: return CpuHaltReason::CacheInvalidation;
    case Dynarmic::HaltReason::MemoryAbort: return CpuHaltReason::DataAbort;
    case Dynarmic::HaltReason::UserDefined2: return CpuHaltReason::BreakLoop;
    case Dynarmic::HaltReason::UserDefined3: return CpuHaltReason::SupervisorCall;
    case Dynarmic::HaltReason::UserDefined4: return CpuHaltReason::InstructionBreakpoint;
    case Dynarmic::HaltReason::UserDefined6: return CpuHaltReason::PrefetchAbort;
    case Dynarmic::HaltReason::UserDefined2and3: return CpuHaltReason::SupervisorCallBreakLoop;
    case Dynarmic::HaltReason::UserDefined2and6: return CpuHaltReason::PrefetchAbortBreakLoop;
    default: break;
    }
    UNIMPLEMENTED();
    return CpuHaltReason::StepThread;
}
