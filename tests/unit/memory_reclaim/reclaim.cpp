// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "core/hle/kernel/k_page_heap.h"
#include "yuzu_common/host_memory.h"

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
#endif
} // namespace

int main() {
    CheckSplit(false);
    CheckSplit(true);
    CheckBacking();
#ifdef _WIN32
    CheckGuardLifetime();
#endif
}
