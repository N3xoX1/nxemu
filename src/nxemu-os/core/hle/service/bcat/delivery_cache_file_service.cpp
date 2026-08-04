// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_common/string_util.h"
#include "core/hle/service/bcat/bcat_result.h"
#include "core/hle/service/bcat/bcat_util.h"
#include "core/hle/service/bcat/delivery_cache_file_service.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::BCAT {

IDeliveryCacheFileService::IDeliveryCacheFileService(Core::System& system_) : 
    ServiceFramework{system_, "IDeliveryCacheFileService"}
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

    UNIMPLEMENTED();

    R_SUCCEED();
}

Result IDeliveryCacheFileService::Read(Out<u64> out_buffer_size, u64 offset,
                                       OutBuffer<BufferAttr_HipcMapAlias> out_buffer) {
    LOG_DEBUG(Service_BCAT, "called, offset={:016X}, size={:016X}", offset, out_buffer.size());

    UNIMPLEMENTED();
    R_SUCCEED();
}

Result IDeliveryCacheFileService::GetSize(Out<u64> out_size) {
    LOG_DEBUG(Service_BCAT, "called");

    UNIMPLEMENTED();
    R_SUCCEED();
}

Result IDeliveryCacheFileService::GetDigest(Out<BcatDigest> out_digest) {
    LOG_DEBUG(Service_BCAT, "called");

    UNIMPLEMENTED();
    R_SUCCEED();
}

} // namespace Service::BCAT
