// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace VideoCommon::ShaderProfile {
// The same game content is packed differently by an NSP and an XCI. Profiles
// therefore locate shaders inside the NCA files these containers hold, which
// keep their name and bytes whichever container carries them.
struct ContainerEntry {
    std::string name;
    uint64_t offset{}, size{}; // From the start of the container file.
};
namespace ContainerDetail {
inline void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
inline uint64_t Get(std::string_view bytes, size_t pos, size_t count) {
    Check(pos <= bytes.size() && count <= bytes.size() - pos, "Container header bounds");
    uint64_t value{};
    for (size_t i = 0; i < count; ++i) value |= uint64_t{static_cast<unsigned char>(bytes[pos + i])} << (8 * i);
    return value;
}
inline void Bounds(uint64_t offset, uint64_t size, uint64_t total) {
    Check(offset <= total && size <= total - offset, "Container storage bounds");
}
} // namespace ContainerDetail

// Files of a plaintext PFS0 or HFS0 partition of `size` bytes. read(offset, length)
// returns bytes of that partition; entry offsets are relative to it.
template <class Read>
std::vector<ContainerEntry> PartitionEntries(uint64_t size, Read&& read) {
    using namespace ContainerDetail;
    Check(size >= 16, "Truncated partition");
    const std::string h = read(0, 16);
    const bool hfs = h.substr(0, 4) == "HFS0";
    Check(hfs || h.substr(0, 4) == "PFS0", "Expected plaintext PFS0/HFS0 game container");
    const auto count = Get(h, 4, 4), strings = Get(h, 8, 4);
    const uint64_t stride = hfs ? 64 : 24;
    Check(count <= 65536 && strings <= 16 * 1024 * 1024, "PFS metadata limit");
    Bounds(16, count * stride + strings, size);
    const std::string table = read(16, count * stride + strings);
    const auto payload = 16 + count * stride + strings;
    std::vector<ContainerEntry> files;
    for (uint64_t i = 0; i < count; ++i) {
        const auto off = Get(table, i * stride, 8), length = Get(table, i * stride + 8, 8), name = Get(table, i * stride + 16, 4);
        Check(name < strings, "PFS string bounds");
        const auto begin = static_cast<size_t>(count * stride + name);
        const auto end = table.find('\0', begin);
        Check(end != std::string::npos && end < count * stride + strings, "PFS string termination");
        Bounds(payload, off, size);
        Bounds(payload + off, length, size);
        files.push_back({table.substr(begin, end - begin), payload + off, length});
    }
    return files;
}

// Files of an NSP, or of the secure partition of an XCI (where its NCAs are).
template <class Read>
std::vector<ContainerEntry> ContainerEntries(uint64_t size, Read&& read) {
    using namespace ContainerDetail;
    Check(size >= 16, "Truncated game container");
    const std::string magic = read(0, 4);
    if (magic == "PFS0" || magic == "HFS0") return PartitionEntries(size, read);
    Check(size >= 0x200, "Truncated gamecard header");
    const std::string header = read(0x100, 0x100);
    Check(header.substr(0, 4) == "DXCI" || header.substr(0, 4) == "HEAD", "Unsupported game container");
    const auto root = Get(header, 0x30, 8);
    Bounds(root, 16, size);
    const auto in = [&](uint64_t base, uint64_t length) {
        return [&read, base, length](uint64_t off, uint64_t n) {
            Bounds(off, n, length);
            return read(base + off, n);
        };
    };
    for (const auto& partition : PartitionEntries(size - root, in(root, size - root)))
        if (partition.name == "secure") {
            const auto base = root + partition.offset;
            auto files = PartitionEntries(partition.size, in(base, partition.size));
            for (auto& file : files) file.offset += base;
            return files;
        }
    throw std::runtime_error("Gamecard secure partition missing");
}

inline bool IsGameContainerName(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos) return false;
    std::string extension(name.substr(dot + 1));
    for (auto& c : extension) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return extension == "dnsp" || extension == "nsp" || extension == "dxci" || extension == "xci";
}
} // namespace VideoCommon::ShaderProfile
