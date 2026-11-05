// SPDX-FileCopyrightText: Copyright 2026 NXEmu Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Standalone regression test (no emulator runtime required):
// Compile with C++20, -Isrc/nxemu-loader, then run the executable.

#include "core/loader/loose_romfs_paths.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

struct TempDirectory {
    fs::path path;

    TempDirectory() {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("nxemu-romfs-paths-" + std::to_string(unique));
        assert(fs::create_directories(path));
    }

    ~TempDirectory() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

static void Touch(const fs::path& path) {
    std::ofstream file{path, std::ios::binary};
    assert(file.good());
}

int main() {
    TempDirectory tmp;
    const auto& base = tmp.path;

    // Canonical extracted title: game/exefs/main and game/romfs/...
    fs::create_directories(base / "game" / "exefs");
    fs::create_directories(base / "game" / "romfs" / "assets");
    Touch(base / "game" / "exefs" / "main");
    Touch(base / "game" / "exefs" / "main.npdm");
    Touch(base / "game" / "romfs" / "assets" / "main");
    assert(Loader::LooseFS::HasLooseTitle(base / "game"));
    assert(Loader::LooseFS::IsExeFSMain(base / "game" / "exefs" / "main"));
    assert(Loader::LooseFS::CanSkipRomFS(base / "game" / "romfs"));
    assert(!Loader::LooseFS::IsExeFSMain(base / "game" / "romfs" / "assets" / "main"));

    // Uppercase directory names, preserving the real on-disk spelling.
    fs::create_directories(base / "upper" / "EXEFS");
    fs::create_directories(base / "upper" / "ROMFS");
    Touch(base / "upper" / "EXEFS" / "main");
    Touch(base / "upper" / "EXEFS" / "main.npdm");
    assert(Loader::LooseFS::HasLooseTitle(base / "upper"));
    assert(Loader::LooseFS::CanSkipRomFS(base / "upper" / "ROMFS"));

    // A directly selected ExeFS directory with a sibling RomFS.
    fs::create_directories(base / "flat" / "romfs");
    Touch(base / "flat" / "main");
    Touch(base / "flat" / "main.npdm");
    assert(Loader::LooseFS::HasLooseTitle(base / "flat"));
    assert(Loader::LooseFS::CanSkipRomFS(base / "flat" / "romfs"));

    // Mixed case, and only folders really named exefs count as an ExeFS folder.
    fs::create_directories(base / "mixed" / "ExeFS");
    fs::create_directories(base / "mixed" / "RomFS");
    Touch(base / "mixed" / "ExeFS" / "main");
    Touch(base / "mixed" / "ExeFS" / "main.npdm");
    assert(Loader::LooseFS::IsExeFSFolder(base / "mixed" / "ExeFS"));
    assert(!Loader::LooseFS::IsExeFSFolder(base / "flat"));
    assert(!Loader::LooseFS::IsExeFSFolder(base / "exefs2"));
    assert(Loader::LooseFS::IsExeFSMain(base / "mixed" / "ExeFS" / "main"));
    assert(Loader::LooseFS::CanSkipRomFS(base / "mixed" / "RomFS"));

    // main.npdm alone, or with a folder named main, is not a title.
    fs::create_directories(base / "npdm_only" / "exefs");
    fs::create_directories(base / "npdm_only" / "romfs");
    Touch(base / "npdm_only" / "exefs" / "main.npdm");
    assert(!Loader::LooseFS::HasLooseTitle(base / "npdm_only"));
    assert(!Loader::LooseFS::CanSkipRomFS(base / "npdm_only" / "romfs"));
    fs::create_directories(base / "dir_main" / "main");
    Touch(base / "dir_main" / "main.npdm");
    assert(!Loader::LooseFS::HasLooseTitle(base / "dir_main"));

    // Never mistake an arbitrary payload named main for an executable.
    fs::create_directories(base / "unrelated" / "romfs");
    Touch(base / "unrelated" / "main");
    assert(!Loader::LooseFS::HasLooseTitle(base / "unrelated"));
    assert(!Loader::LooseFS::IsExeFSMain(base / "unrelated" / "main"));
    assert(!Loader::LooseFS::CanSkipRomFS(base / "unrelated" / "romfs"));
}
