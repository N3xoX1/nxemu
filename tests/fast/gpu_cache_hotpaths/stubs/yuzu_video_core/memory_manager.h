#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <vector>

#include "yuzu_common/common_types.h"

namespace Tegra {

// The production descriptor table only requires this one memory-manager operation.
class MemoryManager {
public:
    explicit MemoryManager(size_t size) : bytes(size) {}

    void ReadBlockUnsafe(GPUVAddr address, void* out, size_t size) const {
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
};

} // namespace Tegra
