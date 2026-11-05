// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "yuzu_common/bit_cast.h"
#include "yuzu_common/common_types.h"
#include "nxemu-module-spec/sharpening_strength.h"

namespace FSR {
// Reimplementations of the constant generating functions in ffx_fsr1.h
// GCC generated a lot of warnings when using the official header.
void FsrEasuConOffset(u32 con0[4], u32 con1[4], u32 con2[4], u32 con3[4],
                      f32 inputViewportInPixelsX, f32 inputViewportInPixelsY,
                      f32 inputSizeInPixelsX, f32 inputSizeInPixelsY, f32 outputSizeInPixelsX,
                      f32 outputSizeInPixelsY, f32 inputOffsetInPixelsX, f32 inputOffsetInPixelsY);

void FsrRcasCon(u32* con, f32 sharpness);
inline void FsrRcasConFromStrength(u32* con, SharpeningStrength strength) {
    FsrRcasCon(con, strength.RcasAttenuationStops());
}

} // namespace FSR
