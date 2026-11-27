// SPDX-FileCopyrightText: Copyright 2026 NXEmu Contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Assertions for the macro JIT differential test. Failed assertions are counted instead of
// aborting, so the interpreter can run with a surplus parameter FIFO like the JIT.
#pragma once

#include <cstdlib>

namespace MacroTest {
inline int assert_failures = 0;
}

#define ASSERT(condition)                                                                          \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            ++MacroTest::assert_failures;                                                          \
        }                                                                                          \
    } while (0)
#define ASSERT_MSG(condition, ...) ASSERT(condition)
#define DEBUG_ASSERT(condition) ASSERT(condition)
#define ASSERT_OR_EXECUTE(condition, body)                                                         \
    do {                                                                                           \
        ASSERT(condition);                                                                         \
        if (!(condition)) {                                                                        \
            body                                                                                   \
        }                                                                                          \
    } while (0)
#define UNIMPLEMENTED_MSG(...) std::abort()
#define UNREACHABLE() std::abort()
