// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>

namespace Vulkan {

// Worker-only snapshots of immutable sets from the same submission. Never
// retain a source across ticks: its allocator may then recycle the set.
template <typename Set, size_t Slots = 16, size_t MaxEntryBytes = 65536> class DescriptorCopyCache {
public:
    struct Source {
        Set set;
        uint32_t binding;
    };

    void Begin(uint64_t tick_) {
        if (tick == tick_) {
            return;
        }
        tick = tick_;
        for (auto& entry : entries) {
            entry.size = 0;
        }
        next = 0;
    }

    std::optional<Source> Find(uint32_t type, std::span<const std::byte> bytes) const {
        for (const auto& entry : entries) {
            if (!bytes.empty() && entry.size == bytes.size() && entry.type == type &&
                std::memcmp(entry.data.get(), bytes.data(), bytes.size()) == 0) {
                return entry.source;
            }
        }
        return std::nullopt;
    }

    void Store(uint32_t type, std::span<const std::byte> bytes, Source source) {
        if (bytes.empty() || bytes.size() > MaxEntryBytes) {
            return;
        }
        auto& entry = entries[next];
        if (entry.capacity < bytes.size()) {
            entry.data = std::make_unique<std::byte[]>(bytes.size());
            entry.capacity = bytes.size();
        }
        std::memcpy(entry.data.get(), bytes.data(), bytes.size());
        entry.size = bytes.size();
        entry.type = type;
        entry.source = source;
        next = (next + 1) % Slots;
    }

private:
    struct Entry {
        std::unique_ptr<std::byte[]> data;
        size_t size{};
        size_t capacity{};
        uint32_t type{};
        Source source{};
    };
    std::array<Entry, Slots> entries;
    uint64_t tick{};
    size_t next{};
};
} // namespace Vulkan
