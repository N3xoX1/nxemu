// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Service::News {

class INewsService final : public ServiceFramework<INewsService> {
public:
    explicit INewsService(Core::System& system_);
    ~INewsService() override;

private:
    Result GetSubscriptionStatus(Out<u32> out_status, InBuffer<BufferAttr_HipcPointer> buffer_data);

    Result IsSystemUpdateRequired(Out<bool> out_is_system_update_required);

    Result RequestAutoSubscription(u64 value);
};

} // namespace Service::News
