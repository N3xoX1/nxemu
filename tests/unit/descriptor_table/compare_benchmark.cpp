// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "yuzu_common/differential_memory_copy.h"

namespace {
using Compare = Common::ReadbackDetail::Comparer;
bool Standard(const unsigned char* a, const unsigned char* b, size_t size) {
    return std::memcmp(a, b, size) == 0;
}
double Measure(Compare compare, const std::vector<unsigned char>& a,
               const std::vector<unsigned char>& b, size_t start, size_t& checksum) {
    constexpr size_t passes = 64;
    const auto begin = std::chrono::steady_clock::now();
    for (size_t pass = 0; pass < passes; ++pass) {
        for (size_t offset = start; offset < a.size();) {
            const size_t bytes = std::min(size_t{4096}, a.size() - offset);
            checksum += compare(a.data() + offset, b.data() + offset, bytes);
            offset += bytes;
        }
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() / passes;
}
}

int main() {
    constexpr size_t size = 16 * 1024 * 1024;
    std::puts("start_offset,mismatch_stride,memcmp_ms,vector_equal_ms,speedup,checksum");
    for (const size_t start : {size_t{0}, size_t{1}, size_t{63}}) {
        for (const size_t stride : {size_t{0}, size_t{97}, size_t{1}}) {
            std::vector<unsigned char> a(size + start, 0x2A), b = a;
            if (stride) for (size_t offset = start; offset < a.size(); offset += stride * 4096)
                b[offset + std::min(size_t{4095}, a.size() - offset - 1)] ^= 1;
            std::vector<double> old_times, new_times;
            size_t expected{}, actual{};
            const auto optimized = Common::ReadbackDetail::SelectComparer();
            for (size_t trial = 0; trial < 7; ++trial) {
                size_t old_sum{}, new_sum{};
                double old_ms{}, new_ms{};
                if (trial % 2) {
                    new_ms = Measure(optimized, a, b, start, new_sum);
                    old_ms = Measure(Standard, a, b, start, old_sum);
                } else {
                    old_ms = Measure(Standard, a, b, start, old_sum);
                    new_ms = Measure(optimized, a, b, start, new_sum);
                }
                if (old_sum != new_sum) std::abort();
                expected += old_sum;
                actual += new_sum;
                old_times.push_back(old_ms);
                new_times.push_back(new_ms);
            }
            std::sort(old_times.begin(), old_times.end());
            std::sort(new_times.begin(), new_times.end());
            std::printf("%zu,%zu,%.6f,%.6f,%.3f,%zu\n", start, stride,
                        old_times[3], new_times[3], old_times[3] / new_times[3], actual);
            if (expected != actual) return 1;
        }
    }
}
