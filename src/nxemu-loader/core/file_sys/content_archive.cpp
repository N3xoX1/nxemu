// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

#include "core/file_sys/content_archive.h"
#include "core/file_sys/partition_filesystem.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/loader/loader.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/polyfill_ranges.h"

#include "core/file_sys/fssystem/fssystem_compression_configuration.h"
#include "core/file_sys/fssystem/fssystem_nca_file_system_driver.h"

namespace FileSys
{

static u8 MasterKeyIdForKeyGeneration(u8 key_generation)
{
    return std::max<u8>(key_generation, 1) - 1;
}

NCA::NCA(VirtualFile file_, const NCA * base_nca) :
    file(std::move(file_))
{
    if (file == nullptr)
    {
        status = LoaderResultStatus::ErrorNullFile;
        return;
    }

    reader = std::make_shared<NcaReader>();
    Result rc = reader->Initialize(file, GetNcaCompressionConfiguration());
    if (R_FAILED(rc))
    {
        if (rc != ResultInvalidNcaSignature)
        {
            LOG_ERROR(Loader, "File reader errored out during header read: {:#x}",
                      rc.GetInnerValue());
        }
        status = LoaderResultStatus::ErrorBadNCAHeader;
        return;
    }

    const s32 fs_count = reader->GetFsCount();
    NcaFileSystemDriver fs(base_nca ? base_nca->reader : nullptr, reader);
    std::vector<VirtualFile> filesystems(fs_count);
    for (s32 i = 0; i < fs_count; i++)
    {
        NcaFsHeaderReader header_reader;
        const Result rc = fs.OpenStorage(&filesystems[i], &header_reader, i);
        if (R_FAILED(rc))
        {
            LOG_ERROR(Loader, "File reader errored out during read of section {}: {:#x}", i, rc.GetInnerValue());
            status = LoaderResultStatus::ErrorBadNCAHeader;
            return;
        }

        if (header_reader.GetFsType() == NcaFsHeader::FsType::RomFs)
        {
            files.push_back(filesystems[i]);
            romfs = files.back();
        }

        if (header_reader.GetFsType() == NcaFsHeader::FsType::PartitionFs)
        {
            auto npfs = std::make_shared<PartitionFilesystem>(filesystems[i]);
            if (npfs->GetStatus() == LoaderResultStatus::Success)
            {
                dirs.push_back(npfs);
                if (IsDirectoryExeFS(npfs))
                {
                    exefs = dirs.back();
                }
                else if (IsDirectoryLogoPartition(npfs))
                {
                    logo = dirs.back();
                }
                else
                {
                    continue;
                }
            }
        }

        if (header_reader.GetEncryptionType() == NcaFsHeader::EncryptionType::AesCtrEx)
        {
            is_update = true;
        }
    }

    if (is_update && base_nca == nullptr)
    {
        status = LoaderResultStatus::ErrorMissingBKTRBaseRomFS;
    }
    else
    {
        status = LoaderResultStatus::Success;
    }
}

NCA::~NCA() = default;

LoaderResultStatus NCA::GetStatus() const
{
    return status;
}

std::vector<VirtualFile> NCA::GetFiles() const
{
    if (status != LoaderResultStatus::Success)
    {
        return {};
    }
    return files;
}

std::vector<VirtualDir> NCA::GetSubdirectories() const
{
    if (status != LoaderResultStatus::Success)
    {
        return {};
    }
    return dirs;
}

std::string NCA::GetName() const
{
    return file->GetName();
}

VirtualDir NCA::GetParentDirectory() const
{
    return file->GetContainingDirectory();
}

NCAContentType NCA::GetType() const
{
    return static_cast<NCAContentType>(reader->GetContentType());
}

uint64_t NCA::GetTitleId() const
{
    if (is_update)
    {
        return reader->GetProgramId() | 0x800;
    }
    return reader->GetProgramId();
}

RightsId NCA::GetRightsId() const
{
    RightsId result;
    reader->GetRightsId(result.data(), result.size());
    return result;
}

u32 NCA::GetSDKVersion() const
{
    return reader->GetSdkAddonVersion();
}

u8 NCA::GetKeyGeneration() const
{
    return reader->GetKeyGeneration();
}

bool NCA::IsUpdate() const
{
    return is_update;
}

VirtualFile NCA::RomFS() const
{
    return romfs;
}

VirtualDir NCA::GetExeFS() const
{
    return exefs;
}

VirtualFile NCA::BaseFile() const
{
    return file;
}

IVirtualFile * NCA::GetBaseFile()
{
    return std::make_unique<VirtualFileImpl>(file).release();
}

VirtualDir NCA::GetLogoPartition() const
{
    return logo;
}

IVirtualFile * NCA::GetRomFS()
{
    return std::make_unique<VirtualFileImpl>(romfs).release();
}

void NCA::Release()
{
    delete this;
}

} // namespace FileSys
