// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <string_view>

#include "yuzu_common/string_util.h"
#include "core/hle/service/bcat/bcat_result.h"
#include "core/hle/service/bcat/bcat_util.h"
#include "core/hle/service/bcat/delivery_cache_directory_service.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::BCAT {

IDeliveryCacheDirectoryService::IDeliveryCacheDirectoryService(Core::System& system_,
                                                               IVirtualDirectoryPtr root_)
    : ServiceFramework{system_, "IDeliveryCacheDirectoryService"}, root(std::move(root_))
{
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IDeliveryCacheDirectoryService::Open>, "Open"},
        {1, D<&IDeliveryCacheDirectoryService::Read>, "Read"},
        {2, D<&IDeliveryCacheDirectoryService::GetCount>, "GetCount"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IDeliveryCacheDirectoryService::~IDeliveryCacheDirectoryService() = default;

Result IDeliveryCacheDirectoryService::Open(const DirectoryName& dir_name_raw)
{
    const auto dir_name = Common::StringFromFixedZeroTerminatedBuffer(dir_name_raw.data(), dir_name_raw.size());

    LOG_DEBUG(Service_BCAT, "called, dir_name={}", dir_name);

    R_TRY(VerifyNameValidDir(dir_name_raw));
    R_UNLESS(!current_dir, ResultEntityAlreadyOpen);

    R_UNLESS(root, ResultFailedOpenEntity);
    current_dir = IVirtualDirectoryPtr(root->GetSubdirectory(dir_name.c_str()));
    R_UNLESS(current_dir, ResultFailedOpenEntity);

    R_SUCCEED();
}

Result IDeliveryCacheDirectoryService::Read(Out<s32> out_count, OutArray<DeliveryCacheDirectoryEntry, BufferAttr_HipcMapAlias> out_buffer)
{
    LOG_DEBUG(Service_BCAT, "called, write_size={:016X}", out_buffer.size());

    R_UNLESS(current_dir, ResultNoOpenEntry);
    IVirtualFileListPtr files(current_dir->GetFiles());
    R_UNLESS(files, ResultFailedOpenEntity);

    const auto output_count =
        static_cast<u32>(std::min<std::size_t>(files->GetSize(), out_buffer.size()));
    *out_count = static_cast<s32>(output_count);
    for (u32 i = 0; i < output_count; ++i) {
        IVirtualFilePtr file(files->GetItem(i));
        FileName name{};
        const std::string_view file_name(file->GetName());
        std::memcpy(name.data(), file_name.data(),
                    (std::min)(file_name.size(), name.size() - 1));
        const auto bytes = file.ReadAllBytes();
        out_buffer[i] = DeliveryCacheDirectoryEntry{name, file->GetSize(), DigestBytes(bytes)};
    }
    R_SUCCEED();
}

Result IDeliveryCacheDirectoryService::GetCount(Out<s32> out_count)
{
    LOG_DEBUG(Service_BCAT, "called");

    R_UNLESS(current_dir, ResultNoOpenEntry);
    IVirtualFileListPtr files(current_dir->GetFiles());
    R_UNLESS(files, ResultFailedOpenEntity);
    *out_count = static_cast<s32>(files->GetSize());
    R_SUCCEED();
}

} // namespace Service::BCAT
