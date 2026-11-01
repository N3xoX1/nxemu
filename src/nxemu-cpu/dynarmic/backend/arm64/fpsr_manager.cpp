// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// FPSR lazy-read changes adapted for NXEmu on 2026-10-30.

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/arm64/fpsr_manager.h"

#include <oaknut/oaknut.hpp>

#include "dynarmic/backend/arm64/abi.h"

namespace Dynarmic::Backend::Arm64 {

using namespace oaknut::util;

FpsrManager::FpsrManager(oaknut::CodeGenerator& code, size_t state_fpsr_offset)
        : code{code}, state_fpsr_offset{state_fpsr_offset} {}

void FpsrManager::Spill() {
    if (!fpsr_loaded)
        return;

    code.LDR(Wscratch0, Xstate, state_fpsr_offset);
    code.MRS(Xscratch1, oaknut::SystemReg::FPSR);
    code.ORR(Wscratch0, Wscratch0, Wscratch1);
    code.STR(Wscratch0, Xstate, state_fpsr_offset);

    fpsr_loaded = false;
}

void FpsrManager::Load() {
    if (fpsr_loaded)
        return;

    code.MSR(oaknut::SystemReg::FPSR, XZR);

    fpsr_loaded = true;
}

void FpsrManager::GetFpsr(oaknut::WReg dest) {
    // Read the guest's accumulated exception flags without forcing a spill.
    code.LDR(dest, Xstate, state_fpsr_offset);
    if (fpsr_loaded) {
        code.MRS(Xscratch1, oaknut::SystemReg::FPSR);
        code.ORR(dest, dest, Wscratch1);
    }
}

}  // namespace Dynarmic::Backend::Arm64
