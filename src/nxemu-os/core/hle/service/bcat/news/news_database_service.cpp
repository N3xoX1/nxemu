// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/hle/service/bcat/news/news_database_service.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::News {

INewsDatabaseService::INewsDatabaseService(Core::System& system_)
    : ServiceFramework{system_, "INewsDatabaseService"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "GetListV1"},
        {1, D<&INewsDatabaseService::Count>, "Count"},
        {2, nullptr, "CountWithKey"},
        {3, nullptr, "UpdateIntegerValue"},
        {4, D<&INewsDatabaseService::UpdateIntegerValueWithAddition>, "UpdateIntegerValueWithAddition"},
        {5, nullptr, "UpdateStringValue"},
        {1000, D<&INewsDatabaseService::GetList>, "GetList"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

INewsDatabaseService::~INewsDatabaseService() = default;

Result INewsDatabaseService::Count(Out<s32> out_count,
                                   InBuffer<BufferAttr_HipcPointer> buffer_data) {
    LOG_WARNING(Service_BCAT, "(STUBBED) called, buffer_size={}", buffer_data.size());
    *out_count = 0;
    R_SUCCEED();
}

Result INewsDatabaseService::UpdateIntegerValueWithAddition(
    u32 value, InBuffer<BufferAttr_HipcPointer> buffer_data_1,
    InBuffer<BufferAttr_HipcPointer> buffer_data_2) {
    LOG_WARNING(Service_BCAT, "(STUBBED) called, value={}, buffer_size_1={}, buffer_data_2={}",
                value, buffer_data_1.size(), buffer_data_2.size());
    R_SUCCEED();
}

Result INewsDatabaseService::GetList(Out<s32> out_count, u32 value,
                                     OutBuffer<BufferAttr_HipcMapAlias> out_buffer_data,
                                     InBuffer<BufferAttr_HipcPointer> buffer_data_1,
                                     InBuffer<BufferAttr_HipcPointer> buffer_data_2) {
    LOG_WARNING(Service_BCAT, "(STUBBED) called, value={}, buffer_size_1={}, buffer_data_2={}",
                value, buffer_data_1.size(), buffer_data_2.size());
    *out_count = 0;
    R_SUCCEED();
}

} // namespace Service::News
