// SPDX-FileCopyrightText: Copyright 2026 nxemu contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <concepts>
#include <cstdlib>
#include <cstring>
#include <span>
#include <type_traits>

#include "yuzu_common/common_types.h"

namespace VideoCommon {

template <class Runtime>
concept StreamUniformReusable = requires(Runtime& runtime, u32 size) {
    typename Runtime::StreamUniformRef;
    requires std::is_trivially_copyable_v<typename Runtime::StreamUniformRef>;
    { runtime.LastStreamUniformRef() } -> std::same_as<typename Runtime::StreamUniformRef>;
    runtime.RebindStreamUniform(runtime.LastStreamUniformRef(), size);
    { runtime.CurrentTick() } -> std::convertible_to<u64>;
};

// Unsupported backends (including OpenGL) have no additional state or overhead.
template <class Runtime, bool Supported = StreamUniformReusable<Runtime>>
class UniformStreamReuse {
public:
    template <class Memory>
    [[nodiscard]] bool Bind(Runtime&, Memory&, size_t, u32, u32, u64, u32) {
        return false;
    }
};

template <class Runtime>
class UniformStreamReuse<Runtime, true> {
public:
    static constexpr u32 MaxBytes = 4096;
    static constexpr size_t NumStages = 5;
    static constexpr size_t NumSlots = 18;

    UniformStreamReuse() noexcept : enabled{[] {
        const char* value = std::getenv("NXEMU_UNIFORM_STREAM_REUSE");
        return value == nullptr || value[0] != '0';
    }()} {}

    template <class Memory>
    [[nodiscard]] bool Bind(Runtime& runtime, Memory& memory, size_t stage, u32 index,
                            u32 binding_index, u64 device_addr, u32 size) {
        if (!enabled || size == 0 || size > MaxBytes || stage >= NumStages || index >= NumSlots) {
            return false;
        }

        Slot& slot = slots[stage][index];
        const u64 tick = runtime.CurrentTick();
        if (slot.size == size && slot.tick == tick && slot.misses >= MaxConsecutiveMisses) {
            // Dynamic uniforms get the original one-read, one-allocation streaming path.
            return false;
        }
        if (slot.size == size && slot.tick == tick && slot.misses < MaxConsecutiveMisses) {
            // Contents, not just guest address, determine whether an allocation may be reused.
            memory.ReadBlockUnsafe(device_addr, scratch.data(), size);
            if (std::memcmp(slot.bytes.data(), scratch.data(), size) == 0) {
                slot.misses = 0;
                runtime.RebindStreamUniform(slot.ref, size);
                return true;
            }
            ++slot.misses;
            const std::span<u8> mapped = runtime.BindMappedUniformBuffer(stage, binding_index, size);
            std::memcpy(mapped.data(), scratch.data(), size);
            std::memcpy(slot.bytes.data(), scratch.data(), size);
        } else {
            // A new tick can never reuse the old allocation. Prime the next comparison.
            if (slot.tick != tick || slot.size != size) {
                slot.misses = 0;
            }
            const std::span<u8> mapped = runtime.BindMappedUniformBuffer(stage, binding_index, size);
            // Upload memory may be uncached/write-combined: never read it back on the CPU.
            // Keep the snapshot in ordinary CPU memory and only write to the mapped buffer.
            memory.ReadBlockUnsafe(device_addr, slot.bytes.data(), size);
            std::memcpy(mapped.data(), slot.bytes.data(), size);
        }
        slot.ref = runtime.LastStreamUniformRef();
        slot.size = size;
        slot.tick = tick;
        return true;
    }

private:
    static constexpr u8 MaxConsecutiveMisses = 2;

    struct Slot {
        typename Runtime::StreamUniformRef ref{};
        u64 tick{};
        u32 size{};
        u8 misses{};
        std::array<u8, MaxBytes> bytes{};
    };

    const bool enabled;
    std::array<std::array<Slot, NumSlots>, NumStages> slots{};
    std::array<u8, MaxBytes> scratch{};
};

} // namespace VideoCommon
