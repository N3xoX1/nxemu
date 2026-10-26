// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Minimal plaintext game containers for tests: the same files packed as an NSP
// (PFS0) or as the secure partition of an XCI, at different offsets.
namespace ContainerFixture {
using Files = std::vector<std::pair<std::string, std::string>>;
inline void Put(std::string& bytes, size_t offset, uint64_t value, size_t length) {
    for (size_t i = 0; i < length; ++i) bytes.at(offset + i) = static_cast<char>(value >> (8 * i));
}
inline std::string Partition(const Files& files, bool hfs = false) {
    std::string names, payload;
    const size_t stride = hfs ? 64 : 24;
    std::string out(16 + files.size() * stride, '\0');
    out.replace(0, 4, hfs ? "HFS0" : "PFS0");
    Put(out, 4, files.size(), 4);
    for (size_t i = 0; i < files.size(); ++i) {
        Put(out, 16 + i * stride, payload.size(), 8);
        Put(out, 24 + i * stride, files[i].second.size(), 8);
        Put(out, 32 + i * stride, names.size(), 4);
        names += files[i].first;
        names += '\0';
        payload += files[i].second;
    }
    Put(out, 8, names.size(), 4);
    return out + names + payload;
}
inline std::string Nsp(const Files& files) { return Partition(files); }
inline std::string Xci(const Files& files) {
    std::string card(0xf000, '\0');
    card.replace(0x100, 4, "DXCI");
    Put(card, 0x130, card.size(), 8);
    return card + Partition({{"update", Partition({}, true)}, {"secure", Partition(files, true)}}, true);
}
} // namespace ContainerFixture
