// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "applets/profile_select.h"
#include "core/hle/service/acc/profile_manager.h"
#include <nxemu-module-spec/base.h>
#include "os_settings.h"
#include "os_settings_identifiers.h"

extern IModuleSettings * g_settings;

void DefaultProfileSelectApplet::Close()
{
}

void DefaultProfileSelectApplet::SelectProfile(void * user_data, ProfileSelectFinishedFn finished, const ProfileSelectHostParameters * parameters) const
{
    (void)parameters;

    Service::Account::ProfileManager manager;
    const std::optional<Common::UUID> user = manager.GetUser(osSettings.current_user);

    uint8_t uuid_bytes[16]{};
    bool has_uuid = false;
    if (user.has_value() && user->IsValid())
    {
        has_uuid = true;
        std::memcpy(uuid_bytes, user->uuid.data(), sizeof(uuid_bytes));
    }

    if (finished != nullptr)
    {
        finished(user_data, has_uuid, uuid_bytes);
    }
    LOG_INFO(Service_ACC, "called, selecting current user instead of prompting...");
}