// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdint>
#include <cstdio>
enum class CpuDebugWatchpointType { Read, Write };
enum class CpuHaltReason { PrefetchAbort };
inline CpuHaltReason TranslateDynarmicHaltReason(CpuHaltReason reason) { return reason; }
inline int unimplemented_calls = 0;
#define UNIMPLEMENTED() (++unimplemented_calls)
#define LOG_CRITICAL(...) ((void)0)
struct Memory {
    bool IsValidVirtualAddressRange(uint64_t addr, uint64_t size) const {
        return addr >= 0x1000 && size <= 0x1000 && addr <= 0x2000 - size;
    }
};
struct Jit {
    bool halted = false;
    void HaltExecution(CpuHaltReason) { halted = true; }
};
struct Parent { Jit* m_jit; };
struct Callbacks {
    bool m_check_memory_access = true;
    bool m_debugger_enabled = false;
    Memory m_memory;
    Jit jit;
    Parent m_parent{&jit};
#include "memory_abort64.inc"
};
int main() {
    int failed = 0;
    auto check = [&](bool result, const char* name) {
        std::printf("%s: %s\n", result ? "PASS" : "FAIL", name);
        failed += !result;
    };
    Callbacks callbacks;
    check(!callbacks.CheckMemoryAccess(0, 8, CpuDebugWatchpointType::Write) && callbacks.jit.halted,
          "enabled abort checks reject an unmapped write and halt execution");
    callbacks.jit.halted = false;
    unimplemented_calls = 0;
    check(callbacks.CheckMemoryAccess(0x1000, 8, CpuDebugWatchpointType::Read) &&
          !callbacks.jit.halted && unimplemented_calls == 0,
          "valid access without a debugger needs no unimplemented path");
    callbacks.m_check_memory_access = false;
    check(callbacks.CheckMemoryAccess(0, 8, CpuDebugWatchpointType::Write) && !callbacks.jit.halted,
          "disabled abort checks retain existing permissive behavior");
    return failed ? 1 : 0;
}
