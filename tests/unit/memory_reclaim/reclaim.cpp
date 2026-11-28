// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "core/hle/kernel/k_page_heap.h"
#include "yuzu_common/host_memory.h"

u64 Kernel::Board::Nintendo::Nx::KSystemControl::GenerateRandomU64() {
    return 17; // Repeatable splits in the real page heap.
}

namespace {
void Check(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

void CheckSplit(bool random) {
    using namespace Kernel;
    constexpr KPhysicalAddress Base{0x80000000};
    constexpr size_t Size = 2 << 20;
    constexpr size_t Granules = Size >> KPageHeap::GranuleShift;
    KPageHeap heap;
    heap.Initialize(Base, Size, 0, KPageHeap::CalculateManagementOverheadSize(Size));
    heap.Free(Base, Size / PageSize);
    for (size_t i = 0; i < Granules; ++i) {
        heap.GranuleReleaseState(i) = 1;
    }

    const size_t count = random ? 17 : 1;
    const auto addr = random ? heap.AllocateAligned(KPageHeap::GetAlignedBlockIndex(count, 1),
                                                    count, 1)
                             : heap.AllocateBlock(0, false);
    Check(addr >= Base && addr + count * PageSize <= Base + Size, "allocation outside heap");
    const size_t offset = addr.GetValue() - Base.GetValue();
    const size_t first = offset >> KPageHeap::GranuleShift;
    const size_t last = (offset + count * PageSize - 1) >> KPageHeap::GranuleShift;
    Check(heap.GetNumFreeGranules() == Granules - (last - first + 1), "free granule count");
    for (size_t i = 0; i < Granules; ++i) {
        const bool untouched = i < first || i > last;
        Check(heap.IsGranuleFree(i) == untouched, "split granule ownership");
        Check((heap.GranuleReleaseState(i) != 0) == untouched, "split changed idle history");
    }
    heap.Free(addr, count);
    Check(heap.GetNumFreeGranules() == Granules && heap.GetFreeSize() == Size, "free after split");
    Check(heap.GranuleReleaseState(first) == 0, "freshly freed page reclaimed too soon");
}

void CheckBacking() {
    constexpr size_t Granule = 1 << 16;
    Common::HostMemory buffer(3 * Granule, 4 * Granule);
    Check(!buffer.ReleaseFreeBacking(3 * Granule), "released past end of backing");
    Check(!buffer.ReleaseFreeBacking(size_t{0} - Granule), "wrapped backing offset");
    buffer.ClearBackingRegion(0, 3 * Granule, 0x5a);
    auto* data = buffer.BackingBasePointer();
    buffer.ClearBackingRegion(Granule, 1, 0);
    Check(data[Granule] == 0 && data[Granule + 1] == 0x5a, "partial clear overwrote neighbor");
    if (buffer.VirtualBasePointer()) {
        buffer.Map(0, Granule, Granule, Common::MemoryPermission::ReadWrite, false);
        buffer.Map(Granule, Granule, Granule, Common::MemoryPermission::ReadWrite, false);
    }
    // A successful reclaim must zero the entire granule, including existing aliases.
    if (buffer.ReleaseFreeBacking(Granule)) {
        Check(!buffer.IsBackingDirty(Granule), "released backing still dirty");
        Check(std::all_of(data + Granule, data + 2 * Granule, [](u8 b) { return b == 0; }),
              "reallocated backing was not zero");
        if (volatile u8* alias = buffer.VirtualBasePointer()) {
            // Two offsets of the same base pointer alias the same physical page via mmap/MapViewOfFile;
            // use volatile so the optimizer cannot assume alias[1] and alias[Granule + 1] are distinct.
            volatile u8* backing = data;
            Check(alias[1] == 0 && alias[Granule + 1] == 0, "reclaimed alias held stale data");
            alias[1] = 0x7f;
            Check(alias[Granule + 1] == 0x7f && backing[Granule + 1] == 0x7f,
                  "alias lost coherence");
        }
    }
    Check(data[Granule - 1] == 0x5a && data[2 * Granule] == 0x5a, "reclaim crossed granule");

    // Coalesced range release must clear every granule in the span without crossing bounds.
    buffer.ClearBackingRegion(0, 3 * Granule, 0x3c);
    Check(!buffer.ReleaseFreeBackingRange(0, 0), "released empty range");
    Check(!buffer.ReleaseFreeBackingRange(0, Granule / 2), "released unaligned length");
    Check(!buffer.ReleaseFreeBackingRange(Granule, 3 * Granule), "released past end of backing");
    if (buffer.ReleaseFreeBackingRange(0, 2 * Granule)) {
        Check(!buffer.IsBackingDirty(0) && !buffer.IsBackingDirty(Granule),
              "coalesced release left dirty granule");
        Check(buffer.IsBackingDirty(2 * Granule), "coalesced release cleared neighbor dirty bit");
        Check(std::all_of(data, data + 2 * Granule, [](u8 b) { return b == 0; }),
              "coalesced release did not zero full span");
        Check(data[2 * Granule] == 0x3c, "coalesced release crossed range end");
    }

    // Releasing an uncommitted-but-marked-dirty granule must not fault under lazy guest commit.
    Common::HostMemory lazy_buf(2 * Granule, 4 * Granule);
    lazy_buf.MarkBackingDirty(Granule, Granule);
    if (lazy_buf.ReleaseFreeBackingRange(Granule, Granule)) {
        Check(!lazy_buf.IsBackingDirty(Granule), "uncommitted dirty granule stayed dirty");
    }

    // Pinned granules (e.g. GPU host-imported chunks on drivers without MMU notifiers) must
    // never be hole-punched by ReleaseFreeBacking*, while neighbors still reclaim and
    // ClearBackingRegion(..., 0) still zeroes pinned granules in place.
    Common::HostMemory pinned_buf(3 * Granule, 4 * Granule);
    pinned_buf.ClearBackingRegion(0, 3 * Granule, 0x4b);
    pinned_buf.MarkBackingPinned(Granule, Granule);
    Check(!pinned_buf.IsBackingPinned(0) && pinned_buf.IsBackingPinned(Granule) &&
              !pinned_buf.IsBackingPinned(2 * Granule),
          "pinned granule mask mismatch");
    Check(!pinned_buf.ReleaseFreeBacking(Granule), "released pinned granule");
    Check(!pinned_buf.ReleaseFreeBackingRange(0, 2 * Granule),
          "released range overlapping pinned granule");
    auto* pinned_data = pinned_buf.BackingBasePointer();
    Check(pinned_data[Granule] == 0x4b && pinned_buf.IsBackingDirty(Granule),
          "pinned granule modified by rejected release");
    if (pinned_buf.ReleaseFreeBacking(0)) {
        Check(pinned_data[0] == 0 && pinned_data[Granule] == 0x4b,
              "unpinned neighbor release affected pinned granule");
    }
    pinned_buf.ClearBackingRegion(0, 3 * Granule, 0);
    Check(std::all_of(pinned_data, pinned_data + 3 * Granule, [](u8 b) { return b == 0; }),
          "clear across pinned boundary did not zero full span");
    Check(!pinned_buf.IsBackingDirty(Granule) && pinned_buf.IsBackingPinned(Granule),
          "clear across pinned granule lost pinned state");

    // Releasing a pin (e.g. after a failed or torn-down GPU import) restores reclamation.
    pinned_buf.ClearBackingRegion(Granule, Granule, 0x6d);
    pinned_buf.UnmarkBackingPinned(Granule, Granule);
    Check(!pinned_buf.IsBackingPinned(Granule), "unpinned granule stayed pinned");
    if (pinned_buf.ReleaseFreeBacking(Granule)) {
        Check(!pinned_buf.IsBackingDirty(Granule), "reclaimed unpinned granule stayed dirty");
        Check(std::all_of(pinned_data + Granule, pinned_data + 2 * Granule,
                          [](u8 b) { return b == 0; }),
              "reclaimed unpinned granule was not zeroed");
    }
}
} // namespace

int main() {
    CheckSplit(false);
    CheckSplit(true);
    CheckBacking();
}
