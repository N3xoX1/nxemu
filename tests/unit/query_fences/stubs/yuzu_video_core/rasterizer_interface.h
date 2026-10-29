// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
namespace VideoCore {
class RasterizerInterface {
public:
    void FlushCommands() { ++flushes; }
    void InvalidateGPUCache() { ++invalidations; }
    std::atomic<unsigned> flushes{};
    std::atomic<unsigned> invalidations{};
};
}
