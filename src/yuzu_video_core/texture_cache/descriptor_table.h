// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "yuzu_common/common_types.h"
#include "yuzu_common/div_ceil.h"
#include "yuzu_video_core/memory_manager.h"
#include "yuzu_video_core/rasterizer_interface.h"

namespace VideoCommon {

template <typename Descriptor>
class DescriptorTable {
public:
    static constexpr size_t BATCH_BLOCK_ENTRIES = 128;
    explicit DescriptorTable(Tegra::MemoryManager& gpu_memory_) : gpu_memory{gpu_memory_} {}

    [[nodiscard]] bool Synchronize(GPUVAddr gpu_addr, u32 limit) {
        [[likely]] if (current_gpu_addr == gpu_addr && current_limit == limit) { return false; }
        Refresh(gpu_addr, limit);
        return true;
    }

    void Invalidate() noexcept {
        std::ranges::fill(read_descriptors, 0);
    }

    [[nodiscard]] std::pair<Descriptor, bool> Read(u32 index) {
        DEBUG_ASSERT(index <= current_limit);
        const GPUVAddr gpu_addr = current_gpu_addr + index * sizeof(Descriptor);
        std::pair<Descriptor, bool> result;
        if (batch_active) {
            ++batch_reads;
            result.first = *BatchedDescriptor(index);
        } else {
            gpu_memory.ReadBlockUnsafe(gpu_addr, &result.first, sizeof(Descriptor));
        }
        if (IsDescriptorRead(index)) {
            result.second = result.first != descriptors[index];
        } else {
            MarkDescriptorAsRead(index);
            result.second = true;
        }
        if (result.second) {
            descriptors[index] = result.first;
            const size_t block = index / BATCH_BLOCK_ENTRIES;
            if (block < canonical_empty_state.size()) canonical_empty_state[block] = 0;
        }
        return result;
    }

    // Snapshot only for the current lookup pass. Every draw rereads guest memory;
    // CPU/DMA writes must never be hidden behind a persistent descriptor cache.
    bool BeginReadBatch() {
        if (batch_cooldown != 0) {
            --batch_cooldown;
            return false;
        }
        batch.resize(static_cast<size_t>(current_limit) + 1);
        const size_t total_bytes = batch.size() * sizeof(Descriptor);
        // A span is used only while checking/copying the snapshot, never by its
        // consumers. Equal blocks already have an identical local canonical copy.
        const u8* source = gpu_memory.GetSpan(current_gpu_addr, total_bytes);
        const size_t first_segment = std::min(total_bytes, size_t{4096} - size_t{current_gpu_addr & 4095});
        const bool segmented = !source && gpu_memory.GetSpan(current_gpu_addr, first_segment);
        if (!source && !segmented) gpu_memory.ReadBlockUnsafe(current_gpu_addr, batch.data(), total_bytes);
        batch_unchanged.resize(Common::DivCeil(batch.size(), BATCH_BLOCK_ENTRIES));
        batch_empty.resize(batch_unchanged.size());
        canonical_empty_state.resize(batch_unchanged.size(), 0);
        static const std::array<Descriptor, BATCH_BLOCK_ENTRIES> empty{};
        for (size_t block = 0; block < batch_unchanged.size(); ++block) {
            const size_t offset = block * BATCH_BLOCK_ENTRIES;
            const size_t count = std::min(BATCH_BLOCK_ENTRIES, batch.size() - offset);
            const size_t bytes = count * sizeof(Descriptor);
            if (segmented) {
                batch_unchanged[block] = !ReadSnapshotSegments(current_gpu_addr + offset * sizeof(Descriptor),
                    reinterpret_cast<const u8*>(descriptors.data() + offset),
                    reinterpret_cast<u8*>(batch.data() + offset), bytes);
            } else {
                const void* incoming = source ? static_cast<const void*>(source + offset * sizeof(Descriptor))
                                              : static_cast<const void*>(batch.data() + offset);
                batch_unchanged[block] = std::memcmp(incoming, descriptors.data() + offset, bytes) == 0;
                if (source && !batch_unchanged[block]) std::memcpy(batch.data() + offset, incoming, bytes);
            }
            const Descriptor* snapshot = batch_unchanged[block] ? descriptors.data() + offset
                                                               : batch.data() + offset;
            if (batch_unchanged[block] && canonical_empty_state[block] != 0) {
                batch_empty[block] = canonical_empty_state[block] == 2;
            } else {
                batch_empty[block] = std::memcmp(snapshot, empty.data(), bytes) == 0;
                if (batch_unchanged[block]) canonical_empty_state[block] = batch_empty[block] ? 2 : 1;
            }
        }
        batch_active = true;
        batch_reads = 0;
        return true;
    }

    void EndReadBatch() noexcept {
        // Dense tables do not benefit from the sparse lookup path. Retry only
        // occasionally, so a later change of occupancy is still discovered.
        if (batch_active && batch_reads > batch.size() / 2) batch_cooldown = 63;
        batch_active = false;
    }

    const Descriptor* BatchedDescriptor(u32 index) const noexcept {
        if (!batch_active || index >= batch.size()) return nullptr;
        return batch_unchanged[index / BATCH_BLOCK_ENTRIES] ? &descriptors[index] : &batch[index];
    }

    bool BatchEntryUnchanged(u32 index) const noexcept {
        return batch_active && index < batch.size() &&
               batch_unchanged[index / BATCH_BLOCK_ENTRIES] && IsDescriptorRead(index);
    }

    // The caller has resolved this entry to a null image without an image lookup.
    // Keep the canonical bytes/read bits consistent with that cached null result.
    void CommitBatchEntry(u32 index) {
        DEBUG_ASSERT(batch_active && index < batch.size());
        const size_t block = index / BATCH_BLOCK_ENTRIES;
        if (!batch_unchanged[block]) {
            descriptors[index] = *BatchedDescriptor(index);
            canonical_empty_state[block] = 0;
        }
        MarkDescriptorAsRead(index);
    }

    void CommitBatchBlock(u32 first) {
        DEBUG_ASSERT(batch_active && first % BATCH_BLOCK_ENTRIES == 0 &&
                     size_t{first} + BATCH_BLOCK_ENTRIES <= batch.size());
        const auto* snapshot = BatchedDescriptor(first);
        if (snapshot != descriptors.data() + first) {
            std::memcpy(descriptors.data() + first, snapshot, BATCH_BLOCK_ENTRIES * sizeof(Descriptor));
        }
        read_descriptors[first / 64] = ~u64{0};
        read_descriptors[first / 64 + 1] = ~u64{0};
        const size_t block = first / BATCH_BLOCK_ENTRIES;
        batch_unchanged[block] = true;
        canonical_empty_state[block] = batch_empty[block] ? 2 : 1;
    }

    [[nodiscard]] u32 Limit() const noexcept {
        return current_limit;
    }

    bool BatchBlockEmpty(u32 first) const noexcept {
        return batch_active && first % BATCH_BLOCK_ENTRIES == 0 &&
               size_t{first} + BATCH_BLOCK_ENTRIES <= batch.size() &&
               batch_empty[first / BATCH_BLOCK_ENTRIES];
    }

    bool BatchBlockUnchanged(u32 first) const noexcept {
        return batch_active && first % BATCH_BLOCK_ENTRIES == 0 &&
               size_t{first} + BATCH_BLOCK_ENTRIES <= batch.size() &&
               batch_unchanged[first / BATCH_BLOCK_ENTRIES] &&
               read_descriptors[first / 64] == ~u64{0} &&
               read_descriptors[first / 64 + 1] == ~u64{0};
    }

private:
    // A logical block can cross discontiguous GPU mappings. Check page-sized
    // segments without retaining their guest pointers. If any segment differs,
    // assemble a complete private snapshot, including the unchanged prefix/tail.
    bool ReadSnapshotSegments(GPUVAddr address, const u8* previous, u8* output, size_t size) {
        bool changed{};
        for (size_t offset = 0; offset < size;) {
            const size_t bytes = std::min(size - offset, size_t{4096} - size_t{(address + offset) & 4095});
            const u8* incoming = gpu_memory.GetSpan(address + offset, bytes);
            if (incoming) {
                if (std::memcmp(incoming, previous + offset, bytes) != 0) {
                    if (!changed && offset) std::memcpy(output, previous, offset);
                    std::memcpy(output + offset, incoming, bytes);
                    changed = true;
                } else if (changed) {
                    std::memcpy(output + offset, previous + offset, bytes);
                }
            } else {
                gpu_memory.ReadBlockUnsafe(address + offset, output + offset, bytes);
                if (std::memcmp(output + offset, previous + offset, bytes) != 0) {
                    if (!changed && offset) std::memcpy(output, previous, offset);
                    changed = true;
                }
            }
            offset += bytes;
        }
        return changed;
    }

    void Refresh(GPUVAddr gpu_addr, u32 limit) {
        batch_active = false;
        batch_cooldown = 0;
        current_gpu_addr = gpu_addr;
        current_limit = limit;
        canonical_empty_state.clear();

        const size_t num_descriptors = static_cast<size_t>(limit) + 1;
        read_descriptors.clear();
        read_descriptors.resize(Common::DivCeil(num_descriptors, 64U), 0);
        descriptors.resize(num_descriptors);
    }

    void MarkDescriptorAsRead(u32 index) noexcept {
        read_descriptors[index / 64] |= 1ULL << (index % 64);
    }

    [[nodiscard]] bool IsDescriptorRead(u32 index) const noexcept {
        return (read_descriptors[index / 64] & (1ULL << (index % 64))) != 0;
    }

    Tegra::MemoryManager& gpu_memory;
    GPUVAddr current_gpu_addr{};
    u32 current_limit{};
    std::vector<u64> read_descriptors;
    std::vector<Descriptor> descriptors;
    std::vector<Descriptor> batch;
    std::vector<u8> batch_unchanged;
    std::vector<u8> batch_empty;
    std::vector<u8> canonical_empty_state; // Unknown, nonempty, or empty canonical bytes.
    bool batch_active{};
    size_t batch_reads{};
    unsigned batch_cooldown{};
};

} // namespace VideoCommon
