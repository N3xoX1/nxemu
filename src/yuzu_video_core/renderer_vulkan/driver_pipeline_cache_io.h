// SPDX-FileCopyrightText: Copyright 2026 NXEmu contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Vulkan::DriverPipelineCacheIO {
namespace Detail {
[[noreturn]] inline void IOError(const char* operation) {
    throw std::system_error(errno ? errno : EIO, std::generic_category(), operation);
}

struct FileOperations {
    static std::FILE* OpenExclusive(const std::filesystem::path& path) {
#ifdef _WIN32
        const int fd = _wopen(path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
                             _S_IREAD | _S_IWRITE);
#else
        const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
#endif
        if (fd < 0) return nullptr;
#ifdef _WIN32
        std::FILE* file = _fdopen(fd, "wb");
#else
        std::FILE* file = ::fdopen(fd, "wb");
#endif
        if (!file) {
            const int error = errno;
#ifdef _WIN32
            _close(fd);
#else
            ::close(fd);
#endif
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            errno = error;
        }
        return file;
    }

    static void Write(std::FILE* file, std::span<const char> bytes) {
        if (bytes.empty()) return;
        errno = 0;
        if (std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size())
            IOError("Write Vulkan driver cache");
    }

    static void Commit(std::FILE* file) {
        errno = 0;
        if (std::fflush(file) != 0) IOError("Flush Vulkan driver cache");
        int result;
        do {
#ifdef _WIN32
            result = _commit(_fileno(file));
#else
            result = ::fsync(::fileno(file));
#endif
        } while (result != 0 && errno == EINTR);
        if (result != 0) IOError("Commit Vulkan driver cache");
    }

    // fclose disassociates the stream even when flushing/closing reports failure.
    static void Close(std::FILE* file) {
        errno = 0;
        if (std::fclose(file) != 0) IOError("Close Vulkan driver cache");
    }

    static void Replace(const std::filesystem::path& temporary,
                        const std::filesystem::path& filename) {
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), filename.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const std::error_code error{static_cast<int>(GetLastError()), std::system_category()};
            throw std::filesystem::filesystem_error("Replace Vulkan driver cache", temporary,
                                                     filename, error);
        }
#else
        std::filesystem::rename(temporary, filename);
#endif
    }
};
} // namespace Detail

// A writer owns its exclusively created temporary sibling. Concurrent instances
// cannot truncate one another's staging file; a failed save never removes the cache.
// Operations also lets the regression tests inject I/O failures into the real path.
template <class Operations = Detail::FileOperations>
void SaveAtomically(const std::filesystem::path& filename, std::span<const char> magic,
                    std::uint32_t version, std::span<const char> payload,
                    const Operations& operations = {}) {
    if (payload.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
        throw std::length_error("Vulkan driver cache exceeds stream size limit");

    static std::atomic<std::uint64_t> serial{};
    std::filesystem::path temporary;
    std::FILE* file{};
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        temporary = filename;
        temporary += ".tmp." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "." + std::to_string(serial.fetch_add(1, std::memory_order_relaxed));
        file = operations.OpenExclusive(temporary);
        if (file) break;
        if (errno != EEXIST) Detail::IOError("Create temporary Vulkan driver cache");
    }
    if (!file) Detail::IOError("Create unique temporary Vulkan driver cache");

    try {
        operations.Write(file, magic);
        operations.Write(file, {reinterpret_cast<const char*>(&version), sizeof(version)});
        operations.Write(file, payload);
        operations.Commit(file);
        operations.Close(std::exchange(file, nullptr));
        operations.Replace(temporary, filename);
    } catch (...) {
        if (file) std::fclose(file);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

enum class LoadStatus { Loaded, Unavailable, Truncated, InvalidHeader, ReadFailure, TooLarge };
struct LoadResult {
    LoadStatus status;
    std::vector<char> payload;
};

namespace Detail {
// Stream state is checked explicitly: malformed/short files are ordinary cache
// misses, without relying on an implementation-specific ios_base::failure type.
inline LoadResult Read(std::istream& file, std::streamoff size, std::span<const char> magic,
                       std::uint32_t expected_version) {
    if (size < 0) return {LoadStatus::ReadFailure, {}};
    const std::uintmax_t header_size = magic.size() + sizeof(expected_version);
    const auto total_size = static_cast<std::uintmax_t>(size);
    if (total_size < header_size) return {LoadStatus::Truncated, {}};
    const auto payload_size = total_size - header_size;
    if (payload_size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()) ||
        payload_size > std::vector<char>{}.max_size())
        return {LoadStatus::TooLarge, {}};

    std::vector<char> actual_magic(magic.size());
    std::uint32_t version{};
    if (!file.read(actual_magic.data(), static_cast<std::streamsize>(actual_magic.size())) ||
        !file.read(reinterpret_cast<char*>(&version), sizeof(version)))
        return {LoadStatus::ReadFailure, {}};
    if (!std::equal(actual_magic.begin(), actual_magic.end(), magic.begin()) ||
        version != expected_version)
        return {LoadStatus::InvalidHeader, {}};

    std::vector<char> payload(static_cast<std::size_t>(payload_size));
    if (!payload.empty() && !file.read(payload.data(), static_cast<std::streamsize>(payload.size())))
        return {LoadStatus::ReadFailure, {}};
    return {LoadStatus::Loaded, std::move(payload)};
}
} // namespace Detail

inline LoadResult Load(const std::filesystem::path& filename, std::span<const char> magic,
                       std::uint32_t expected_version) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {LoadStatus::Unavailable, {}};
    const auto end = file.tellg();
    if (end == std::streampos{-1}) return {LoadStatus::ReadFailure, {}};
    if (!file.seekg(0, std::ios::beg)) return {LoadStatus::ReadFailure, {}};
    return Detail::Read(file, static_cast<std::streamoff>(end), magic, expected_version);
}

inline const char* StatusName(LoadStatus status) {
    switch (status) {
    case LoadStatus::Loaded: return "wrapper-valid";
    case LoadStatus::Unavailable: return "missing-or-unreadable";
    case LoadStatus::Truncated: return "truncated-wrapper";
    case LoadStatus::InvalidHeader: return "invalid-wrapper";
    case LoadStatus::ReadFailure: return "read-failure";
    case LoadStatus::TooLarge: return "size-out-of-range";
    }
    return "unknown";
}
} // namespace Vulkan::DriverPipelineCacheIO
