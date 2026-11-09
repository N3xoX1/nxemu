// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include "yuzu_common/common_types.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace Core {
class System;
}

class FileSystemController;

namespace FileSys {

class ContentProvider;
class NCA;
class NACP;

enum class PatchType { Update, DLC, Mod };

struct Patch {
    bool enabled;
    std::string name;
    std::string version;
    PatchType type;
    uint64_t program_id;
    uint64_t title_id;
    uint32_t numeric_version = 0;
    bool replaces_romfs = false; // a mod that layers files over the game's RomFS
};

// A centralized class to manage patches to games.
class PatchManager {
public:
    using BuildID = std::array<u8, 0x20>;
    using Metadata = std::pair<std::unique_ptr<NACP>, VirtualFile>;

    explicit PatchManager(uint64_t title_id_, const FileSystemController& fs_controller_, const ContentProvider& content_provider_);
    ~PatchManager();

    [[nodiscard]] uint64_t GetTitleID() const;

    // Currently tracked ExeFS patches:
    // - Game Updates
    [[nodiscard]] VirtualDir PatchExeFS(VirtualDir exefs) const;

    // Currently tracked NSO patches:
    // - IPS
    // - IPSwitch
    [[nodiscard]] std::vector<u8> PatchNSO(const std::vector<u8>& nso,
                                           const std::string& name) const;

    // Checks to see if PatchNSO() will have any effect given the NSO's build ID.
    // Used to prevent expensive copies in NSO loader.
    [[nodiscard]] bool HasNSOPatch(const BuildID& build_id, std::string_view name) const;

    // The patch set files (exefs/*.nxpatch, core/file_sys/patch_sets.h) of the enabled mods, in mod order.
    // Each names the build id it is for; PatchSets keeps those of the module it patches.
    [[nodiscard]] std::vector<VirtualFile> CollectPatchSetFiles() const;

    // Hash of the ExeFS patches and cheats of the enabled mods, empty without any (core/file_sys/code_mods.h).
    [[nodiscard]] std::string HashEnabledCodeMods() const;

    // Currently tracked RomFS patches:
    // - Game Updates
    // - LayeredFS
    [[nodiscard]] VirtualFile PatchRomFS(const NCA* base_nca, VirtualFile base_romfs,
                                         LoaderContentRecordType type = LoaderContentRecordType::Program,
                                         VirtualFile packed_update_raw = nullptr,
                                         bool apply_layeredfs = true) const;

    // Returns a vector of patches
    [[nodiscard]] std::vector<Patch> GetPatches(VirtualFile update_raw = nullptr) const;

    // If the game update exists, returns the u32 version field in its Meta-type NCA. If that fails,
    // it will fallback to the Meta-type NCA of the base game. If that fails, the result will be
    // std::nullopt
    [[nodiscard]] std::optional<u32> GetGameVersion() const;

    // Given title_id of the program, attempts to get the control data of the update and parse
    // it, falling back to the base control data.
    [[nodiscard]] Metadata GetControlMetadata() const;

    // Version of GetControlMetadata that takes an arbitrary NCA
    [[nodiscard]] Metadata ParseControlNCA(const NCA& nca) const;

private:
    [[nodiscard]] std::vector<VirtualFile> CollectPatches(const std::vector<VirtualDir>& patch_dirs,
                                                          const std::string& build_id) const;

    uint64_t title_id;
    const FileSystemController& fs_controller;
    const ContentProvider& content_provider;
};

[[nodiscard]] PatchManager::Metadata ParseControlRomFS(VirtualFile romfs);

// The names of the title's add-ons that are off: those the user turned off, and the bundled mods that are off by
// default (mod.json "enabled_by_default": false) the user has not turned on
[[nodiscard]] std::vector<std::string> GetDisabledAddOns(uint64_t title_id, const FileSystemController& fs_controller);
// Whether one add-on is off, as GetDisabledAddOns gives it
[[nodiscard]] bool IsAddOnDisabled(uint64_t title_id, const FileSystemController& fs_controller, const std::string& name);
// Stores the add-ons that are off, as GetDisabledAddOns gives them
void SetDisabledAddOns(uint64_t title_id, const FileSystemController& fs_controller, std::vector<std::string> names);

} // namespace FileSys
