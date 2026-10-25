// SPDX-License-Identifier: GPL-2.0-or-later

#include <string_view>

#include "nce/decode.h"
#include "nce/visitor_base.h"

namespace Core::NCE {

bool IsA64Instruction(u32 instruction) {
    const auto matcher = Dynarmic::A64::Decode<VisitorBase>(instruction);
    if (!matcher) {
        return false;
    }
    // Unallocated encodings have matchers too: an empty name, or one starting "Unallocated".
    const std::string_view name = matcher->get().GetName();
    return !name.empty() && !name.starts_with("Unallocated");
}

} // namespace Core::NCE
