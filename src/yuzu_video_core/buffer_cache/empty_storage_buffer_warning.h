// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <limits>
#include "yuzu_common/common_types.h"

namespace VideoCommon {
// Only throttles repetitions of the same empty descriptor. A different descriptor
// is always reported immediately. Fixed storage, no allocation or hashing.
// Access follows the owning BufferCache's locking contract.
class EmptyStorageBufferWarningCounter {
public:
    // Returns the occurrence to report, or zero when this repetition is omitted.
    u64 Observe(const std::array<u64, 4>& descriptor) {
        if (count == 0 || descriptor != previous) {
            previous = descriptor;
            count = 1;
            return count;
        }
        if (count == std::numeric_limits<u64>::max()) {
            return 0;
        }
        ++count;
        return (count & (count - 1)) == 0 ? count : 0;
    }

private:
    std::array<u64, 4> previous{};
    u64 count{};
};
} // namespace VideoCommon
