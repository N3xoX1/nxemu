// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>

#include "applets/applet.h"
#include "core/hle/service/am/frontend/applet_profile_select.h"
#include "yuzu_common/uuid.h"

struct ProfileSelectParameters
{
    Service::AM::Frontend::UiMode mode;
    std::array<Common::UUID, 8> invalid_uid_list;
    Service::AM::Frontend::UiSettingsDisplayOptions display_options;
    Service::AM::Frontend::UserSelectionPurpose purpose;
};

class DefaultProfileSelectApplet final : 
    public IProfileSelectApplet
{
public:
    void Close() override;
    void SelectProfile(void * user_data, ProfileSelectFinishedFn finished, const ProfileSelectHostParameters * parameters) const override;
};
