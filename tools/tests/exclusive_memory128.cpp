// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>

#include "yuzu_common/atomic_ops.h"

// The runner extracts the production adapter from memory.cpp. Only the backing
// memory is replaced, so this exercises its argument conversion and the real CAS.
struct Memory {
    struct Impl {
        alignas(16) u128 data{};

        bool WriteExclusive128(uint64_t, u128 value, u128 expected) {
            return Common::AtomicCompareAndSwap(data.data(), value, expected);
        }
    } storage;

    Impl* impl = &storage;
    bool WriteExclusive128(uint64_t addr, uint64_t dataHi, uint64_t dataLow,
                           uint64_t expectedHi, uint64_t expectedLow);
};

#include "memory_exclusive128_adapter.inc"

static void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

int main() {
    Memory memory;

    memory.storage.data = {0x11, 0x22};
    bool result = memory.WriteExclusive128(0, 0x44, 0x33, 0x22, 0x11);
    Check(result && memory.storage.data == u128{0x33, 0x44},
          "matching unequal halves must compare and store correctly");

    memory.storage.data = {0, 0};
    result = memory.WriteExclusive128(0, 0x123456789ABCDEF0ULL, 1, 0, 0);
    Check(result && memory.storage.data == u128{1, 0x123456789ABCDEF0ULL},
          "a zero expected pair must preserve the new low/high positions");

    memory.storage.data = {0x11, 0x22};
    result = memory.WriteExclusive128(0, 0x44, 0x33, 0x22, 0x10);
    Check(!result && memory.storage.data == u128{0x11, 0x22},
          "a mismatching expected pair must fail without modifying memory");

    memory.storage.data = {0x11, 0x22};
    result = memory.WriteExclusive128(0, 0x44, 0x33, 0x11, 0x22);
    Check(!result && memory.storage.data == u128{0x11, 0x22},
          "reversed expected halves must not produce a false success");

    std::puts("PASS: exclusive 128-bit adapter ordering, zero pair and failed comparisons");
}
