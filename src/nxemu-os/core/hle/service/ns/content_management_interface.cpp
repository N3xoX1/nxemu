// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/common_funcs.h"
#include "core/core.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/ns/content_management_interface.h"
#include "core/hle/service/ns/ns_types.h"
#include <nxemu-module-spec/system_loader.h>

namespace Service::NS {

IContentManagementInterface::IContentManagementInterface(Core::System& system_)
    : ServiceFramework{system_, "IContentManagementInterface"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {43, D<&IContentManagementInterface::CheckSdCardMountStatus>, "CheckSdCardMountStatus"},
        {47, D<&IContentManagementInterface::GetTotalSpaceSize>, "GetTotalSpaceSize"},
        {48, D<&IContentManagementInterface::GetFreeSpaceSize>, "GetFreeSpaceSize"},
        {600, nullptr, "CountApplicationContentMeta"},
        {601, nullptr, "ListApplicationContentMetaStatus"},
        {605, nullptr, "ListApplicationContentMetaStatusWithRightsCheck"},
        {607, nullptr, "IsAnyApplicationRunning"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IContentManagementInterface::~IContentManagementInterface() = default;

Result IContentManagementInterface::CheckSdCardMountStatus() {
    LOG_WARNING(Service_NS, "(STUBBED) called");
    R_SUCCEED();
}

Result IContentManagementInterface::GetTotalSpaceSize(Out<s64> out_total_space_size,
                                                      u64 storage_id) {
    LOG_DEBUG(Service_NS, "called, storage_id={}", storage_id);

    if (storage_id > static_cast<u64>(StorageId::SdCard)) {
        *out_total_space_size = 0;
        R_SUCCEED();
    }

    *out_total_space_size = static_cast<s64>(system.GetSystemloader()
                                                 .FileSystemController()
                                                 .GetTotalSpaceSize(static_cast<StorageId>(storage_id)));
    R_SUCCEED();
}

Result IContentManagementInterface::GetFreeSpaceSize(Out<s64> out_free_space_size,
                                                     u64 storage_id) {
    LOG_DEBUG(Service_NS, "called, storage_id={}", storage_id);

    if (storage_id > static_cast<u64>(StorageId::SdCard)) {
        *out_free_space_size = 0;
        R_SUCCEED();
    }

    *out_free_space_size = static_cast<s64>(system.GetSystemloader()
                                                .FileSystemController()
                                                .GetFreeSpaceSize(static_cast<StorageId>(storage_id)));
    R_SUCCEED();
}

} // namespace Service::NS
