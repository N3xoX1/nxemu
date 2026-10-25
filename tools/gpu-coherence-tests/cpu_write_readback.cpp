// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <iostream>
#include <stdexcept>
#include "yuzu_common/common_types.h"
#include "yuzu_common/range_sets.inc"
#include "yuzu_video_core/gpu_write_history.h"
#include "yuzu_video_core/buffer_cache/memory_tracker_base.h"

namespace Regression {
struct ProtectionTracker {
    void UpdatePagesCachedCount(u64, u64, int) {}
};
template <class P> struct BufferCacheFixture {
    ProtectionTracker protection;
    VideoCommon::MemoryTrackerBase<ProtectionTracker> memory_tracker{protection};
    VideoCommon::GpuWriteHistory<> gpu_write_history;
    unsigned exact_queries{};
    Common::RangeSet<u64> gpu_modified_ranges;
    Common::RangeSet<u64> pending;
    void WriteMemory(DAddr address, u64 size);
    void ClearDownload(DAddr address, u64 size) { pending.Subtract(address, size); }
    void InvalidateReadbackReferences(DAddr, u64) {}
    bool IsRegionGpuModified(DAddr address, u64 size) {
        ++exact_queries;
        bool found = false;
        gpu_modified_ranges.ForEachInRange(address, size, [&](u64, u64) { found = true; });
        return found;
    }
};
// Generated directly from production WriteMemory(), rather than a copied policy.
#include "production_cpu_write.inc"
}

int main() {
    try {
        constexpr u64 base = 0x56260000;
        for (bool reverse : {false, true}) {
            Regression::BufferCacheFixture<void> cache;
            cache.memory_tracker.UnmarkRegionAsCpuModified(base, 4096);
            cache.memory_tracker.MarkRegionAsGpuModified(base, 4096);
            cache.gpu_write_history.Mark(base, 4096);
            cache.gpu_modified_ranges.Add(base, 4096);
            cache.pending.Add(base, 4096);
            std::array<u8, 4096> guest{};
            const std::array<u64, 2> offsets = reverse ? std::array<u64, 2>{1536, 256} : std::array<u64, 2>{256, 1536};
            for (u64 offset : offsets) {
                std::fill_n(guest.begin() + offset, 64, u8{0xa5});
                cache.WriteMemory(base + offset, 64);
            }
            // A completed older GPU download writes only its still-live ranges.
            cache.pending.ForEach([&](u64 start, u64 end) {
                std::fill(guest.begin() + (start - base), guest.begin() + (end - base), u8{0x11});
            });
            for (u64 i = 0; i < guest.size(); ++i) {
                const bool cpu = (256 <= i && i < 320) || (1536 <= i && i < 1600);
                if (guest[i] != (cpu ? u8{0xa5} : u8{0x11})) {
                    std::cerr << "Older GPU download overwrote CPU byte at +" << i << '\n';
                    throw std::runtime_error("CPU writes on the same tracked page are not both protected");
                }
            }
        }
        {
            Regression::BufferCacheFixture<void> cache;
            cache.gpu_write_history.Mark(base + 65536, 4096);
            cache.gpu_modified_ranges.Add(base + 65536, 4096);
            cache.WriteMemory(base, 64);
            if (cache.exact_queries != 0) throw std::runtime_error("Never-GPU-written range performed an unnecessary exact lookup");
        }
        std::cout << "PASS: production WriteMemory preserves disjoint CPU writes against an older GPU readback\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
