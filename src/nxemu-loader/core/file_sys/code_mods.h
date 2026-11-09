// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include <common/sha256.h>

#include "core/file_sys/patch_sets.h"
#include "core/file_sys/vfs/vfs.h"

namespace FileSys {

// Hash of the files in the given exefs and cheats folders of the enabled mods, in the order given, or empty without
// any. It follows what the files hold, so an updated mod hashes differently under the same name, and what else decides
// which patch sets apply: their switches, and native_code, as PatchSets leaves out sets the host cannot run.
inline std::string HashCodeMods(const std::vector<VirtualDir> & code_dirs, bool native_code = false)
{
    std::string description;
    bool has_patch_sets = false;
    for (const VirtualDir & dir : code_dirs)
    {
        std::vector<VirtualFile> files = dir->GetFiles();
        std::sort(files.begin(), files.end(), [](const VirtualFile & l, const VirtualFile & r) { return l->GetName() < r->GetName(); });
        for (const VirtualFile & file : files)
        {
            const std::vector<u8> bytes = file->ReadAllBytes();
            description += dir->GetName() + "/" + file->GetName() + ":" + sha256(std::string(bytes.begin(), bytes.end())) + "\n";
            if (file->GetExtension() == "nxpatch")
            {
                has_patch_sets = true;
                description += PatchSets::FileSetSwitches(file);
            }
        }
    }
    if (has_patch_sets && native_code)
    {
        description += "native code\n";
    }
    return description.empty() ? std::string{} : sha256(description);
}

} // namespace FileSys
