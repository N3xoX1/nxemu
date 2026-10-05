// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <oaknut/oaknut.hpp>

namespace Dynarmic::Backend::Arm64 {

// Deferred memory fallbacks can lie outside B.cond/CBZ/CBNZ's +/-1 MiB range.
// Keep the conditional leg local and use B for the distant target. AddressSpace
// limits the entire arm64 code cache to 128 MiB, within B's reach.
// Bound targets within signed imm19 range retain the original single instruction.
// Unbound fallbacks require the long form because Oaknut cannot expand a fixup.
inline bool IsNearConditionalTarget(const oaknut::CodeGenerator& code,
                                    const oaknut::Label& target) {
    if (!target.is_bound()) {
        return false;
    }
    const auto distance = target.offset() - code.offset();
    return distance >= -(1 << 20) && distance <= (1 << 20) - 4;
}

inline void EmitFarConditionalBranch(oaknut::CodeGenerator& code, oaknut::Cond cond,
                                     oaknut::Label& target) {
    if (IsNearConditionalTarget(code, target)) {
        code.B(cond, target);
        return;
    }
    oaknut::Label continue_execution;
    code.B(oaknut::invert(cond), continue_execution);
    code.B(target);
    code.l(continue_execution);
}

template<typename Reg>
void EmitFarZeroBranch(oaknut::CodeGenerator& code, Reg reg, oaknut::Label& target) {
    if (IsNearConditionalTarget(code, target)) {
        code.CBZ(reg, target);
        return;
    }
    oaknut::Label continue_execution;
    code.CBNZ(reg, continue_execution);
    code.B(target);
    code.l(continue_execution);
}

template<typename Reg>
void EmitFarNonZeroBranch(oaknut::CodeGenerator& code, Reg reg, oaknut::Label& target) {
    if (IsNearConditionalTarget(code, target)) {
        code.CBNZ(reg, target);
        return;
    }
    oaknut::Label continue_execution;
    code.CBZ(reg, continue_execution);
    code.B(target);
    code.l(continue_execution);
}

}  // namespace Dynarmic::Backend::Arm64
