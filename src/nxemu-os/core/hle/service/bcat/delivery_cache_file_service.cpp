// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_common/string_util.h"
#include "core/hle/service/bcat/bcat_result.h"
#include "core/hle/service/bcat/bcat_util.h"
#include "core/hle/service/bcat/delivery_cache_file_service.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::BCAT {

IDeliveryCacheFileService::IDeliveryCacheFileService(Core::System& system_,
                                                     IVirtualDirectoryPtr root_)
    : ServiceFramework{system_, "IDeliveryCacheFileService"}, root(std::move(root_))
{
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IDeliveryCacheFileService::Open>, "Open"},
        {1, D<&IDeliveryCacheFileService::Read>, "Read"},
        {2, D<&IDeliveryCacheFileService::GetSize>, "GetSize"},
        {3, D<&IDeliveryCacheFileService::GetDigest>, "GetDigest"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IDeliveryCacheFileService::~IDeliveryCacheFileService() = default;

Result IDeliveryCacheFileService::Open(const DirectoryName& dir_name_raw,
                                       const FileName& file_name_raw) {
    const auto dir_name = Common::StringFromFixedZeroTerminatedBuffer(dir_name_raw.data(), dir_name_raw.size());
    const auto file_name = Common::StringFromFixedZeroTerminatedBuffer(file_name_raw.data(), file_name_raw.size());

    LOG_DEBUG(Service_BCAT, "called, dir_name={}, file_name={}", dir_name, file_name);

    R_TRY(VerifyNameValidDir(dir_name_raw));
    R_TRY(VerifyNameValidFile(file_name_raw));
    R_UNLESS(!current_file, ResultEntityAlreadyOpen);

    R_UNLESS(root, ResultFailedOpenEntity);
    IVirtualDirectoryPtr dir(root->GetSubdirectory(dir_name.c_str()));
    R_UNLESS(dir, ResultFailedOpenEntity);

    current_file = IVirtualFilePtr(dir->GetFile(file_name.c_str()));
    R_UNLESS(current_file, ResultFailedOpenEntity);

    R_SUCCEED();
}

Result IDeliveryCacheFileService::Read(Out<u64> out_buffer_size, u64 offset,
                                       OutBuffer<BufferAttr_HipcMapAlias> out_buffer) {
    LOG_DEBUG(Service_BCAT, "called, offset={:016X}, size={:016X}", offset, out_buffer.size());

    R_UNLESS(current_file, ResultNoOpenEntry);

    if (offset >= current_file->GetSize()) {
        *out_buffer_size = 0;
        R_SUCCEED();
    }

    const u64 bytes_to_read =
        std::min<u64>(current_file->GetSize() - offset, out_buffer.size());
    *out_buffer_size = current_file->ReadBytes(out_buffer.data(), bytes_to_read, offset);
    R_SUCCEED();
}

Result IDeliveryCacheFileService::GetSize(Out<u64> out_size) {
    LOG_DEBUG(Service_BCAT, "called");

    R_UNLESS(current_file, ResultNoOpenEntry);
    *out_size = current_file->GetSize();
    R_SUCCEED();
}

Result IDeliveryCacheFileService::GetDigest(Out<BcatDigest> out_digest) {
    LOG_DEBUG(Service_BCAT, "called");

    R_UNLESS(current_file, ResultNoOpenEntry);
    const auto bytes = current_file.ReadAllBytes();
    *out_digest = DigestBytes(bytes);
    R_SUCCEED();
}

} // namespace Service::BCAT
