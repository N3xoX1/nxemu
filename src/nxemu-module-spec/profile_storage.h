// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Shared, host-side disk layout used by the guest account manager and frontend.
// Keep these POD sizes unchanged: profiles.dat is an existing binary format.
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <optional>
#include <type_traits>

namespace HostProfileStorage {

constexpr std::size_t MaxUsers = 8;
constexpr std::size_t UsernameBytes = 32;
constexpr std::size_t ExtraBytes = 0x80;

struct UserRaw {
    std::array<std::uint8_t, 16> uuid{};
    std::array<std::uint8_t, 16> uuid2{};
    std::uint64_t timestamp{};
    std::array<std::uint8_t, UsernameBytes> username{};
    std::array<std::uint8_t, ExtraBytes> extra_data{};
};

struct FileRaw {
    std::array<std::uint8_t, 0x10> reserved{};
    std::array<UserRaw, MaxUsers> users{};
};

static_assert(sizeof(UserRaw) == 0xC8);
static_assert(sizeof(FileRaw) == 0x650);
static_assert(offsetof(UserRaw, timestamp) == 0x20);
static_assert(offsetof(UserRaw, extra_data) == 0x48);
static_assert(std::is_trivially_copyable_v<FileRaw>);

inline std::filesystem::path FilePath(const std::filesystem::path& nand_directory) {
    return nand_directory / "system/save/8000000000000010/su/avators/profiles.dat";
}

// A missing, truncated or inaccessible file is NOT interpreted as an empty
// database: callers must not overwrite it with an empty snapshot.
inline std::optional<FileRaw> Read(const std::filesystem::path& file_path) {
    std::ifstream stream(file_path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    FileRaw result{};
    if (!stream.read(reinterpret_cast<char*>(&result), sizeof(result))) {
        return std::nullopt;
    }
    return result;
}

inline bool Write(const std::filesystem::path& file_path, const FileRaw& data) {
    std::error_code error;
    std::filesystem::create_directories(file_path.parent_path(), error);
    if (error) {
        return false;
    }
    std::ofstream stream(file_path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream.write(reinterpret_cast<const char*>(&data), sizeof(data));
    stream.flush();
    return stream.good();
}

} // namespace HostProfileStorage
