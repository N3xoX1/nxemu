// SPDX-FileCopyrightText: Copyright 2026 NXEmu Contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Memory::GetSpan may only return a host pointer when the whole guest range is backed by one
// linear host range. The production definitions from nxemu-os/core/memory.cpp (extracted
// unchanged by extract.py) run on a real Common::PageTable with random mappings, holes, remaps,
// rasterizer-cached and debug pages, and every answer is checked against a page-by-page oracle.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <vector>

#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/page_table.h"

namespace {
constexpr std::size_t YUZU_PAGEBITS = 12;
constexpr u64 YUZU_PAGESIZE = 1ULL << YUZU_PAGEBITS;
constexpr u64 YUZU_PAGEMASK = YUZU_PAGESIZE - 1;

#include "free.inc"

constexpr u64 PHYS_BASE = 0x80000000;
constexpr u64 PHYS_PAGES = 4096;

struct FakeDeviceMemory {
    u8* base{};
    template <typename T>
    T* GetPointer(Common::PhysicalAddress addr) {
        return reinterpret_cast<T*>(base + (addr.GetValue() - PHYS_BASE));
    }
};

struct FakeSystem {
    FakeDeviceMemory memory;
    FakeDeviceMemory& DeviceMemory() {
        return memory;
    }
};

struct Impl {
    FakeSystem& system;
    Common::PageTable* current_page_table;

#include "impl.inc"
};

constexpr std::size_t ADDRESS_SPACE_BITS = 32;
constexpr u64 ADDRESS_SPACE_PAGES = 1ULL << (ADDRESS_SPACE_BITS - YUZU_PAGEBITS);

// Two windows of guest pages: one low, one ending at the top of the address space.
constexpr u64 WINDOW_PAGES = 192;
constexpr u64 WINDOWS[]{0x1000, ADDRESS_SPACE_PAGES - WINDOW_PAGES};

class Fixture {
public:
    Fixture() : physical(static_cast<std::size_t>((PHYS_PAGES + 1) * YUZU_PAGESIZE)) {
        const auto raw = reinterpret_cast<std::uintptr_t>(physical.data());
        system.memory.base =
            reinterpret_cast<u8*>((raw + YUZU_PAGEMASK) & ~static_cast<std::uintptr_t>(YUZU_PAGEMASK));
        table.Resize(ADDRESS_SPACE_BITS, YUZU_PAGEBITS);
        oracle.assign(ADDRESS_SPACE_PAGES, -1);
    }

    void Map(u64 vpage, u64 count, u64 ppage) {
        impl.MapPages(table, vpage, count, Common::PhysicalAddress{PHYS_BASE + ppage * YUZU_PAGESIZE},
                      Common::PageType::Memory);
        for (u64 i = 0; i < count; ++i) {
            oracle[vpage + i] = static_cast<s64>(ppage + i);
        }
    }

    void Unmap(u64 vpage, u64 count) {
        impl.MapPages(table, vpage, count, Common::PhysicalAddress{0}, Common::PageType::Unmapped);
        for (u64 i = 0; i < count; ++i) {
            oracle[vpage + i] = -1;
        }
    }

    // Like RasterizerMarkRegionCached and debug memory: the direct pointer goes away, the backing
    // address stays.
    void Indirect(u64 vpage, u64 count, Common::PageType type) {
        for (u64 page = vpage; page < vpage + count; ++page) {
            if (oracle[page] >= 0) {
                table.pointers[page].Store(0, type);
            }
        }
    }

    const u8* Expected(u64 addr, u64 size) const {
        const u64 end = addr + (size != 0 ? size : 1);
        if (end < addr || end > (1ULL << ADDRESS_SPACE_BITS)) {
            return nullptr;
        }
        const u64 first = addr >> YUZU_PAGEBITS;
        const u64 last = (end - 1) >> YUZU_PAGEBITS;
        for (u64 page = first; page <= last; ++page) {
            if (oracle[page] < 0 || oracle[page] - oracle[first] != static_cast<s64>(page - first)) {
                return nullptr;
            }
        }
        return system.memory.base + static_cast<u64>(oracle[first]) * YUZU_PAGESIZE +
               (addr & YUZU_PAGEMASK);
    }

    const u8* Actual(u64 addr, u64 size) const {
        return impl.GetSpan(addr, size);
    }

    std::vector<u8> physical;
    FakeSystem system;
    Common::PageTable table;
    Impl impl{system, &table};
    std::vector<s64> oracle;
};

int failures = 0;

void Check(const Fixture& f, u64 addr, u64 size, const char* what) {
    const u8* const expected = f.Expected(addr, size);
    const u8* const actual = f.Actual(addr, size);
    if (expected == actual) {
        return;
    }
    if (++failures <= 10) {
        std::fprintf(stderr, "FAIL %s: GetSpan(0x%llx, 0x%llx) returned %s\n", what,
                     static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size),
                     actual == nullptr ? "nullptr for a linear range"
                     : expected == nullptr ? "a pointer for a non-linear range"
                                           : "the wrong pointer");
    }
}

void DirectedCases(Fixture& f) {
    const u64 base = 0x2000;
    const u64 addr = base * YUZU_PAGESIZE;
    f.Map(base, 4, 100);
    f.Map(base + 4, 1, 500); // Unrelated mapping right after the first one
    Check(f, addr, 4 * YUZU_PAGESIZE, "range ending at a mapping boundary");
    Check(f, addr + 3 * YUZU_PAGESIZE, 2 * YUZU_PAGESIZE, "range crossing into another mapping");
    // Replace a page in the middle of one MapPages call: both ends keep the same block base.
    f.Unmap(base + 1, 1);
    Check(f, addr, 3 * YUZU_PAGESIZE, "hole in the middle of a mapping");
    f.Map(base + 1, 1, 900);
    Check(f, addr, 3 * YUZU_PAGESIZE, "remapped page in the middle of a mapping");
    // Physically contiguous pages mapped by separate calls form one linear range.
    f.Map(base + 1, 1, 101);
    Check(f, addr, 4 * YUZU_PAGESIZE, "contiguous pages from separate mappings");
    f.Indirect(base + 2, 1, Common::PageType::RasterizerCachedMemory);
    Check(f, addr, 4 * YUZU_PAGESIZE, "rasterizer-cached page inside a linear range");
    f.Unmap(base, 5);
    const u64 top = ADDRESS_SPACE_PAGES - 2;
    f.Map(top, 2, 50);
    Check(f, top * YUZU_PAGESIZE, 2 * YUZU_PAGESIZE, "range ending at the address space limit");
    Check(f, (top + 2) * YUZU_PAGESIZE - 1, 0, "empty range on the last byte");
    Check(f, top * YUZU_PAGESIZE, 3 * YUZU_PAGESIZE, "range past the address space limit");
    Check(f, ~0ULL - 8, 64, "range wrapping around");
    f.Unmap(top, 2);
}

void RandomCases(Fixture& f, u64 trials, u64 seed) {
    std::mt19937_64 rng{seed};
    const auto uniform = [&](u64 low, u64 high) {
        return std::uniform_int_distribution<u64>{low, high}(rng);
    };
    for (u64 trial = 0; trial < trials; ++trial) {
        const u64 window = WINDOWS[trial % 2];
        f.Unmap(window, WINDOW_PAGES);
        u64 next_ppage = uniform(0, PHYS_PAGES - 64);
        for (u64 n = uniform(2, 10); n > 0; --n) {
            const u64 count = uniform(1, 32);
            const u64 vpage = window + uniform(0, WINDOW_PAGES - count);
            // Often continue the previous physical range, so separate mappings join up.
            const u64 ppage = uniform(0, 2) == 0 ? uniform(0, PHYS_PAGES - 32) : next_ppage;
            if (ppage + count <= PHYS_PAGES) {
                f.Map(vpage, count, ppage);
                next_ppage = ppage + count;
            }
        }
        for (u64 n = uniform(0, 3); n > 0; --n) {
            const u64 count = uniform(1, 8);
            f.Unmap(window + uniform(0, WINDOW_PAGES - count), count);
        }
        for (u64 n = uniform(0, 3); n > 0; --n) {
            const u64 count = uniform(1, 16);
            f.Indirect(window + uniform(0, WINDOW_PAGES - count), count,
                       uniform(0, 3) == 0 ? Common::PageType::DebugMemory
                                          : Common::PageType::RasterizerCachedMemory);
        }
        for (int query = 0; query < 400; ++query) {
            const u64 page = window + uniform(0, WINDOW_PAGES - 1);
            u64 offset;
            switch (uniform(0, 3)) {
            case 0:
                offset = 0;
                break;
            case 1:
                offset = YUZU_PAGESIZE - uniform(1, 16);
                break;
            default:
                offset = uniform(0, YUZU_PAGESIZE - 1);
                break;
            }
            const u64 addr = page * YUZU_PAGESIZE + offset;
            u64 size;
            switch (uniform(0, 4)) {
            case 0:
                size = uniform(0, 64);
                break;
            case 1:
                // End exactly on a page boundary.
                size = (page + uniform(1, 8)) * YUZU_PAGESIZE - addr;
                break;
            case 2:
                size = uniform(1, 40) * YUZU_PAGESIZE;
                break;
            default:
                size = uniform(1, 40 * YUZU_PAGESIZE);
                break;
            }
            Check(f, addr, size, "random");
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    std::fill(std::begin(Common::Log::g_classLevel), std::end(Common::Log::g_classLevel),
              LogLevel::Warning);
    const u64 trials = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2000;
    const u64 seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
    auto fixture = std::make_unique<Fixture>();
    DirectedCases(*fixture);
    RandomCases(*fixture, trials, seed);
    std::fprintf(stderr, "%d GetSpan mismatches\n", failures);
    return failures == 0 ? 0 : 1;
}
