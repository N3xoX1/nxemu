// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/hle/service/bcat/news/news_data_service.h"

namespace Service::News {

INewsDataService::INewsDataService(Core::System& system_)
    : ServiceFramework{system_, "INewsDataService"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "Open"},
        {1, nullptr, "OpenWithNewsRecordV1"},
        {2, nullptr, "Read"},
        {3, nullptr, "GetSize"},
        {1001, nullptr, "OpenWithNewsRecord"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

INewsDataService::~INewsDataService() = default;

} // namespace Service::News
