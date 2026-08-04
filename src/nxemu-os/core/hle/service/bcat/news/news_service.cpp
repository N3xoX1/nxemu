// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/hle/service/bcat/news/news_service.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::News {

INewsService::INewsService(Core::System& system_) : ServiceFramework{system_, "INewsService"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {10100, nullptr, "PostLocalNews"},
        {20100, nullptr, "SetPassphrase"},
        {30100, D<&INewsService::GetSubscriptionStatus>, "GetSubscriptionStatus"},
        {30101, nullptr, "GetTopicList"},
        {30110, nullptr, "Unknown30110"},
        {30200, D<&INewsService::IsSystemUpdateRequired>, "IsSystemUpdateRequired"},
        {30201, nullptr, "Unknown30201"},
        {30210, nullptr, "Unknown30210"},
        {30300, nullptr, "RequestImmediateReception"},
        {30400, nullptr, "DecodeArchiveFile"},
        {30500, nullptr, "Unknown30500"},
        {30900, nullptr, "Unknown30900"},
        {30901, nullptr, "Unknown30901"},
        {30902, nullptr, "Unknown30902"},
        {40100, nullptr, "SetSubscriptionStatus"},
        {40101, D<&INewsService::RequestAutoSubscription>, "RequestAutoSubscription"},
        {40200, nullptr, "ClearStorage"},
        {40201, nullptr, "ClearSubscriptionStatusAll"},
        {90100, nullptr, "GetNewsDatabaseDump"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

INewsService::~INewsService() = default;

Result INewsService::GetSubscriptionStatus(Out<u32> out_status,
                                           InBuffer<BufferAttr_HipcPointer> buffer_data) {
    LOG_WARNING(Service_BCAT, "(STUBBED) called, buffer_size={}", buffer_data.size());
    *out_status = 0;
    R_SUCCEED();
}

Result INewsService::IsSystemUpdateRequired(Out<bool> out_is_system_update_required) {
    LOG_WARNING(Service_BCAT, "(STUBBED) called");
    *out_is_system_update_required = false;
    R_SUCCEED();
}

Result INewsService::RequestAutoSubscription(u64 value) {
    LOG_WARNING(Service_BCAT, "(STUBBED) called");
    R_SUCCEED();
}

} // namespace Service::News
