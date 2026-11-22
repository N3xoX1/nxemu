// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "core/hle/kernel/k_page_heap.h"
#include "yuzu_common/host_memory.h"
#include "yuzu_common/multi_level_page_table.h"

#ifdef _WIN32
#include <windows.h>
#endif

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
        if (auto* alias = buffer.VirtualBasePointer()) {
            Check(alias[1] == 0 && alias[Granule + 1] == 0, "reclaimed alias held stale data");
            alias[1] = 0x7f;
            Check(alias[Granule + 1] == 0x7f && data[Granule + 1] == 0x7f, "alias lost coherence");
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
}

void CheckMultiLevelPageTableLifetime() {
    Common::MultiLevelPageTable<u32> first{32, 8, 12};
    Common::MultiLevelPageTable<u32> second{32, 8, 12};
    first.ReserveRange(0, 0x10000);
    second.ReserveRange(0x1000000, 0x10000);
    first[0] = 0x12345678u;
    second[0x1000000 >> 12] = 0x87654321u;

    auto* const first_raw = first.data();
    first = std::move(second);
    Check(first[0x1000000 >> 12] == 0x87654321u, "move-assigned multi-level page table lost data");
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    Check(VirtualQuery(first_raw, &info, sizeof(info)) && info.State == MEM_FREE,
          "old multi-level page table survived move assignment");
#else
    (void)first_raw;
#endif

    Common::MultiLevelPageTable<u32> moved{std::move(first)};
    Check(moved[0x1000000 >> 12] == 0x87654321u, "move-constructed multi-level page table lost data");
}

#ifdef _WIN32
void CheckGuardLifetime() {
    constexpr size_t Size = 64 << 20;
    Common::VirtualBuffer<u8> destination(Size);
    Common::VirtualBuffer<u8> source(Size);
    auto* old = destination.data();
    Common::GuardUntouchedPages(old, Size);
    Common::GuardUntouchedPages(source.data(), Size);
    destination = std::move(source);
    MEMORY_BASIC_INFORMATION info{};
    Check(VirtualQuery(old, &info, sizeof(info)) && info.State == MEM_FREE, "old mapping survived move");
    destination[Size - 1] = 42;
    Check(destination[Size - 1] == 42, "moved guard page inaccessible");
}

void CheckLazyCommitLifetime() {
    for (const size_t size : {size_t{4} << 20, size_t{64} << 20}) {
        Common::VirtualBuffer<u8> buf;
        buf.resize_lazy(size);
        MEMORY_BASIC_INFORMATION info{};
        Check(VirtualQuery(buf.data(), &info, sizeof(info)) && info.State == MEM_RESERVE,
              "lazy buffer was committed upfront");
        // Reading an untouched page must fault-commit a zeroed chunk without committing the rest.
        Check(buf[0] == 0, "lazy read was not zero");
        buf[size - 1] = 73;
        Check(buf[size - 1] == 73, "lazy write failed");
        Check(VirtualQuery(buf.data(), &info, sizeof(info)) && info.State == MEM_COMMIT,
              "touched start chunk not committed");
        Check(VirtualQuery(buf.data() + (size / 2), &info, sizeof(info)) && info.State == MEM_RESERVE,
              "untouched middle chunk was committed");
        auto* raw = buf.data();
        Common::VirtualBuffer<u8> moved = std::move(buf);
        Check(moved[size - 1] == 73, "moved lazy buffer lost contents");
        moved.resize_lazy(0);
        Check(VirtualQuery(raw, &info, sizeof(info)) && info.State == MEM_FREE,
              "freed lazy buffer still mapped");
    }
}
#endif
} // namespace

int main() {
    CheckSplit(false);
    CheckSplit(true);
    CheckBacking();
    CheckMultiLevelPageTableLifetime();
#ifdef _WIN32
    CheckGuardLifetime();
    CheckLazyCommitLifetime();
#endif
}
