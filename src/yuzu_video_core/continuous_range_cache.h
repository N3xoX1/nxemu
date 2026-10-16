// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <limits>
#include "yuzu_common/common_types.h"

namespace Tegra {

// A mapping change invalidates both before mutation and on completion. Readers
// never retain a result obtained while any mapping update is in progress.
class MappingChangeScope {
public:
    MappingChangeScope(std::atomic<u64>& generation_, std::atomic<u32>& writers_)
        : generation{generation_}, writers{writers_} {
        writers.fetch_add(1, std::memory_order_acq_rel);
        generation.fetch_add(1, std::memory_order_release);
    }
    ~MappingChangeScope() {
        generation.fetch_add(1, std::memory_order_release);
        writers.fetch_sub(1, std::memory_order_release);
    }
    MappingChangeScope(const MappingChangeScope&) = delete;
    MappingChangeScope& operator=(const MappingChangeScope&) = delete;
private:
    std::atomic<u64>& generation;
    std::atomic<u32>& writers;
};

// Thread-local, positive results only. A contained range of a verified linear
// mapping is also linear; byte contents and GPU resource bindings are not cached.
class ContinuousRangeCache {
public:
    bool ContainsStableMapping(size_t owner_, u64 observed_generation, GPUVAddr address,
                               size_t size, const std::atomic<u64>& current_generation,
                               const std::atomic<u32>& writers) {
        // A writer can finish between the caller's generation snapshot and this
        // lookup. Recheck the generation after observing the writer count and hit.
        return writers.load(std::memory_order_acquire) == 0 &&
               Contains(owner_, observed_generation, address, size) &&
               current_generation.load(std::memory_order_acquire) == observed_generation;
    }
    bool Contains(size_t owner_, u64 generation_, GPUVAddr address, size_t size) {
        Select(owner_, generation_);
        for (const auto& range : ranges) {
            if (size != 0 && address >= range.begin && address < range.end &&
                size <= range.end - address) {
                return true;
            }
        }
        return false;
    }
    void Remember(size_t owner_, u64 generation_, GPUVAddr address, size_t size) {
        Select(owner_, generation_);
        if (size == 0 || size > std::numeric_limits<GPUVAddr>::max() - address) {
            return;
        }
        if (Contains(owner_, generation_, address, size)) {
            return;
        }
        ranges[next] = {address, address + size};
        next = (next + 1) % ranges.size();
    }
private:
    void Select(size_t owner_, u64 generation_) {
        if (owner != owner_ || generation != generation_) {
            ranges = {};
            next = 0;
            owner = owner_;
            generation = generation_;
        }
    }
    struct Range { GPUVAddr begin{}, end{}; };
    std::array<Range, 8> ranges{};
    size_t next{};
    size_t owner = std::numeric_limits<size_t>::max();
    u64 generation{};
};
} // namespace Tegra
