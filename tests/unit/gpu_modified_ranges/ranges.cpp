// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

#include "yuzu_common/range_sets.inc"
#include "yuzu_video_core/buffer_cache/gpu_modified_ranges.h"

namespace {

constexpr u64 Page = 64;
constexpr u64 Pages = 16;
constexpr u64 Bytes = Page * Pages;

using Ranges = VideoCommon::GpuModifiedRanges<u64, Page>;

unsigned checks{};

void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        throw std::runtime_error(what);
    }
}

struct Fixture {
    Ranges ranges;
    std::array<s32, Pages> protect_counts{};
    std::vector<std::pair<u64, u64>> notified;

    Fixture() {
        ranges.SetOnPages([this](u64 addr, u64 size, s32 delta) {
            Check(addr % Page == 0 && size % Page == 0 && size != 0, "notified range is whole pages");
            for (u64 page = addr / Page; page < (addr + size) / Page; ++page) {
                protect_counts[page] += delta;
                Check(protect_counts[page] == 0 || protect_counts[page] == 1, "page notified once per change");
            }
            notified.emplace_back(addr, addr + size);
        });
    }

    std::vector<std::pair<u64, u64>> Gaps(u64 addr, u64 size) const {
        std::vector<std::pair<u64, u64>> gaps;
        ranges.ForEachNotInRange(addr, size, [&](u64 start, u64 end) { gaps.emplace_back(start, end); });
        return gaps;
    }
};

void TestPagesFollowRanges() {
    Fixture f;
    f.ranges.Add(10, 20);
    Check(f.notified == std::vector<std::pair<u64, u64>>{{0, 64}}, "first range protects its page");
    f.ranges.Add(40, 10);
    Check(f.notified.size() == 1, "second range on a protected page notifies nothing");
    f.ranges.Add(60, 100);
    Check(f.notified.back() == std::pair<u64, u64>{64, 192}, "only the new pages are protected");
    f.ranges.Subtract(10, 20);
    Check(f.notified.size() == 2, "page with another range stays protected");
    f.ranges.Subtract(64, 64);
    Check(f.notified.back() == std::pair<u64, u64>{64, 128}, "emptied interior page is released");
    f.ranges.Subtract(0, Bytes);
    Check(f.protect_counts == std::array<s32, Pages>{}, "every page is released");
    Check(f.ranges.Empty(), "no range is left");
}

void TestGaps() {
    Fixture f;
    f.ranges.Add(100, 50);
    f.ranges.Add(200, 10);
    using G = std::vector<std::pair<u64, u64>>;
    Check(f.Gaps(64, 192) == G{{64, 100}, {150, 200}, {210, 256}}, "gaps around ranges");
    Check(f.Gaps(100, 50).empty(), "no gap inside a range");
    Check(f.Gaps(0, 64) == G{{0, 64}}, "a range-free span is one gap");
}

void TestRandomAgainstBytes() {
    std::mt19937 rng{1234};
    for (int round = 0; round < 200; ++round) {
        Fixture f;
        std::array<bool, Bytes> gpu{};
        for (int op = 0; op < 64; ++op) {
            const u64 addr = rng() % Bytes;
            const u64 size = 1 + rng() % std::min<u64>(Bytes - addr, 3 * Page);
            const bool add = rng() % 2 == 0;
            if (add) {
                f.ranges.Add(addr, size);
            } else {
                f.ranges.Subtract(addr, size);
            }
            std::fill_n(gpu.begin() + addr, size, add);
            for (u64 page = 0; page < Pages; ++page) {
                bool any = false;
                for (u64 byte = page * Page; byte < (page + 1) * Page; ++byte) {
                    any |= gpu[byte];
                }
                Check(f.protect_counts[page] == (any ? 1 : 0), "protected pages are the ones with GPU bytes");
            }
            const u64 query = rng() % Bytes;
            const u64 query_size = 1 + rng() % (Bytes - query);
            std::array<bool, Bytes> covered{};
            for (const auto& [start, end] : f.Gaps(query, query_size)) {
                Check(start < end, "gaps are not empty");
                for (u64 byte = start; byte < end; ++byte) {
                    covered[byte] = true;
                }
            }
            for (u64 byte = query; byte < query + query_size; ++byte) {
                Check(covered[byte] == !gpu[byte], "gaps are exactly the bytes without GPU writes");
            }
        }
    }
}

} // namespace

int main() {
    try {
        TestPagesFollowRanges();
        TestGaps();
        TestRandomAgainstBytes();
    } catch (const std::exception& e) {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
    std::printf("gpu_modified_ranges: %u checks passed\n", checks);
    return 0;
}
