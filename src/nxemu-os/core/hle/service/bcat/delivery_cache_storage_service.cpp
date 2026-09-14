// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <string_view>

#include "core/hle/service/bcat/bcat_result.h"
#include "core/hle/service/bcat/delivery_cache_directory_service.h"
#include "core/hle/service/bcat/delivery_cache_file_service.h"
#include "core/hle/service/bcat/delivery_cache_storage_service.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::BCAT {

IDeliveryCacheStorageService::IDeliveryCacheStorageService(Core::System& system_,
                                                           IVirtualDirectoryPtr root_)
    : ServiceFramework{system_, "IDeliveryCacheStorageService"}, root(std::move(root_))
{
    if (root) {
        IVirtualDirectoryListPtr directories(root->GetSubdirectories());
        if (directories) {
            entries.reserve(directories->GetSize());
            for (u32 i = 0; i < directories->GetSize(); ++i) {
                IVirtualDirectoryPtr directory(directories->GetItem(i));
                const std::string_view directory_name(directory->GetName());
                DirectoryName name{};
                if (directory_name.empty() || directory_name.size() >= name.size()) {
                    continue;
                }

                std::memcpy(name.data(), directory_name.data(), directory_name.size());
                entries.emplace_back(name);
            }
        }
    }
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IDeliveryCacheStorageService::CreateFileService>, "CreateFileService"},
        {1, D<&IDeliveryCacheStorageService::CreateDirectoryService>, "CreateDirectoryService"},
        {10, D<&IDeliveryCacheStorageService::EnumerateDeliveryCacheDirectory>, "EnumerateDeliveryCacheDirectory"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IDeliveryCacheStorageService::~IDeliveryCacheStorageService() = default;

Result IDeliveryCacheStorageService::CreateFileService(OutInterface<IDeliveryCacheFileService> out_interface)
{
    LOG_DEBUG(Service_BCAT, "called");
    IVirtualDirectoryPtr root_copy(root ? root->Duplicate() : nullptr);
    *out_interface =
        std::make_shared<IDeliveryCacheFileService>(system, std::move(root_copy));
    R_SUCCEED();
}

Result IDeliveryCacheStorageService::CreateDirectoryService(OutInterface<IDeliveryCacheDirectoryService> out_interface)
{
    LOG_DEBUG(Service_BCAT, "called");

    IVirtualDirectoryPtr root_copy(root ? root->Duplicate() : nullptr);
    *out_interface =
        std::make_shared<IDeliveryCacheDirectoryService>(system, std::move(root_copy));
    R_SUCCEED();
}

Result IDeliveryCacheStorageService::EnumerateDeliveryCacheDirectory(Out<s32> out_directory_count, OutArray<DirectoryName, BufferAttr_HipcMapAlias> out_directories)
{
    LOG_DEBUG(Service_BCAT, "called, size={:016X}", out_directories.size());

    const std::size_t remaining = entries.size() - next_read_index;
    *out_directory_count = static_cast<s32>(std::min(out_directories.size(), remaining));
    if (*out_directory_count > 0) {
        std::memcpy(out_directories.data(), entries.data() + next_read_index,
                    static_cast<std::size_t>(*out_directory_count) * sizeof(DirectoryName));
        next_read_index += static_cast<std::size_t>(*out_directory_count);
    }
    R_SUCCEED();
}

} // namespace Service::BCAT
