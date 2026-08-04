// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_common/string_util.h"
#include "core/hle/service/bcat/bcat_result.h"
#include "core/hle/service/bcat/bcat_util.h"
#include "core/hle/service/bcat/delivery_cache_directory_service.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::BCAT {

IDeliveryCacheDirectoryService::IDeliveryCacheDirectoryService(Core::System& system_) : 
    ServiceFramework{system_, "IDeliveryCacheDirectoryService"}
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
    UNIMPLEMENTED();

    R_SUCCEED();
}

Result IDeliveryCacheDirectoryService::Read(Out<s32> out_count, OutArray<DeliveryCacheDirectoryEntry, BufferAttr_HipcMapAlias> out_buffer)
{
    LOG_DEBUG(Service_BCAT, "called, write_size={:016X}", out_buffer.size());

    UNIMPLEMENTED();
    R_SUCCEED();
}

Result IDeliveryCacheDirectoryService::GetCount(Out<s32> out_count)
{
    LOG_DEBUG(Service_BCAT, "called");

    UNIMPLEMENTED();
    R_SUCCEED();
}

} // namespace Service::BCAT
