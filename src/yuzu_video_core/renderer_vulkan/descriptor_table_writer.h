// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <algorithm>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace Vulkan {

// This metadata is valid while entries are modified through DescriptorTableWriter.
struct DescriptorTableWriterState {
    struct Block { VkDescriptorImageInfo value{}; bool constant{}; };
    std::vector<Block> blocks;
    size_t size{};
};

// Rebuild the logical table without clearing unchanged physical entries. The
// change flag compares Vulkan fields, never padding bytes or resource contents.
class DescriptorTableWriter {
public:
    static constexpr size_t BLOCK_ENTRIES = 128;
    explicit DescriptorTableWriter(std::vector<VkDescriptorImageInfo>& entries_,
                                   DescriptorTableWriterState* state_ = nullptr)
        : entries{entries_}, state{state_} {
        if (state) {
            if (state->size != entries.size()) state->blocks.clear();
            state->blocks.resize((entries.size() + BLOCK_ENTRIES - 1) / BLOCK_ENTRIES);
        }
    }

    size_t Reserve(size_t count, const VkDescriptorImageInfo& initial) {
        const size_t start = cursor;
        cursor += count;
        if (cursor > entries.size()) {
            const size_t previous_size = entries.size();
            entries.resize(cursor, initial);
            if (state) {
                state->blocks.resize((cursor + BLOCK_ENTRIES - 1) / BLOCK_ENTRIES);
                if (previous_size % BLOCK_ENTRIES) state->blocks[previous_size / BLOCK_ENTRIES].constant = false;
            }
            changed = true;
        }
        return start;
    }

    void Write(size_t index, const VkDescriptorImageInfo& value) {
        auto& entry = entries[index];
        if (!Equal(entry, value)) {
            entry = value;
            if (state) state->blocks[index / BLOCK_ENTRIES].constant = false;
            changed = true;
        }
    }

    void WriteRun(size_t index, size_t count, const VkDescriptorImageInfo& value) {
        if (state && index % BLOCK_ENTRIES == 0 && count == BLOCK_ENTRIES) {
            auto& block = state->blocks[index / BLOCK_ENTRIES];
            if (block.constant && Equal(block.value, value)) return;
            const auto first = entries.begin() + index;
            if (!std::all_of(first, first + count, [&](const auto& entry) { return Equal(entry, value); })) {
                std::fill_n(first, count, value);
                changed = true;
            }
            block = {value, true};
        } else {
            for (size_t i = 0; i < count; ++i) Write(index + i, value);
        }
    }

    void Append(const VkDescriptorImageInfo& value) {
        const size_t index = Reserve(1, value);
        Write(index, value);
    }

    void Finish() {
        if (entries.size() != cursor) {
            entries.resize(cursor);
            changed = true;
        }
        if (state) {
            state->blocks.resize((cursor + BLOCK_ENTRIES - 1) / BLOCK_ENTRIES);
            if (cursor % BLOCK_ENTRIES) state->blocks.back().constant = false;
            state->size = cursor;
        }
    }

    bool Changed() const noexcept { return changed; }

private:
    static bool Equal(const VkDescriptorImageInfo& a, const VkDescriptorImageInfo& b) {
        return a.sampler == b.sampler && a.imageView == b.imageView && a.imageLayout == b.imageLayout;
    }
    std::vector<VkDescriptorImageInfo>& entries;
    DescriptorTableWriterState* state;
    size_t cursor{};
    bool changed{};
};

} // namespace Vulkan
