#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <vector>

#include "yuzu_common/common_types.h"

namespace Tegra {

// Minimal memory-manager stub for the descriptor table's direct and fallback reads.
class MemoryManager {
public:
    explicit MemoryManager(size_t size) : bytes(size) {}

    void SetDirectAccess(bool enabled) { direct_access = enabled; }

    [[nodiscard]] size_t FallbackReads() const { return fallback_reads; }

    [[nodiscard]] const u8* GetPointer(GPUVAddr address) const {
        if (!direct_access || address >= bytes.size()) {
            return nullptr;
        }
        return reinterpret_cast<const u8*>(bytes.data() + address);
    }

    void ReadBlockUnsafe(GPUVAddr address, void* out, size_t size) const {
        ++fallback_reads;
        assert(address <= bytes.size() && size <= bytes.size() - address);
        std::memcpy(out, bytes.data() + address, size);
    }

    template <typename T>
    void Write(GPUVAddr address, const T& value) {
        assert(address <= bytes.size() && sizeof(T) <= bytes.size() - address);
        std::memcpy(bytes.data() + address, &value, sizeof(T));
    }

private:
    std::vector<std::byte> bytes;
    mutable size_t fallback_reads{};
    bool direct_access{true};
};

} // namespace Tegra
