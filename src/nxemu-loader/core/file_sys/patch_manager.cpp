// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

#include "yuzu_common/hex_util.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/settings.h"
#include <nxemu-module-spec/base.h>
#include "nxemu-os/os_settings_identifiers.h"
#ifndef _WIN32
#include "yuzu_common/string_util.h"
#endif

#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/filesystem.h"
#include "core/file_sys/ips_layer.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_cached.h"
#include "core/file_sys/vfs/vfs_layered.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/service/ns/language.h"
#include "core/hle/service/set/settings_server.h"
#include "core/loader/loader.h"
#include "core/loader/nso.h"
#include "loader_settings.h"

extern IModuleSettings * g_settings;

namespace FileSys
{
namespace
{

constexpr u32 SINGLE_BYTE_MODULUS = 0x100;

constexpr std::array<const char *, 14> EXEFS_FILE_NAMES{
    "main",
    "main.npdm",
    "rtld",
    "sdk",
    "subsdk0",
    "subsdk1",
    "subsdk2",
    "subsdk3",
    "subsdk4",
    "subsdk5",
    "subsdk6",
    "subsdk7",
    "subsdk8",
    "subsdk9",
};

enum class TitleVersionFormat : u8
{
    ThreeElements, ///< vX.Y.Z
    FourElements,  ///< vX.Y.Z.W
};

std::string FormatTitleVersion(u32 version, TitleVersionFormat format = TitleVersionFormat::ThreeElements)
{
    std::array<u8, sizeof(u32)> bytes{};
    bytes[0] = static_cast<u8>(version % SINGLE_BYTE_MODULUS);
    for (std::size_t i = 1; i < bytes.size(); ++i)
    {
        version /= SINGLE_BYTE_MODULUS;
        bytes[i] = static_cast<u8>(version % SINGLE_BYTE_MODULUS);
    }

    if (format == TitleVersionFormat::FourElements)
    {
        return fmt::format("v{}.{}.{}.{}", bytes[3], bytes[2], bytes[1], bytes[0]);
    }
    return fmt::format("v{}.{}.{}", bytes[3], bytes[2], bytes[1]);
}

void AppendCommaIfNotEmpty(std::string & to, std::string_view with)
{
    if (!to.empty())
    {
        to += ", ";
    }
    to += with;
}

bool IsDirValidAndNonEmpty(const VirtualDir & dir)
{
    return dir != nullptr && (!dir->GetFiles().empty() || !dir->GetSubdirectories().empty());
}

bool IsVersionedUpdateDisabled(const std::vector<std::string> & disabled, uint32_t version)
{
    const std::string disabled_key = fmt::format("Update@{}", version);
    return std::find(disabled.cbegin(), disabled.cend(), disabled_key) != disabled.cend() ||
           std::find(disabled.cbegin(), disabled.cend(), "Update") != disabled.cend();
}

std::string GetUpdateDisplayVersion(const ManualContentProvider & manual_provider, uint64_t update_tid, uint32_t version)
{
    VirtualFile control = manual_provider.GetEntryForVersion(update_tid, LoaderContentRecordType::Control, version);
    if (control != nullptr)
    {
        const NCA nca(control);
        if (nca.GetStatus() == LoaderResultStatus::Success)
        {
            const VirtualFile romfs = nca.RomFS();
            if (romfs != nullptr)
            {
                const VirtualDir extracted = ExtractRomFS(romfs);
                if (extracted != nullptr)
                {
                    VirtualFile nacp_file = extracted->GetFile("control.nacp");
                    if (nacp_file == nullptr)
                    {
                        nacp_file = extracted->GetFile("Control.nacp");
                    }
                    if (nacp_file != nullptr)
                    {
                        const NACP nacp(nacp_file);
                        const char * version_string = nacp.GetVersionString();
                        if (version_string != nullptr && version_string[0] != '\0')
                        {
                            return version_string;
                        }
                    }
                }
            }
        }
    }
    return FormatTitleVersion(version);
}

VirtualDir FindSubdirectoryCaseless(const VirtualDir dir, std::string_view name)
{
#ifdef _WIN32
    return dir->GetSubdirectory(name);
#else
    const auto subdirs = dir->GetSubdirectories();
    for (const auto & subdir : subdirs)
    {
        std::string dir_name = Common::ToLower(subdir->GetName());
        if (dir_name == name)
        {
            return subdir;
        }
    }

    return nullptr;
#endif
}

} // Anonymous namespace

PatchManager::PatchManager(uint64_t title_id_, const FileSystemController & fs_controller_, const ContentProvider & content_provider_) :
    title_id{title_id_},
    fs_controller{fs_controller_},
    content_provider{content_provider_}
{
}

PatchManager::~PatchManager() = default;

uint64_t PatchManager::GetTitleID() const
{
    return title_id;
}

VirtualDir PatchManager::PatchExeFS(VirtualDir exefs) const
{
    LOG_INFO(Loader, "Patching ExeFS for title_id={:016X}", title_id);

    if (exefs == nullptr)
    {
        return exefs;    
    }

    const std::vector<std::string> & disabled = loaderSettings.disabled_addons[title_id];
    const uint64_t update_tid = GetUpdateTitleID(title_id);
    const ManualContentProvider * manual_provider = content_provider.GetManualContentProvider();

    bool update_disabled = true;
    std::optional<u32> enabled_version;
    bool checked_manual = false;

    if (manual_provider != nullptr)
    {
        const std::vector<ManualUpdateEntry> update_versions = manual_provider->ListUpdateVersions(update_tid);
        if (!update_versions.empty())
        {
            checked_manual = true;
            for (const ManualUpdateEntry & update_entry : update_versions)
            {
                if (!IsVersionedUpdateDisabled(disabled, update_entry.version))
                {
                    update_disabled = false;
                    enabled_version = update_entry.version;
                    break;
                }
            }
        }
    }

    if (!checked_manual)
    {
        update_disabled = std::find(disabled.cbegin(), disabled.cend(), "Update") != disabled.cend();
    }

    std::unique_ptr<NCA> update;
    if (enabled_version.has_value() && manual_provider != nullptr)
    {
        VirtualFile file = manual_provider->GetEntryForVersion(update_tid, LoaderContentRecordType::Program,
                                                              *enabled_version);
        if (file != nullptr)
        {
            update = std::make_unique<NCA>(file);
        }
    }

    if (update == nullptr && !update_disabled)
    {
        update = content_provider.GetEntryNCA(update_tid, LoaderContentRecordType::Program);
    }

    if (!update_disabled && update != nullptr && update->GetExeFS() != nullptr)
    {
        const u32 version = enabled_version.value_or(content_provider.GetEntryVersion(update_tid).value_or(0));
        LOG_INFO(Loader, "    ExeFS: Update ({}) applied successfully", FormatTitleVersion(version));
        exefs = update->GetExeFS();
    }

    // LayeredExeFS
    const VirtualDir load_dir = fs_controller.GetModificationLoadRoot(title_id);
    const VirtualDir sdmc_load_dir = fs_controller.GetSDMCModificationLoadRoot(title_id);

    std::vector<VirtualDir> patch_dirs = {sdmc_load_dir};
    if (load_dir != nullptr)
    {
        const std::vector<VirtualDir> load_patch_dirs = load_dir->GetSubdirectories();
        patch_dirs.insert(patch_dirs.end(), load_patch_dirs.begin(), load_patch_dirs.end());
    }

    std::sort(patch_dirs.begin(), patch_dirs.end(),
              [](const VirtualDir & l, const VirtualDir & r) { return l->GetName() < r->GetName(); });

    std::vector<VirtualDir> layers;
    layers.reserve(patch_dirs.size() + 1);
    for (const VirtualDir & subdir : patch_dirs)
    {
        if (std::find(disabled.begin(), disabled.end(), subdir->GetName()) != disabled.end())
        {
            continue;
        }

        VirtualDir exefs_dir = FindSubdirectoryCaseless(subdir, "exefs");
        if (exefs_dir != nullptr)
        {
            layers.push_back(std::move(exefs_dir));
        }
    }
    layers.push_back(exefs);

    VirtualDir layered = LayeredVfsDirectory::MakeLayeredDirectory(std::move(layers));
    if (layered != nullptr)
    {
        LOG_INFO(Loader, "    ExeFS: LayeredExeFS patches applied successfully");
        exefs = std::move(layered);
    }

    if (Settings::values.dump_exefs)
    {
        LOG_INFO(Loader, "Dumping ExeFS for title_id={:016X}", title_id);
        const VirtualDir dump_dir = fs_controller.GetModificationDumpRoot(title_id);
        if (dump_dir != nullptr)
        {
            const VirtualDir exefs_dir = GetOrCreateDirectoryRelative(dump_dir, "/exefs");
            VfsRawCopyD(exefs, exefs_dir);
        }
    }

    return exefs;
}

std::vector<VirtualFile> PatchManager::CollectPatches(const std::vector<VirtualDir> & patch_dirs, const std::string & build_id) const
{
    const auto & disabled = loaderSettings.disabled_addons[title_id];
    const auto nso_build_id = fmt::format("{:0<64}", build_id);

    std::vector<VirtualFile> out;
    out.reserve(patch_dirs.size());
    for (const auto & subdir : patch_dirs)
    {
        if (std::find(disabled.cbegin(), disabled.cend(), subdir->GetName()) != disabled.cend())
        {
            continue;
        }

        auto exefs_dir = FindSubdirectoryCaseless(subdir, "exefs");
        if (exefs_dir != nullptr)
        {
            for (const auto & file : exefs_dir->GetFiles())
            {
                if (file->GetExtension() == "ips")
                {
                    auto name = file->GetName();

                    const auto this_build_id = fmt::format("{:0<64}", name.substr(0, name.find('.')));
                    if (nso_build_id == this_build_id)
                    {
                        out.push_back(file);
                    }
                }
                else if (file->GetExtension() == "pchtxt")
                {
                    IPSwitchCompiler compiler{file};
                    if (!compiler.IsValid())
                    {
                        continue;
                    }
                    const auto this_build_id = Common::HexToString(compiler.GetBuildID());
                    if (nso_build_id == this_build_id)
                    {
                        out.push_back(file);
                    }
                }
            }
        }
    }

    return out;
}

std::vector<u8> PatchManager::PatchNSO(const std::vector<u8> & nso, const std::string & name) const
{
    if (nso.size() < sizeof(Loader::NSOHeader))
    {
        return nso;
    }

    Loader::NSOHeader header;
    std::memcpy(&header, nso.data(), sizeof(header));

    if (header.magic != Common::MakeMagic('N', 'S', 'O', '0'))
    {
        return nso;
    }

    const std::string build_id_raw = Common::HexToString(header.build_id);
    const std::string build_id = build_id_raw.substr(0, build_id_raw.find_last_not_of('0') + 1);

    if (Settings::values.dump_nso)
    {
        LOG_INFO(Loader, "Dumping NSO for name={}, build_id={}, title_id={:016X}", name, build_id, title_id);
        const VirtualDir dump_dir = fs_controller.GetModificationDumpRoot(title_id);
        if (dump_dir != nullptr)
        {
            const VirtualDir nso_dir = GetOrCreateDirectoryRelative(dump_dir, "/nso");
            const VirtualFile file = nso_dir->CreateFile(fmt::format("{}-{}.nso", name, build_id));

            file->Resize(nso.size());
            file->WriteBytes(nso);
        }
    }

    LOG_INFO(Loader, "Patching NSO for name={}, build_id={}", name, build_id);

    const VirtualDir load_dir = fs_controller.GetModificationLoadRoot(title_id);
    if (load_dir == nullptr)
    {
        LOG_ERROR(Loader, "Cannot load mods for invalid title_id={:016X}", title_id);
        return nso;
    }

    std::vector<VirtualDir> patch_dirs = load_dir->GetSubdirectories();
    std::sort(patch_dirs.begin(), patch_dirs.end(), [](const VirtualDir & l, const VirtualDir & r) { return l->GetName() < r->GetName(); });
    const std::vector<VirtualFile> patches = CollectPatches(patch_dirs, build_id);

    std::vector<u8> out = nso;
    for (const VirtualFile & patch_file : patches)
    {
        if (patch_file->GetExtension() == "ips")
        {
            LOG_INFO(Loader, "    - Applying IPS patch from mod \"{}\"", patch_file->GetContainingDirectory()->GetParentDirectory()->GetName());
            const VirtualFile patched = PatchIPS(std::make_shared<VectorVfsFile>(out), patch_file);
            if (patched != nullptr)
            {
                out = patched->ReadAllBytes();            
            }
        }
        else if (patch_file->GetExtension() == "pchtxt")
        {
            LOG_INFO(Loader, "    - Applying IPSwitch patch from mod \"{}\"", patch_file->GetContainingDirectory()->GetParentDirectory()->GetName());
            const IPSwitchCompiler compiler{patch_file};
            const VirtualFile patched = compiler.Apply(std::make_shared<VectorVfsFile>(out));
            if (patched != nullptr)
            {            
                out = patched->ReadAllBytes();
            }
        }
    }

    if (out.size() < sizeof(Loader::NSOHeader))
    {
        return nso;
    }

    std::memcpy(out.data(), &header, sizeof(header));
    return out;
}

bool PatchManager::HasNSOPatch(const BuildID & build_id_, std::string_view name) const
{
    const auto build_id_raw = Common::HexToString(build_id_);
    const auto build_id = build_id_raw.substr(0, build_id_raw.find_last_not_of('0') + 1);

    LOG_INFO(Loader, "Querying NSO patch existence for build_id={}, name={}", build_id, name);

    const auto load_dir = fs_controller.GetModificationLoadRoot(title_id);
    if (load_dir == nullptr)
    {
        LOG_ERROR(Loader, "Cannot load mods for invalid title_id={:016X}", title_id);
        return false;
    }

    auto patch_dirs = load_dir->GetSubdirectories();
    std::sort(patch_dirs.begin(), patch_dirs.end(),
              [](const VirtualDir & l, const VirtualDir & r) { return l->GetName() < r->GetName(); });

    return !CollectPatches(patch_dirs, build_id).empty();
}

static void ApplyLayeredFS(VirtualFile & romfs, uint64_t title_id, LoaderContentRecordType type, const FileSystemController & fs_controller)
{
    const auto load_dir = fs_controller.GetModificationLoadRoot(title_id);
    const auto sdmc_load_dir = fs_controller.GetSDMCModificationLoadRoot(title_id);
    if ((type != LoaderContentRecordType::Program && type != LoaderContentRecordType::Data &&
         type != LoaderContentRecordType::HtmlDocument) ||
        (load_dir == nullptr && sdmc_load_dir == nullptr))
    {
        return;
    }

    const auto & disabled = loaderSettings.disabled_addons[title_id];
    std::vector<VirtualDir> patch_dirs = load_dir->GetSubdirectories();
    if (std::find(disabled.cbegin(), disabled.cend(), "SDMC") == disabled.cend())
    {
        patch_dirs.push_back(sdmc_load_dir);
    }
    std::sort(patch_dirs.begin(), patch_dirs.end(),
              [](const VirtualDir & l, const VirtualDir & r) { return l->GetName() < r->GetName(); });

    std::vector<VirtualDir> layers;
    std::vector<VirtualDir> layers_ext;
    layers.reserve(patch_dirs.size() + 1);
    layers_ext.reserve(patch_dirs.size() + 1);
    for (const auto & subdir : patch_dirs)
    {
        if (std::find(disabled.cbegin(), disabled.cend(), subdir->GetName()) != disabled.cend())
        {
            continue;
        }

        auto romfs_dir = FindSubdirectoryCaseless(subdir, "romfs");
        if (romfs_dir != nullptr)
        {
            layers.emplace_back(std::make_shared<CachedVfsDirectory>(std::move(romfs_dir)));
        }

        auto ext_dir = FindSubdirectoryCaseless(subdir, "romfs_ext");
        if (ext_dir != nullptr)
        {
            layers_ext.emplace_back(std::make_shared<CachedVfsDirectory>(std::move(ext_dir)));
        }

        if (type == LoaderContentRecordType::HtmlDocument)
        {
            auto manual_dir = FindSubdirectoryCaseless(subdir, "manual_html");
            if (manual_dir != nullptr)
            {
                layers.emplace_back(std::make_shared<CachedVfsDirectory>(std::move(manual_dir)));
            }
        }
    }

    // When there are no layers to apply, return early as there is no need to rebuild the RomFS
    if (layers.empty() && layers_ext.empty())
    {
        return;
    }

    auto extracted = ExtractRomFS(romfs);
    if (extracted == nullptr)
    {
        return;
    }

    layers.emplace_back(std::move(extracted));

    auto layered = LayeredVfsDirectory::MakeLayeredDirectory(std::move(layers));
    if (layered == nullptr)
    {
        return;
    }

    auto layered_ext = LayeredVfsDirectory::MakeLayeredDirectory(std::move(layers_ext));

    auto packed = CreateRomFS(std::move(layered), std::move(layered_ext));
    if (packed == nullptr)
    {
        return;
    }

    LOG_INFO(Loader, "    RomFS: LayeredFS patches applied successfully");
    romfs = std::move(packed);
}

VirtualFile PatchManager::PatchRomFS(const NCA * base_nca, VirtualFile base_romfs, LoaderContentRecordType type, VirtualFile packed_update_raw, bool apply_layeredfs) const
{
    const auto log_string = fmt::format("Patching RomFS for title_id={:016X}, type={:02X}", title_id, static_cast<u8>(type));
    if (type == LoaderContentRecordType::Program || type == LoaderContentRecordType::Data)
    {
        LOG_INFO(Loader, "{}", log_string);
    }
    else
    {
        LOG_DEBUG(Loader, "{}", log_string);
    }

    VirtualFile romfs = base_romfs;

    // Game Updates
    const uint64_t update_tid = GetUpdateTitleID(title_id);
    const auto & disabled = loaderSettings.disabled_addons[title_id];
    const ManualContentProvider * manual_provider = content_provider.GetManualContentProvider();

    bool update_disabled = true;
    std::optional<u32> enabled_version;
    bool checked_manual = false;
    VirtualFile update_raw;

    if (manual_provider != nullptr)
    {
        const auto update_versions = manual_provider->ListUpdateVersions(update_tid);
        if (!update_versions.empty())
        {
            checked_manual = true;
            for (const auto & update_entry : update_versions)
            {
                if (!IsVersionedUpdateDisabled(disabled, update_entry.version))
                {
                    update_disabled = false;
                    enabled_version = update_entry.version;
                    update_raw = manual_provider->GetEntryForVersion(update_tid, type, update_entry.version);
                    break;
                }
            }
        }
    }

    if (!checked_manual)
    {
        update_disabled = std::find(disabled.cbegin(), disabled.cend(), "Update") != disabled.cend();
        update_raw = content_provider.GetEntryRaw(update_tid, type);
    }
    else if (update_raw == nullptr && !update_disabled)
    {
        update_raw = content_provider.GetEntryRaw(update_tid, type);
    }

    if (!update_disabled && update_raw != nullptr && base_nca != nullptr)
    {
        const auto new_nca = std::make_shared<NCA>(update_raw, base_nca);
        if (new_nca->GetStatus() == LoaderResultStatus::Success &&
            new_nca->RomFS() != nullptr)
        {
            const u32 version = enabled_version.value_or(content_provider.GetEntryVersion(update_tid).value_or(0));
            LOG_INFO(Loader, "    RomFS: Update ({}) applied successfully", FormatTitleVersion(version));
            romfs = new_nca->RomFS();
        }
    }
    else if (!update_disabled && packed_update_raw != nullptr && base_nca != nullptr)
    {
        const auto new_nca = std::make_shared<NCA>(packed_update_raw, base_nca);
        if (new_nca->GetStatus() == LoaderResultStatus::Success &&
            new_nca->RomFS() != nullptr)
        {
            LOG_INFO(Loader, "    RomFS: Update (PACKED) applied successfully");
            romfs = new_nca->RomFS();
        }
    }

    // LayeredFS
    if (apply_layeredfs)
    {
        ApplyLayeredFS(romfs, title_id, type, fs_controller);
    }

    return romfs;
}

std::vector<Patch> PatchManager::GetPatches(VirtualFile update_raw) const
{
    if (title_id == 0)
    {
        return {};
    }

    std::vector<Patch> out;

    // Game Updates
    const auto update_tid = GetUpdateTitleID(title_id);
    const auto & disabled = loaderSettings.disabled_addons[title_id];
    const ManualContentProvider * manual_provider = content_provider.GetManualContentProvider();

    std::vector<Patch> update_patches;
    if (manual_provider != nullptr)
    {
        for (const auto & update_entry : manual_provider->ListUpdateVersions(update_tid))
        {
            update_patches.push_back({.enabled = !IsVersionedUpdateDisabled(disabled, update_entry.version),
                                      .name = "Update",
                                      .version = GetUpdateDisplayVersion(*manual_provider, update_tid, update_entry.version),
                                      .type = PatchType::Update,
                                      .program_id = title_id,
                                      .title_id = update_tid,
                                      .numeric_version = update_entry.version});
        }
    }

    if (update_patches.size() > 1)
    {
        bool found_enabled = false;
        for (auto & patch : update_patches)
        {
            if (!patch.enabled)
            {
                continue;
            }
            if (found_enabled)
            {
                patch.enabled = false;
            }
            else
            {
                found_enabled = true;
            }
        }
    }

    if (!update_patches.empty())
    {
        for (auto & patch : update_patches)
        {
            out.push_back(std::move(patch));
        }
    }
    else
    {
        PatchManager update{update_tid, fs_controller, content_provider};
        const auto metadata = update.GetControlMetadata();
        const auto & nacp = metadata.first;

        const bool update_disabled = std::find(disabled.cbegin(), disabled.cend(), "Update") != disabled.cend();
        Patch update_patch = {.enabled = !update_disabled,
                              .name = "Update",
                              .version = "",
                              .type = PatchType::Update,
                              .program_id = title_id,
                              .title_id = title_id};

        if (nacp != nullptr)
        {
            update_patch.version = nacp->GetVersionString();
            out.push_back(update_patch);
        }
        else if (content_provider.HasEntry(update_tid, LoaderContentRecordType::Program))
        {
            const auto meta_ver = content_provider.GetEntryVersion(update_tid);
            if (meta_ver.value_or(0) == 0)
            {
                out.push_back(update_patch);
            }
            else
            {
                update_patch.version = FormatTitleVersion(*meta_ver);
                update_patch.numeric_version = *meta_ver;
                out.push_back(update_patch);
            }
        }
        else if (update_raw != nullptr)
        {
            update_patch.version = "PACKED";
            out.push_back(update_patch);
        }
    }

    // General Mods (LayeredFS and IPS)
    const auto mod_dir = fs_controller.GetModificationLoadRoot(title_id);
    if (mod_dir != nullptr)
    {
        for (const auto & mod : mod_dir->GetSubdirectories())
        {
            std::string types;

            const auto exefs_dir = FindSubdirectoryCaseless(mod, "exefs");
            if (IsDirValidAndNonEmpty(exefs_dir))
            {
                bool ips = false;
                bool ipswitch = false;
                bool layeredfs = false;

                for (const auto & file : exefs_dir->GetFiles())
                {
                    if (file->GetExtension() == "ips")
                    {
                        ips = true;
                    }
                    else if (file->GetExtension() == "pchtxt")
                    {
                        ipswitch = true;
                    }
                    else if (std::find(EXEFS_FILE_NAMES.begin(), EXEFS_FILE_NAMES.end(),
                                       file->GetName()) != EXEFS_FILE_NAMES.end())
                    {
                        layeredfs = true;
                    }
                }

                if (ips)
                {
                    AppendCommaIfNotEmpty(types, "IPS");
                }
                if (ipswitch)
                {
                    AppendCommaIfNotEmpty(types, "IPSwitch");
                }
                if (layeredfs)
                {
                    AppendCommaIfNotEmpty(types, "LayeredExeFS");
                }
            }
            if (IsDirValidAndNonEmpty(FindSubdirectoryCaseless(mod, "romfs")))
            {
                AppendCommaIfNotEmpty(types, "LayeredFS");
            }
            if (IsDirValidAndNonEmpty(FindSubdirectoryCaseless(mod, "cheats")))
            {
                AppendCommaIfNotEmpty(types, "Cheats");
            }

            if (types.empty())
            {
                continue;
            }

            const bool mod_disabled =
                std::find(disabled.begin(), disabled.end(), mod->GetName()) != disabled.end();
            out.push_back({.enabled = !mod_disabled,
                           .name = mod->GetName(),
                           .version = std::move(types),
                           .type = PatchType::Mod,
                           .program_id = title_id,
                           .title_id = title_id});
        }
    }

    // SDMC mod directory (RomFS LayeredFS)
    const auto sdmc_mod_dir = fs_controller.GetSDMCModificationLoadRoot(title_id);
    if (sdmc_mod_dir != nullptr)
    {
        std::string types;
        if (IsDirValidAndNonEmpty(FindSubdirectoryCaseless(sdmc_mod_dir, "exefs")))
        {
            AppendCommaIfNotEmpty(types, "LayeredExeFS");
        }
        if (IsDirValidAndNonEmpty(FindSubdirectoryCaseless(sdmc_mod_dir, "romfs")))
        {
            AppendCommaIfNotEmpty(types, "LayeredFS");
        }

        if (!types.empty())
        {
            const bool mod_disabled =
                std::find(disabled.begin(), disabled.end(), "SDMC") != disabled.end();
            out.push_back({.enabled = !mod_disabled,
                           .name = "SDMC",
                           .version = std::move(types),
                           .type = PatchType::Mod,
                           .program_id = title_id,
                           .title_id = title_id});
        }
    }

    // DLC
    const auto dlc_entries = content_provider.ListEntriesFilter(LoaderTitleType::AOC, LoaderContentRecordType::Data);
    std::vector<ContentProviderEntry> dlc_match;
    dlc_match.reserve(dlc_entries.size());
    std::copy_if(dlc_entries.begin(), dlc_entries.end(), std::back_inserter(dlc_match),
                 [this](const ContentProviderEntry & entry) {
                     const auto nca = content_provider.GetEntryNCA(entry);
                     return GetBaseTitleID(entry.titleID) == title_id && nca != nullptr &&
                            nca->GetStatus() == LoaderResultStatus::Success;
                 });
    if (!dlc_match.empty())
    {
        std::sort(dlc_match.begin(), dlc_match.end(),
                  [](const ContentProviderEntry & lhs, const ContentProviderEntry & rhs) {
                      return (lhs.titleID < rhs.titleID) ||
                             (lhs.titleID == rhs.titleID && lhs.type < rhs.type);
                  });

        std::string list;
        for (size_t i = 0; i < dlc_match.size() - 1; ++i)
        {
            list += fmt::format("{}, ", dlc_match[i].titleID & 0x7FF);
        }
        list += fmt::format("{}", dlc_match.back().titleID & 0x7FF);

        const bool dlc_disabled =
            std::find(disabled.begin(), disabled.end(), "DLC") != disabled.end();
        out.push_back({.enabled = !dlc_disabled,
                       .name = "DLC",
                       .version = std::move(list),
                       .type = PatchType::DLC,
                       .program_id = title_id,
                       .title_id = dlc_match.back().titleID});
    }

    return out;
}

std::optional<u32> PatchManager::GetGameVersion() const
{
    const auto update_tid = GetUpdateTitleID(title_id);
    if (content_provider.HasEntry(update_tid, LoaderContentRecordType::Program))
    {
        return content_provider.GetEntryVersion(update_tid);
    }

    return content_provider.GetEntryVersion(title_id);
}

PatchManager::Metadata PatchManager::GetControlMetadata() const
{
    const auto raw = content_provider.GetEntryRaw(title_id, LoaderContentRecordType::Control);
    if (raw == nullptr)
    {
        return {};
    }
    const auto base_control_nca = std::make_unique<NCA>(raw);
    if (base_control_nca == nullptr)
    {
        return {};
    }
    return ParseControlNCA(*base_control_nca);
}

PatchManager::Metadata PatchManager::ParseControlNCA(const NCA & nca) const
{
    const auto base_romfs = nca.RomFS();
    if (base_romfs == nullptr)
    {
        return {};
    }

    const auto romfs = PatchRomFS(&nca, base_romfs, LoaderContentRecordType::Control);
    if (romfs == nullptr)
    {
        return {};
    }

    const auto extracted = ExtractRomFS(romfs);
    if (extracted == nullptr)
    {
        return {};
    }

    auto nacp_file = extracted->GetFile("control.nacp");
    if (nacp_file == nullptr)
    {
        nacp_file = extracted->GetFile("Control.nacp");
    }

    auto nacp = nacp_file == nullptr ? nullptr : std::make_unique<NACP>(nacp_file);

    // Get language code from settings
    const auto language_code = Service::Set::GetLanguageCodeFromIndex((uint32_t)g_settings->GetInt(NXOsSetting::LanguageIndex));

    // Convert to application language and get priority list
    const auto application_language = Service::NS::ConvertToApplicationLanguage(language_code).value_or(Service::NS::ApplicationLanguage::AmericanEnglish);
    const auto language_priority_list = Service::NS::GetApplicationLanguagePriorityList(application_language);

    // Convert to language names
    auto priority_language_names = FileSys::LANGUAGE_NAMES; // Copy
    if (language_priority_list)
    {
        for (size_t i = 0; i < priority_language_names.size(); ++i)
        {
            // Relies on FileSys::LANGUAGE_NAMES being in the same order as
            // Service::NS::ApplicationLanguage
            const auto language_index = static_cast<u8>(language_priority_list->at(i));

            if (language_index < FileSys::LANGUAGE_NAMES.size())
            {
                priority_language_names[i] = FileSys::LANGUAGE_NAMES[language_index];
            }
            else
            {
                // Not a catastrophe, unlikely to happen
                LOG_WARNING(Loader, "Invalid language index {}", language_index);
            }
        }
    }

    // Get first matching icon
    VirtualFile icon_file;
    for (const auto & language : priority_language_names)
    {
        icon_file = extracted->GetFile(std::string("icon_").append(language).append(".dat"));
        if (icon_file != nullptr)
        {
            break;
        }
    }
        
    return {std::move(nacp), icon_file};
}
} // namespace FileSys
