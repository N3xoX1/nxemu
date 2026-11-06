// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace VideoCommon::ShaderProfile {
// An .nx archive keeps the NCA files of a game, its updates and DLC compressed,
// so profiles read them through the NX reader instead of at a file offset. The
// NCA names and bytes are those of the NSP or XCI the archive came from.
class NxGame {
public:
    struct Nca {
        std::string name;
        uint64_t size{};
        uint32_t file{};
    };

    explicit NxGame(const std::filesystem::path& path); // Throws when unreadable.
    ~NxGame();
    NxGame(const NxGame&) = delete;
    NxGame& operator=(const NxGame&) = delete;

    // The NCAs of every snapshot, each name once.
    std::vector<Nca> AllContents() const;
    // The NCAs the game starts with, as the loader picks them: the base of
    // `title_id` and its most recent update.
    std::vector<Nca> PlayedContents(uint64_t title_id) const;
    // Thread-safe; throws on a range or read error.
    void Read(uint32_t file, uint64_t offset, std::span<char> bytes);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

inline bool IsNxName(std::string_view name) {
    if (name.size() < 3) return false;
    const auto tail = name.substr(name.size() - 3);
    return tail[0] == '.' && (tail[1] | 0x20) == 'n' && (tail[2] | 0x20) == 'x';
}
} // namespace VideoCommon::ShaderProfile
