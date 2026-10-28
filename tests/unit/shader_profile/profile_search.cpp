// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile_search.h"
#include <iostream>
#include <random>

using VideoCommon::ShaderProfile::PrefixSearch;
int main() {
    std::mt19937 random(481);
    for (unsigned trial = 0; trial < 180; ++trial) {
        std::vector<PrefixSearch::Prefix> patterns(1 + random() % 24);
        for (auto& p : patterns) for (auto& b : p) b = random() % (trial % 3 ? 256 : 3);
        if (trial % 4 == 0) patterns.push_back(patterns.front());
        std::vector<unsigned char> data(trial < 20 ? trial : 4096 + random() % 4096);
        for (auto& b : data) b = random() % (trial % 3 ? 256 : 3);
        for (size_t pos = 13; pos + 16 <= data.size(); pos += 59)
            std::copy(patterns[pos % patterns.size()].begin(), patterns[pos % patterns.size()].end(), data.begin() + pos);
        if (data.size() >= 16) std::copy(patterns[0].begin(), patterns[0].end(), data.end() - 16);
        std::vector<size_t> expected;
        for (size_t pos = 0; pos + 16 <= data.size();) {
            bool hit = false;
            for (const auto& p : patterns) if (!std::memcmp(data.data() + pos, p.data(), 16)) { hit = true; break; }
            if (hit) { expected.push_back(pos); pos += 16; } else ++pos;
        }
        PrefixSearch search(patterns);
        std::vector<size_t> actual;
        size_t resume = 0;
        std::array<size_t, 3> batch;
        while (resume < data.size()) {
            const auto count = search.Scan(data, resume, batch);
            actual.insert(actual.end(), batch.begin(), batch.begin() + count);
        }
        if (actual != expected) { std::cerr << "Prefix mismatch trial " << trial; return 1; }
    }
    // Exercise zero-word skipping and dense overlapping matches.
    PrefixSearch::Prefix zero{};
    PrefixSearch search(std::span(&zero, 1));
    std::vector<unsigned char> zeros(1024);
    size_t resume = 0;
    std::array<size_t, 100> hits{};
    if (search.Scan(zeros, resume, hits) != 64 || hits[63] != 1008) return 2;
    return 0;
}
