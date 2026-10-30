#pragma once
#include <cassert>
#include <cstring>
#include <vector>
#include "yuzu_common/common_types.h"
#define DEBUG_ASSERT(condition) assert(condition)
namespace Tegra {
struct MemoryManager {
    mutable unsigned reads{};
    mutable unsigned span_requests{};
    bool contiguous{};
    bool fragmented{};
    size_t blocked_span_page = size_t(-1);
    std::vector<u8> bytes = std::vector<u8>(4096);
    const u8* GetSpan(GPUVAddr address, size_t size) const {
        ++span_requests;
        assert(address + size <= bytes.size());
        if (address / 4096 == blocked_span_page) return nullptr;
        return contiguous || (fragmented && (address & 4095) + size <= 4096)
                   ? bytes.data() + address : nullptr;
    }
    // The real memory manager is an out-of-line function as well.
#ifdef _MSC_VER
    __declspec(noinline)
#else
    __attribute__((noinline))
#endif
    void ReadBlockUnsafe(GPUVAddr address, void* output, size_t size) const {
        assert(address + size <= bytes.size());
        ++reads;
        std::memcpy(output, bytes.data() + address, size);
    }
};
}
