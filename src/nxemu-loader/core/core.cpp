// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/fs/fs.h"
#include "yuzu_common/string_util.h"
#include "core/core.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_concat.h"
#include "core/file_sys/vfs/vfs.h"
#include <algorithm>
#include <cctype>
#include <fmt/format.h>

namespace Core {

FileSys::VirtualFile GetGameFileFromPath(const FileSys::VirtualFilesystem& vfs,
                                         const std::string& path) {
    // To account for split 00+01+etc files.
    std::string dir_name;
    std::string filename;
    Common::SplitPath(path, &dir_name, &filename, nullptr);

    if (filename == "00") {
        const auto dir = vfs->OpenDirectory(dir_name, VirtualFileOpenMode::Read);
        std::vector<FileSys::VirtualFile> concat;

        for (u32 i = 0; i < 0x10; ++i) {
            const auto file_name = fmt::format("{:02X}", i);
            auto next = dir->GetFile(file_name);

            if (next != nullptr) {
                concat.push_back(std::move(next));
            } else {
                next = dir->GetFile(file_name);

                if (next == nullptr) {
                    break;
                }

                concat.push_back(std::move(next));
            }
        }

        return FileSys::ConcatenatedVfsFile::MakeConcatenatedFile(dir->GetName(),
                                                                  std::move(concat));
    }

    if (Common::FS::IsDir(path)) {
        // Both an ExeFS directory and an extracted title root are accepted.
        // Never touch its (potentially enormous) romfs/ during path resolution.
        const auto directory = vfs->OpenDirectory(path, VirtualFileOpenMode::Read);
        if (!directory) {
            return nullptr;
        }
        if (directory->GetFile("main.npdm") != nullptr) {
            if (auto main = directory->GetFile("main")) {
                return main;
            }
        }
        auto exefs = directory->GetSubdirectory("exefs");
        if (!exefs) {
            for (const auto& child : directory->GetSubdirectories()) {
                const auto name = child->GetName();
                if (name.size() == 5 &&
                    std::equal(name.begin(), name.end(), "exefs", [](unsigned char a, unsigned char b) {
                        return std::tolower(a) == std::tolower(b);
                    })) {
                    exefs = child;
                    break;
                }
            }
        }
        if (exefs && exefs->GetFile("main.npdm") != nullptr) {
            return exefs->GetFile("main");
        }
        return nullptr;
    }

    return vfs->OpenFile(path, VirtualFileOpenMode::Read);
}

} // namespace Core
