// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// FPSR lazy-read changes adapted for NXEmu on 2026-10-30.

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <mcl/stdint.hpp>

namespace oaknut {
struct CodeGenerator;
struct WReg;
}  // namespace oaknut

namespace Dynarmic::Backend::Arm64 {

class FpsrManager {
public:
    explicit FpsrManager(oaknut::CodeGenerator& code, size_t state_fpsr_offset);

    void Spill();
    void Load();
    void Overwrite() { fpsr_loaded = false; }
    void GetFpsr(oaknut::WReg dest);

private:
    oaknut::CodeGenerator& code;
    size_t state_fpsr_offset;
    bool fpsr_loaded = false;
};

}  // namespace Dynarmic::Backend::Arm64
