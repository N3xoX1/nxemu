// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "yuzu_common/common_types.h"

namespace Core::NCE {

// True when the word is an allocated A64 instruction.
bool IsA64Instruction(u32 instruction);

} // namespace Core::NCE
