// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include "yuzu_common/range_sets.inc"

namespace Common {
// Updated only by FIFO retirement, including payload whose guest copy was
// cancelled. This preserves the exact GPU snapshot for unchanged chunks even
// when a texture/query cache writes guest RAM without a CPU-write notification.
struct ReadbackCpuReference {
    explicit ReadbackCpuReference(std::size_t size)
        : bytes{std::make_unique_for_overwrite<std::uint8_t[]>(size)} {}
    std::unique_ptr<std::uint8_t[]> bytes;
};
struct ReadbackCpuCopy {
    std::shared_ptr<ReadbackCpuReference> reference;
    std::uint64_t offset{};
};
// The GPU reference describes RAM after the already-queued, FIFO retirements.
// CPU writes, synchronous downloads and other RAM changes invalidate its proof.
// No content hash is used, and partially invalid chunks must be downloaded.
class ReadbackReference {
public:
    static constexpr std::size_t ChunkSize = 64 * 1024;
    static constexpr std::size_t MaxChunks = 128;
    static constexpr std::size_t MaxCopySize = ChunkSize * MaxChunks;
    using Mask = std::array<std::uint32_t, MaxChunks / 32>;

    Mask ValidMask(std::uint64_t offset, std::size_t size) const {
        Mask mask{};
        if (!size || size > MaxCopySize) return mask;
        valid.ForEachInRange(offset, size, [&](std::uint64_t first, std::uint64_t last) {
            const std::size_t begin = (first - offset + ChunkSize - 1) / ChunkSize;
            const std::size_t end = last == offset + size ? (size + ChunkSize - 1) / ChunkSize
                                                        : (last - offset) / ChunkSize;
            for (std::size_t chunk = begin; chunk < end; ++chunk) {
                mask[chunk / 32] |= std::uint32_t{1} << (chunk % 32);
            }
        });
        return mask;
    }
    void Scheduled(std::uint64_t offset, std::size_t size) { valid.Add(offset, size); }
    void Invalidate(std::uint64_t offset, std::size_t size) { valid.Subtract(offset, size); }
private:
    Common::RangeSet<std::uint64_t> valid;
};
} // namespace Common
