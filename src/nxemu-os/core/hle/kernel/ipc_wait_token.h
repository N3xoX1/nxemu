// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_common/common_types.h"

namespace Kernel {
// Read under the scheduler lock. An async response signals its request's event
// and must not depend on any newer synchronous wait of the submitting thread.
constexpr bool CanCompleteIpcWait(u64 request_generation, u64 current_generation,
                                  bool waiting, bool terminating) {
    return waiting && !terminating && request_generation == current_generation;
}
constexpr bool CanPublishIpcReply(u64 request_generation, u64 current_generation,
                                 bool waiting, bool terminating, bool asynchronous,
                                 bool process_terminating) {
    return !process_terminating && (asynchronous ||
        CanCompleteIpcWait(request_generation, current_generation, waiting, terminating));
}
} // namespace Kernel
