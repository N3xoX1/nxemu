// SPDX-FileCopyrightText: Copyright 2026 NXEmu Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace Loader::LooseFS {

// Only names with established, ASCII spellings are compared here. Using u8string
// avoids a locale-dependent conversion of other names on Windows.
inline bool NameEqualsIgnoreCase(const std::filesystem::path& path, std::string_view expected) {
    const auto name = path.filename().u8string();
    return name.size() == expected.size() &&
           std::equal(name.begin(), name.end(), expected.begin(), [](char8_t a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

inline bool IsExeFSFolder(const std::filesystem::path& directory) {
    return NameEqualsIgnoreCase(directory, "exefs");
}

inline bool IsExeFSMain(const std::filesystem::path& path) {
    if (path.filename() != "main") {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(path.parent_path() / "main.npdm", ec) && !ec;
}

inline bool HasMainAndNpdm(const std::filesystem::path& directory) {
    std::error_code ec;
    return std::filesystem::is_regular_file(directory / "main.npdm", ec) && !ec &&
           std::filesystem::is_regular_file(directory / "main", ec) && !ec;
}

inline bool HasLooseTitle(const std::filesystem::path& directory) {
    if (HasMainAndNpdm(directory) || HasMainAndNpdm(directory / "exefs")) {
        return true;
    }

    // The VFS supports differently capitalized ExeFS names. Match it when
    // deciding whether a sibling RomFS payload may be skipped during scanning.
    std::error_code ec;
    std::filesystem::directory_iterator it{
        directory, std::filesystem::directory_options::skip_permission_denied, ec};
    for (const std::filesystem::directory_iterator end; !ec && it != end; it.increment(ec)) {
        if (IsExeFSFolder(it->path()) && HasMainAndNpdm(it->path())) {
            return true;
        }
    }
    return false;
}

inline bool CanSkipRomFS(const std::filesystem::path& directory) {
    return NameEqualsIgnoreCase(directory, "romfs") &&
           HasLooseTitle(directory.parent_path());
}

} // namespace Loader::LooseFS
