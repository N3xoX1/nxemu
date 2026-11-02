// SPDX-License-Identifier: GPL-2.0-or-later
#include "../fast_copy/fixture.h"
#include "nxemu-video/video_settings.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>
VideoSettings videoSettings{};
unsigned checks{};
void Check(bool ok, const char *what) {
    ++checks;
    if (!ok)
        throw std::runtime_error(what);
}
constexpr size_t Page = 4096, Count = 64;
constexpr DAddr Base = 0x100000;
struct NullableMemory : TestMemory {
    u64 null_address = ~u64{};
    u8 *GetPointerSilent(u64 address) override {
        return address == null_address ? nullptr : TestMemory::GetPointerSilent(address);
    }
};
struct Fixture {
    NullableMemory memory;
    TestRasterizer rasterizer;
    Tegra::MaxwellDeviceMemoryManager device{memory};
    Core::Asid asid;
    std::array<int, Count> oracle;
    Fixture() : asid(device.RegisterProcess(&memory)) {
        device.BindInterface(&rasterizer);
        oracle.fill(-1);
        for (size_t i = 0; i < 128; ++i)
            std::fill_n(memory.bytes.data() + i * Page, Page, u8(i + 1));
    }
    void Map(size_t first, size_t n, size_t physical, bool track) {
        device.Map(Base + first * Page, physical * Page, n * Page, asid, track);
        for (size_t i = 0; i < n; ++i)
            oracle[first + i] = int(physical + i);
    }
    void Unmap(size_t first, size_t n) {
        device.Unmap(Base + first * Page, n * Page);
        std::fill_n(oracle.begin() + first, n, -1);
    }
    void Verify(size_t first, size_t bytes, size_t offset = 0) {
        if (device.GetSpan(Base + first * Page + offset, bytes) != nullptr) {
            for (size_t p = first; p <= first + (offset + bytes - 1) / Page; ++p)
                Check(oracle[p] >= 0 && oracle[p] == oracle[first] + int(p - first),
                      "GetSpan falsely accepts a hole/discontinuity");
        }
        std::vector<u8> out(bytes, 0xff);
        device.ReadBlockUnsafe(Base + first * Page + offset, out.data(), bytes);
        for (size_t i = 0; i < bytes; ++i) {
            size_t p = first + (i + offset) / Page;
            u8 expected = oracle[p] < 0 ? 0 : u8(oracle[p] + 1);
            Check(out[i] == expected, "read crosses stale run/hole/remap");
        }
    }
};
void Tests() {
    Fixture f;
    f.Map(Count, 0, 0, true);
    f.Unmap(Count, 0);
    f.device.TrackContinuity(Base + Count * Page, 0, 0, f.asid);
    f.Map(0, Count, 0, true);
    f.Verify(0, Count * Page);
    f.Map(20, 1, 100, false);
    Check(f.device.GetSpan(Base, Count * Page) == nullptr,
          "GetSpan accepts an untracked physical remap");
    f.Verify(0, Count * Page);
    f.Unmap(12, 1);
    Check(f.device.GetSpan(Base, Count * Page) == nullptr, "GetSpan accepts unmapped interior");
    f.Verify(0, Count * Page);
    f.Unmap(0, 1);
    f.Verify(0, Count * Page);
    f.Map(0, 1, 100, false);
    f.Verify(0, Count * Page);
    f.Unmap(12, 1);
    f.Map(12, 1, 110, false);
    f.Verify(0, Count * Page);
    // Retracking a suffix can shorten its run; later invalidation must still
    // reach predecessors.
    f.Unmap(0, Count);
    f.Map(0, Count, 0, true);
    f.device.TrackContinuity(Base + 10 * Page, 10 * Page, 10 * Page, f.asid);
    f.Unmap(18, 1);
    f.Verify(0, Count * Page);
    std::mt19937 random(0x4d415038);
    for (unsigned trial = 0; trial < 2000; ++trial) {
        size_t first = random() % Count, n = 1 + random() % (Count - first);
        f.Unmap(first, n);
        if (random() % 3)
            f.Map(first, n, random() % (129 - n), random() % 2);
        size_t begin = random() % Count, offset = random() % Page,
               bytes = 1 + random() % ((Count - begin) * Page - offset);
        f.Verify(begin, bytes, offset);
    }
    // A mapped run replaced by a failed CPU translation must become a one-page
    // hole.
    f.Unmap(0, Count);
    f.Map(0, Count, 0, true);
    f.Unmap(20, 1);
    f.memory.null_address = 100 * Page;
    f.device.Map(Base + 20 * Page, 100 * Page, Page, f.asid, true);
    f.Verify(0, Count * Page);
    // Compare writes against a page-by-page oracle, including untouched backing
    // behind holes.
    std::vector<u8> expected(f.memory.bytes.data(), f.memory.bytes.data() + 128 * Page),
        input(Count * Page, 0xa5);
    for (size_t page = 0; page < Count; ++page)
        if (f.oracle[page] >= 0)
            std::fill_n(expected.data() + f.oracle[page] * Page, Page, 0xa5);
    f.device.WriteBlockUnsafe(Base, input.data(), input.size());
    Check(std::equal(expected.begin(), expected.end(), f.memory.bytes.data()),
          "write crossed a hole or touched unrelated backing");
    std::fprintf(stderr, "%u byte checks passed; real Map/Unmap/Remap/WalkBlock/GetSpan\n", checks);
}
struct LargeFixture {
    TestMemory memory;
    TestRasterizer rasterizer;
    Tegra::MaxwellDeviceMemoryManager device{memory};
    Core::Asid asid{device.RegisterProcess(&memory)};
    LargeFixture() { device.BindInterface(&rasterizer); }
    void Prepare(size_t pages, bool track = false) {
        device.TrackContinuity(Base, 0, pages * Page, asid);
        device.Map(Base, 0, pages * Page, asid, track);
    }
};
void LargeTests() {
    LargeFixture f;
    constexpr size_t pages = 16384;
    std::fill_n(f.memory.bytes.data(), pages * Page, 0x5c);
    f.Prepare(pages);
    Check(f.device.GetSpan(Base, pages * Page) == f.memory.bytes.data(),
          "pretracked heap Map(track=false) must retain a full contiguous span");
    const auto &const_device = f.device;
    Check(const_device.GetSpan(Base, pages * Page) == f.memory.bytes.data(),
          "const span lost heap continuity");
    std::vector<u8> output(pages * Page);
    for (size_t hole : {0u, 63u, 64u, 4095u, 4096u, 8191u, 8192u, 16383u}) {
        f.device.Unmap(Base + hole * Page, Page);
        Check(f.device.GetSpan(Base, pages * Page) == nullptr, "large span crosses hole");
        Check(const_device.GetSpan(Base, pages * Page) == nullptr, "const span crosses hole");
        f.device.ReadBlockUnsafe(Base, output.data(), output.size());
        Check(std::all_of(output.begin() + hole * Page, output.begin() + (hole + 1) * Page,
                          [](u8 value) { return value == 0; }),
              "hole not zero filled");
        if (hole)
            Check(output[hole * Page - 1] == 0x5c, "mapped prefix lost");
        if (hole + 1 < pages)
            Check(output[(hole + 1) * Page] == 0x5c, "mapped suffix lost");
        f.device.Map(Base + hole * Page, hole * Page, Page, f.asid, false);
        Check(f.device.GetSpan(Base, pages * Page) == f.memory.bytes.data(),
              "untracked remap must merge adjoining physical runs");
    }
    // Physical discontinuities and CPU translation holes must still split the span.
    f.device.Map(Base + Page, 100 * Page, Page, f.asid, false);
    Check(f.device.GetSpan(Base, 3 * Page) == nullptr, "physical remap accepted as contiguous");
    f.device.Unmap(Base + Page, Page);
    f.device.Map(Base + Page, Page, Page, f.asid, false);
    for (size_t page = pages; page > 0; --page)
        f.device.Unmap(Base + (page - 1) * Page, Page);
    Check(f.device.GetSpan(Base, pages * Page) == nullptr, "reverse teardown left a mapping");
    // Build the heap from disjoint chunks in an order unlike virtual-address order.
    std::vector<size_t> chunks(32);
    for (size_t i = 0; i < chunks.size(); ++i)
        chunks[i] = i;
    std::mt19937 order(542);
    std::shuffle(chunks.begin(), chunks.end(), order);
    f.device.TrackContinuity(Base, 0, 8192 * Page, f.asid);
    for (size_t chunk : chunks) {
        f.device.Map(Base + chunk * 256 * Page, chunk * 256 * Page, 256 * Page, f.asid, false);
        Check(f.device.GetSpan(Base + chunk * 256 * Page, 256 * Page) != nullptr,
              "heap chunk fragmented");
    }
    Check(f.device.GetSpan(Base, 8192 * Page) == f.memory.bytes.data(),
          "disjoint heap chunks did not merge");
    for (unsigned i = 0; i < 1000; ++i) {
        f.device.Unmap(Base + 4096 * Page, Page);
        Check(f.device.GetSpan(Base, 8192 * Page) == nullptr, "middle mutation lost a boundary");
        f.device.Map(Base + 4096 * Page, 4096 * Page, Page, f.asid, false);
        Check(f.device.GetSpan(Base, 8192 * Page) != nullptr, "middle remap failed to merge");
    }
    f.device.Unmap(Base, 8192 * Page);
    // Sparse mappings across every summary level, including the final virtual page.
    for (size_t landmark : {64u, 4096u, 262144u, 4194303u}) {
        DAddr address = landmark * Page;
        f.device.Map(address, 0, Page, f.asid, false);
        Check(f.device.GetSpan(0, address + Page) == nullptr,
              "sparse mapping filled a virtual hole");
        std::array<u8, 3 * Page> sparse{};
        f.device.ReadBlockUnsafe(address - 2 * Page, sparse.data(), sparse.size());
        Check(std::all_of(sparse.begin(), sparse.begin() + 2 * Page, [](u8 v) { return v == 0; }),
              "sparse hole read");
        Check(std::all_of(sparse.begin() + 2 * Page, sparse.end(), [](u8 v) { return v == 0x5c; }),
              "sparse mapped read");
        f.device.Unmap(address, Page);
    }
    Check(f.device.GetSpan(DAddr{1} << 34, 0) == nullptr, "end-of-address-space span");
    Check(f.device.GetSpan(Base, std::numeric_limits<size_t>::max()) == nullptr,
          "overflowing span");
    // Last device page: no sentinel read beyond the continuity representation.
    constexpr DAddr last = (DAddr{1} << 34) - Page;
    f.device.Map(last, 0, Page, f.asid, false);
    Check(f.device.GetSpan(last, Page) == f.memory.bytes.data(), "last page mapping");
    f.device.Unmap(last, Page);
    std::fprintf(stderr, "Large heap continuity and reverse teardown passed\n");
}
void Benchmark(bool track) {
    LargeFixture f;
    std::fill_n(f.memory.bytes.data(), 16384 * Page, 0x5c);
    std::vector<u8> output(16384 * Page);
    using Clock = std::chrono::steady_clock;
    auto elapsed = [](auto start) {
        return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    };
    for (size_t pages : {64u, 256u, 1024u, 4096u, 8192u, 16384u}) {
        for (unsigned repeat = 0; repeat < 5; ++repeat) {
            auto start = Clock::now();
            f.Prepare(pages, track);
            auto map = elapsed(start);
            auto *span = f.device.GetSpan(Base, pages * Page);
            start = Clock::now();
            for (unsigned i = 0; i < 32; ++i)
                f.device.ReadBlockUnsafe(Base, output.data(), pages * Page);
            auto read = elapsed(start) / 32;
            Check(output[0] == 0x5c && output[pages * Page - 1] == 0x5c,
                  "benchmark transfer result");
            start = Clock::now();
            uintptr_t checksum{};
            for (unsigned i = 0; i < 200000; ++i)
                checksum += reinterpret_cast<uintptr_t>(
                    f.device.GetSpan(Base + (i % 32), pages * Page - (i % 32)));
            auto get_span = elapsed(start) / 200000;
            Check(!span || checksum != 0, "span result not observed");
            start = Clock::now();
            checksum = 0;
            for (unsigned i = 0; i < 200000; ++i)
                checksum += reinterpret_cast<uintptr_t>(f.device.GetSpan(Base + (i % 32), 8));
            auto small_span = elapsed(start) / 200000;
            Check(checksum != 0, "small span result not observed");
            start = Clock::now();
            for (size_t page = pages; page > 0; --page)
                f.device.Unmap(Base + (page - 1) * Page, Page);
            auto reverse = elapsed(start);
            std::printf("%zu,%u,%u,%.6f,%.6f,%.6f,%.6f,%.6f\n", pages, repeat,
                        unsigned(span != nullptr), map, read, get_span, reverse, small_span);
        }
    }
}
int main(int argc, char **argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--benchmark")
            Benchmark(false);
        else if (argc > 1 && std::string_view(argv[1]) == "--benchmark-tracked")
            Benchmark(true);
        else if (argc > 1 && std::string_view(argv[1]) == "--large")
            LargeTests();
        else {
            Tests();
            LargeTests();
        }
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
