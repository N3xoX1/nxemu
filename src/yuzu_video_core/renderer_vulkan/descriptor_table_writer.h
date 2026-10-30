// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <algorithm>
#include <memory>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace Vulkan {

// Only resolved host handles cross the scheduler boundary. The worker expands
// this immutable recipe without reading guest memory or either resource cache.
struct DescriptorTablePlan {
    struct Run { size_t first{}, count{}; VkDescriptorImageInfo value{}; };
    std::vector<Run> defaults;
    std::vector<Run> writes;
    size_t size{};
    size_t written_end{};

    static bool EqualEntry(const VkDescriptorImageInfo& a, const VkDescriptorImageInfo& b) {
        return a.sampler == b.sampler && a.imageView == b.imageView && a.imageLayout == b.imageLayout;
    }
    bool operator==(const DescriptorTablePlan& other) const {
        const auto equal_run = [](const Run& a, const Run& b) {
            return a.first == b.first && a.count == b.count && EqualEntry(a.value, b.value);
        };
        return size == other.size && std::ranges::equal(defaults, other.defaults, equal_run) &&
               std::ranges::equal(writes, other.writes, equal_run);
    }
    void Materialize(std::vector<VkDescriptorImageInfo>& entries) const {
        entries.resize(size);
        for (const auto& run : defaults) std::fill_n(entries.begin() + run.first, run.count, run.value);
        for (const auto& run : writes) std::fill_n(entries.begin() + run.first, run.count, run.value);
    }
    void Write(size_t first, size_t count, const VkDescriptorImageInfo& value) {
        if (count == 0) return;
        // Production emits each array in order. Preserve later overwrites too,
        // including restoring a default over a previously nondefault entry.
        const bool after_writes = first >= written_end;
        if (after_writes && !defaults.empty()) {
            const auto& base = defaults.back();
            if (first >= base.first && first - base.first <= base.count &&
                count <= base.count - (first - base.first) && EqualEntry(value, base.value)) return;
        }
        writes.push_back({first, count, value});
        written_end = std::max(written_end, first + count);
    }
};

// Owned and accessed exclusively by the scheduler worker. UpdateDescriptorSets
// consumes these host bytes before returning, so later recipes may reuse them.
// Host memory lifetime: https://docs.vulkan.org/spec/latest/chapters/fundamentals.html#fundamentals-objectmodel-lifetime
class DescriptorTableMaterializer {
public:
    const std::vector<VkDescriptorImageInfo>& Resolve(
        const std::shared_ptr<const DescriptorTablePlan>& next) {
        if (current != next) {
            next->Materialize(entries);
            current = next;
        }
        return entries;
    }
private:
    std::shared_ptr<const DescriptorTablePlan> current;
    std::vector<VkDescriptorImageInfo> entries;
};

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
                                   DescriptorTableWriterState* state_ = nullptr,
                                   DescriptorTablePlan* plan_ = nullptr)
        : entries{entries_}, state{state_}, plan{plan_} {
        if (plan) {
            plan->defaults.clear();
            plan->writes.clear();
            plan->size = 0;
            plan->written_end = 0;
            return;
        }
        if (state) {
            if (state->size != entries.size()) state->blocks.clear();
            state->blocks.resize((entries.size() + BLOCK_ENTRIES - 1) / BLOCK_ENTRIES);
        }
    }

    size_t Reserve(size_t count, const VkDescriptorImageInfo& initial) {
        const size_t start = cursor;
        cursor += count;
        if (plan) {
            if (count) {
                if (!plan->defaults.empty() &&
                    plan->defaults.back().first + plan->defaults.back().count == start &&
                    DescriptorTablePlan::EqualEntry(plan->defaults.back().value, initial)) {
                    plan->defaults.back().count += count;
                } else {
                    plan->defaults.push_back({start, count, initial});
                }
            }
            return start;
        }
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
        if (plan) {
            plan->Write(index, 1, value);
            return;
        }
        auto& entry = entries[index];
        if (!Equal(entry, value)) {
            entry = value;
            if (state) state->blocks[index / BLOCK_ENTRIES].constant = false;
            changed = true;
        }
    }

    void WriteRun(size_t index, size_t count, const VkDescriptorImageInfo& value) {
        if (plan) {
            plan->Write(index, count, value);
            return;
        }
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
        if (plan) return; // The newly reserved element already has this default.
        Write(index, value);
    }

    bool IsCompact() const noexcept { return plan != nullptr; }

    void Finish() {
        if (plan) {
            plan->size = cursor;
            return;
        }
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
    DescriptorTablePlan* plan;
    size_t cursor{};
    bool changed{};
};

} // namespace Vulkan
