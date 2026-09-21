// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/file_sys/bis_factory.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/yuzu_assert.h"
#include <fmt/format.h>

namespace FileSys
{

constexpr uint64_t NAND_USER_SIZE = 0x680000000;  // 26624 MiB
constexpr uint64_t NAND_SYSTEM_SIZE = 0xA0000000; // 2560 MiB
constexpr uint64_t NAND_TOTAL_SIZE = 0x747C00000; // 29820 MiB

BISFactory::BISFactory(VirtualDir nand_root_, VirtualDir load_root_, VirtualDir dump_root_) :
    nand_root(std::move(nand_root_)),
    load_root(std::move(load_root_)),
    dump_root(std::move(dump_root_)),
    sysnand_cache(std::make_unique<RegisteredCache>(GetOrCreateDirectoryRelative(nand_root, "/system/Contents/registered"))),
    usrnand_cache(std::make_unique<RegisteredCache>(GetOrCreateDirectoryRelative(nand_root, "/user/Contents/registered"))),
    sysnand_placeholder(std::make_unique<PlaceholderCache>(GetOrCreateDirectoryRelative(nand_root, "/system/Contents/placehld"))),
    usrnand_placeholder(std::make_unique<PlaceholderCache>(GetOrCreateDirectoryRelative(nand_root, "/user/Contents/placehld")))
{
}

BISFactory::~BISFactory() = default;

VirtualDir BISFactory::GetSystemNANDContentDirectory() const
{
    return GetOrCreateDirectoryRelative(nand_root, "/system/Contents");
}

VirtualDir BISFactory::GetUserNANDContentDirectory() const
{
    return GetOrCreateDirectoryRelative(nand_root, "/user/Contents");
}

RegisteredCache * BISFactory::GetSystemNANDContents() const
{
    return sysnand_cache.get();
}

RegisteredCache * BISFactory::GetUserNANDContents() const
{
    return usrnand_cache.get();
}

PlaceholderCache * BISFactory::GetSystemNANDPlaceholder() const
{
    return sysnand_placeholder.get();
}

PlaceholderCache * BISFactory::GetUserNANDPlaceholder() const
{
    return usrnand_placeholder.get();
}

VirtualDir BISFactory::GetModificationLoadRoot(uint64_t title_id) const
{
    // LayeredFS doesn't work on updates and title id-less homebrew
    if (title_id == 0 || (title_id & 0xFFF) == 0x800)
    {
        return nullptr;
    }
    return GetOrCreateDirectoryRelative(load_root, fmt::format("/{:016X}", title_id));
}

VirtualDir BISFactory::GetModificationDumpRoot(uint64_t title_id) const
{
    if (title_id == 0)
    {
        return nullptr;
    }
    return GetOrCreateDirectoryRelative(dump_root, fmt::format("/{:016X}", title_id));
}

VirtualDir BISFactory::OpenPartition(BisPartitionId id) const
{
    switch (id)
    {
    case BisPartitionId::CalibrationFile:
        return GetOrCreateDirectoryRelative(nand_root, "/prodinfof");
    case BisPartitionId::SafeMode:
        return GetOrCreateDirectoryRelative(nand_root, "/safe");
    case BisPartitionId::System:
        return GetOrCreateDirectoryRelative(nand_root, "/system");
    case BisPartitionId::User:
        return GetOrCreateDirectoryRelative(nand_root, "/user");
    default:
        return nullptr;
    }
}

VirtualFile BISFactory::OpenPartitionStorage(BisPartitionId id, VirtualFilesystem file_system) const
{
    UNIMPLEMENTED();
    return nullptr;
}

VirtualDir BISFactory::GetImageDirectory() const
{
    return GetOrCreateDirectoryRelative(nand_root, "/user/Album");
}

uint64_t BISFactory::GetSystemNANDFreeSpace() const
{
    const auto sys_dir = GetOrCreateDirectoryRelative(nand_root, "/system");
    if (sys_dir == nullptr)
    {
        return GetSystemNANDTotalSpace();
    }

    const auto total_size = GetSystemNANDTotalSpace();
    const auto used_size = sys_dir->GetSize();
    return used_size < total_size ? total_size - used_size : 0;
}

uint64_t BISFactory::GetSystemNANDTotalSpace() const
{
    return NAND_SYSTEM_SIZE;
}

uint64_t BISFactory::GetUserNANDFreeSpace() const
{
    // For some reason games such as BioShock 1 checks whether this is exactly 0x680000000 bytes.
    // Set the free space to be 1 MiB less than the total as a workaround to this issue.
    return GetUserNANDTotalSpace() - 0x100000;
}

uint64_t BISFactory::GetUserNANDTotalSpace() const
{
    return NAND_USER_SIZE;
}

uint64_t BISFactory::GetFullNANDTotalSpace() const
{
    return NAND_TOTAL_SIZE;
}

VirtualDir BISFactory::GetBCATDirectory(uint64_t title_id) const
{
    return GetOrCreateDirectoryRelative(nand_root, fmt::format("/system/save/bcat/{:016X}", title_id));
}

} // namespace FileSys
