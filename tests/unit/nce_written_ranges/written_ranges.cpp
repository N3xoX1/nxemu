// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
#include "yuzu_common/common_types.h"
#include "nxemu-module-spec/operating_system.h"
#include "nxemu-os/core/gpu_dirty_memory_manager.h"
#include "nxemu-os/core/nce_written_ranges.h"

namespace {
constexpr u64 PageSize = 0x1000;
using Page = std::array<u8, PageSize>;
using Ranges = std::vector<std::pair<u64, u64>>;

Ranges Written(const Page& current, const Page& snapshot) {
    Ranges ranges;
    Core::ForEachNceWrittenRange(current.data(), snapshot.data(), PageSize,
                                 [&](u64 offset, u64 size) { ranges.emplace_back(offset, size); });
    return ranges;
}

void Store(Page& page, u64 offset, u32 value) {
    std::memcpy(page.data() + offset, &value, sizeof(value));
}

u32 Load(const Page& page, u64 offset) {
    u32 value;
    std::memcpy(&value, page.data() + offset, sizeof(value));
    return value;
}

// The buffer cache drops the GPU writes a CPU write overlaps and uploads guest memory there.
void Invalidate(Page& host, std::vector<bool>& gpu_modified, const Page& guest, const Ranges& ranges) {
    for (const auto& [offset, size] : ranges) {
        for (u64 i = offset; i < offset + size; ++i) {
            gpu_modified[i] = false;
            host[i] = guest[i];
        }
    }
}
// Invalidations the GPU thread applies when it gathers the CPU writes, relative to a page.
struct Recorder final : ICacheInvalidator {
    u64 base;
    Ranges ranges;
    explicit Recorder(u64 base_) : base{base_} {}
    void OnCacheInvalidation(uint64_t address, uint32_t size) override {
        ranges.emplace_back(address - base, size);
    }
};

template <typename Collect>
Ranges Gathered(u64 base, const Ranges& written, Collect&& collect) {
    Core::GPUDirtyMemoryManager manager;
    for (const auto& [offset, size] : written) collect(manager, base + offset, size);
    Recorder recorder{base};
    manager.Gather(&recorder);
    return recorder.ranges;
}
} // namespace

int main() {
    unsigned failures{};
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
    };

    Page snapshot{};
    check(Written(snapshot, snapshot).empty(), "an unchanged page reports nothing");

    Page current = snapshot;
    Store(current, 0x104, 1);
    Ranges ranges = Written(current, snapshot);
    check(ranges.size() == 1 && ranges[0] == std::pair<u64, u64>{0x104, 4}, "one word is reported as itself");

    current = snapshot;
    Store(current, 0x3c, 1);
    Store(current, 0x40, 2);
    Store(current, 0x7c, 3);
    ranges = Written(current, snapshot);
    check(ranges == Ranges{{0x3c, 8}, {0x7c, 4}}, "unchanged words split writes across adjacent blocks");

    current = snapshot;
    Store(current, 0x0, 1);
    Store(current, 0x200, 2);
    ranges = Written(current, snapshot);
    check(ranges.size() == 2 && ranges[0] == std::pair<u64, u64>{0, 4} && ranges[1] == std::pair<u64, u64>{0x200, 4},
          "separate writes are reported separately");

    current.fill(0xff);
    ranges = Written(current, snapshot);
    check(ranges.size() == 1 && ranges[0] == std::pair<u64, u64>{0, PageSize}, "a rewritten page is one range");

    // Pokemon Legends Z-A: the GPU writes counters at the end of one storage buffer, in a page
    // that the CPU also fills from 0xaa0 with the next buffer. The counters are not downloaded.
    Page guest{};
    Page host = guest;
    std::vector<bool> gpu_modified(PageSize);
    for (u64 offset = 0x6c; offset < 0xa80; offset += 0x100) {
        Store(host, offset, 0xf9);
        for (u64 i = offset; i < offset + 4; ++i) gpu_modified[i] = true;
    }
    const Page fault_snapshot = guest;
    for (u64 offset = 0xaa0; offset < PageSize; offset += 4) Store(guest, offset, 0x1234);
    for (u64 offset = 0xaa0; offset < PageSize; offset += 4) Store(host, offset, 0x1234);

    Page old_host = host;
    std::vector<bool> old_modified = gpu_modified;
    Invalidate(old_host, old_modified, guest, {{0, PageSize}});
    check(Load(old_host, 0x6c) == 0, "reporting the whole page loses the GPU counters (the bug)");

    Invalidate(host, gpu_modified, guest, Written(guest, fault_snapshot));
    bool kept = true;
    for (u64 offset = 0x6c; offset < 0xa80; offset += 0x100) kept &= Load(host, offset) == 0xf9 && gpu_modified[offset];
    check(kept, "the GPU counters next to a CPU write are kept");
    check(Load(host, 0xaa0) == 0x1234 && Load(host, PageSize - 4) == 0x1234, "the CPU write reaches the host buffer");

    // The GPU writes a counter at 0xaa8 and the CPU writes 4 bytes at 0xaa0, in the same 64-byte
    // block. The ranges reach the buffer cache through the GPU dirty memory manager.
    {
        constexpr u64 base = 0x19838000;
        Page guest{};
        Page host = guest;
        std::vector<bool> gpu_modified(PageSize);
        Store(host, 0xaa8, 0xf9);
        for (u64 i = 0xaa8; i < 0xaac; ++i) gpu_modified[i] = true;
        const Page before = guest;
        Store(guest, 0xaa0, 0x1234);
        Store(host, 0xaa0, 0x1234);
        const Ranges cpu = Written(guest, before);
        check(cpu.size() == 1 && cpu[0] == std::pair<u64, u64>{0xaa0, 4}, "the CPU write is one word");

        Page block_host = host;
        std::vector<bool> block_modified = gpu_modified;
        Invalidate(block_host, block_modified, guest,
                   Gathered(base, cpu, [](auto& m, u64 a, u64 n) { m.Collect(a, n); }));
        check(Load(block_host, 0xaa8) == 0, "64-byte collection loses the GPU counter (the bug)");

        const Ranges exact = Gathered(base, cpu, [](auto& m, u64 a, u64 n) { m.CollectExact(a, n); });
        check(exact == cpu, "exact collection invalidates only the CPU write");
        Invalidate(host, gpu_modified, guest, exact);
        check(Load(host, 0xaa8) == 0xf9 && gpu_modified[0xaa8], "the GPU counter in the same block is kept");
        check(Load(host, 0xaa0) == 0x1234, "the CPU write in the same block reaches the host buffer");
    }

    // CPU writes on both sides of an undownloaded GPU counter must not erase it.
    // Exercise holes inside one scan block and across adjacent scan blocks.
    for (const auto& offsets : {std::array<u64, 3>{0x100, 0x104, 0x108},
                               std::array<u64, 3>{0x13c, 0x140, 0x144}}) {
        constexpr u64 base = 0x19838000;
        Page guest{};
        Page host{};
        std::vector<bool> gpu_modified(PageSize);
        const Page before = guest;
        Store(host, offsets[1], 0xf9);
        for (u64 i = offsets[1]; i < offsets[1] + 4; ++i) gpu_modified[i] = true;
        Store(guest, offsets[0], 1);
        Store(guest, offsets[2], 2);
        const Ranges cpu = Written(guest, before);
        check(cpu == Ranges{{offsets[0], 4}, {offsets[2], 4}},
              "unchanged words between CPU stores are not invalidated");
        Invalidate(host, gpu_modified, guest,
                   Gathered(base, cpu, [](auto& m, u64 a, u64 n) { m.CollectExact(a, n); }));
        check(Load(host, offsets[1]) == 0xf9 && gpu_modified[offsets[1]],
              "the GPU counter between CPU stores is kept");
        check(Load(host, offsets[0]) == 1 && Load(host, offsets[2]) == 2,
              "both CPU stores reach the host buffer");
    }

    std::printf("NCE written ranges: %u failures\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
