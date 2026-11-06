// SPDX-FileCopyrightText: Copyright 2026 NXEmu Contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Minimal test-only assertions for standalone MME interpreter tests.
#pragma once

#include <cassert>
#include <cstdlib>

#define ASSERT(condition) assert(condition)
#define ASSERT_MSG(condition, ...) assert(condition)
#define DEBUG_ASSERT(condition) assert(condition)
#define UNIMPLEMENTED_MSG(...) std::abort()
#define UNREACHABLE() std::abort()
