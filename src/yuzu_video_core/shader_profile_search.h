// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace VideoCommon::ShaderProfile {
// Offline discovery only. This indexes prefixes, never pipeline combinations.
// Callers must compare the complete program before recording a resource locator.
//
// Wu-Manber search: the 4-byte block ending a 16-byte window tells how far the
// window can move without skipping a possible prefix start. With thousands of
// prefixes, 2-byte pairs appear in almost every prefix and barely move the
// window; hashed 4-byte blocks keep most moves near the full 13 bytes.
class PrefixSearch {
public:
    static constexpr size_t Width = 16;
    using Prefix = std::array<unsigned char, Width>;
    explicit PrefixSearch(std::span<const Prefix> patterns) : shifts(TableSize, MaxShift) {
        if (patterns.empty()) throw std::invalid_argument("Empty shader prefix index");
        for (const auto& pattern : patterns) {
            first_bytes[pattern[0]] = true;
            for (size_t end = Block; end <= Width; ++end) {
                auto& shift = shifts[Hash(pattern.data() + end - Block)];
                shift = std::min(shift, static_cast<unsigned char>(Width - end));
            }
            sorted.push_back(pattern);
        }
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    }

    // Positions are ascending and non-overlapping, like the original prefix regex.
    // Bounded batches let callers check full programs without retaining all hits.
    // Resume and data refer to the same input buffer; no bytes are read past it.
    size_t Scan(std::span<const unsigned char> data, size_t& resume,
                std::span<size_t> positions) const {
        size_t count = 0;
        if (positions.empty()) return count;
        while (resume <= data.size() && data.size() - resume >= Width) {
            // Containers contain large zero-filled areas. Skip a word only when
            // zero cannot start any pattern, preserving every possible start.
            if (!first_bytes[0] && data[resume] == 0) {
                std::uint64_t word;
                std::memcpy(&word, data.data() + resume, sizeof(word));
                if (word == 0) { resume += sizeof(word); continue; }
            }
            if (const auto shift = shifts[Hash(data.data() + resume + Width - Block)]) {
                resume += shift;
                continue;
            }
            const auto* at = data.data() + resume;
            const bool found = first_bytes[*at] && std::binary_search(sorted.begin(), sorted.end(), at, Less{});
            if (found) {
                positions[count++] = resume;
                resume += Width;
                if (count == positions.size()) return count;
            } else {
                ++resume;
            }
        }
        resume = data.size();
        return count;
    }

private:
    static constexpr size_t Block = 4, Bits = 20, TableSize = size_t{1} << Bits;
    static constexpr unsigned char MaxShift = Width - Block + 1;
    static unsigned Hash(const unsigned char* p) {
        std::uint32_t block;
        std::memcpy(&block, p, sizeof(block));
        return (block * 2654435761u) >> (32 - Bits);
    }
    struct Less {
        bool operator()(const Prefix& a, const unsigned char* b) const { return std::memcmp(a.data(), b, Width) < 0; }
        bool operator()(const unsigned char* a, const Prefix& b) const { return std::memcmp(a, b.data(), Width) < 0; }
    };
    std::vector<unsigned char> shifts;
    std::array<bool, 256> first_bytes{};
    std::vector<Prefix> sorted;
};
} // namespace VideoCommon::ShaderProfile
