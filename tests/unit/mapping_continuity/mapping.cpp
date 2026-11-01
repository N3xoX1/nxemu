// SPDX-License-Identifier: GPL-2.0-or-later
#include "../fast_copy/fixture.h"
#include "nxemu-video/video_settings.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
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
void Benchmark() {
    Fixture f;
    f.Map(0, Count, 0, true);
    std::vector<u8> output(Count * Page);
    using Clock = std::chrono::steady_clock;
    for (unsigned repeat = 0; repeat < 7; ++repeat) {
        auto begin = Clock::now();
        for (unsigned i = 0; i < 10000; ++i)
            f.device.ReadBlockUnsafe(Base, output.data(), output.size());
        auto read = std::chrono::duration<double, std::micro>(Clock::now() - begin).count() / 10000;
        Check(output[Page] == 2, "benchmark read result");
        begin = Clock::now();
        for (unsigned i = 0; i < 10000; ++i) {
            f.Unmap(0, Count);
            f.Map(0, Count, 0, true);
        }
        auto map = std::chrono::duration<double, std::micro>(Clock::now() - begin).count() / 10000;
        begin = Clock::now();
        for (unsigned i = 0; i < 10000; ++i) {
            f.Unmap(Count - 1, 1);
            f.Map(Count - 1, 1, Count - 1, true);
            f.device.TrackContinuity(Base, 0, Count * Page, f.asid);
        }
        auto suffix =
            std::chrono::duration<double, std::micro>(Clock::now() - begin).count() / 10000;
        std::printf("%u,%.6f,%.6f,%.6f\n", repeat, read, map, suffix);
    }
}
int main(int argc, char **argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--benchmark")
            Benchmark();
        else
            Tests();
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
