// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include "core/core.h"
#include "applets/profile_select.h"
#include "core/hle/service/acc/errors.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/am/frontend/applet_profile_select.h"
#include "core/hle/service/am/service/storage.h"
#include "yuzu_common/string_util.h"
#include "yuzu_common/yuzu_assert.h"

namespace
{

ProfileSelectHostParameters CreateHostParameters(Service::AM::Frontend::UiMode mode, const std::array<Common::UUID, 8> & invalid_uid_list, const Service::AM::Frontend::UiSettingsDisplayOptions & display_options, Service::AM::Frontend::UserSelectionPurpose purpose)
{
    ProfileSelectHostParameters parameters{};
    parameters.mode = (ProfileUiMode)mode;
    for (size_t i = 0; i < invalid_uid_list.size(); ++i)
    {
        std::memcpy(parameters.invalid_uid_list[i], invalid_uid_list[i].uuid.data(), sizeof(parameters.invalid_uid_list[i]));
    }
    parameters.display_options = {
        .is_network_service_account_required = display_options.is_network_service_account_required,
        .is_skip_enabled = display_options.is_skip_enabled,
        .is_system_or_launcher = display_options.is_system_or_launcher,
        .is_registration_permitted = display_options.is_registration_permitted,
        .show_skip_button = display_options.show_skip_button,
        .additional_select = display_options.additional_select,
        .show_user_selector = display_options.show_user_selector,
        .is_unqualified_user_selectable = display_options.is_unqualified_user_selectable,
    };
    parameters.purpose = (UserSelectionPurposeHost)purpose;
    return parameters;
}

} // namespace

namespace Service::AM::Frontend
{

ProfileSelect::ProfileSelect(Core::System & system_, std::shared_ptr<Applet> applet_, LibraryAppletMode applet_mode_, IProfileSelectApplet & frontend_) :
    FrontendApplet{system_, applet_, applet_mode_},
    frontend{frontend_}
{
}

ProfileSelect::~ProfileSelect() = default;

void ProfileSelect::Initialize()
{
    complete = false;
    status = ResultSuccess;
    final_data.clear();

    FrontendApplet::Initialize();
    profile_select_version = ProfileSelectAppletVersion{common_args.library_version};

    const std::shared_ptr<IStorage> user_config_storage = PopInData();
    ASSERT(user_config_storage != nullptr);
    const auto & user_config = user_config_storage->GetData();

    LOG_INFO(Service_AM, "Initializing Profile Select Applet with version={}", profile_select_version);

    switch (profile_select_version)
    {
    case ProfileSelectAppletVersion::Version1:
        ASSERT(user_config.size() == sizeof(UiSettingsV1));
        std::memcpy(&config_old, user_config.data(), sizeof(UiSettingsV1));
        break;
    case ProfileSelectAppletVersion::Version2:
    case ProfileSelectAppletVersion::Version3:
        ASSERT(user_config.size() == sizeof(UiSettings));
        std::memcpy(&config, user_config.data(), sizeof(UiSettings));
        break;
    default:
        UNIMPLEMENTED_MSG("Unknown profile_select_version = {}", profile_select_version);
        break;
    }
}

Result ProfileSelect::GetStatus() const
{
    return status;
}

void ProfileSelect::ExecuteInteractive()
{
    ASSERT_MSG(false, "Attempted to call interactive execution on non-interactive applet.");
}

void ProfileSelect::Execute()
{
    if (complete)
    {
        PushOutData(std::make_shared<IStorage>(system, std::move(final_data)));
        Exit();
        return;
    }

    ProfileSelectHostParameters parameters{};

    switch (profile_select_version)
    {
    case ProfileSelectAppletVersion::Version1:
        parameters = CreateHostParameters(config_old.mode, config_old.invalid_uid_list, config_old.display_options, UserSelectionPurpose::General);
        break;
    case ProfileSelectAppletVersion::Version2:
    case ProfileSelectAppletVersion::Version3:
        parameters = CreateHostParameters(config.mode, config.invalid_uid_list, config.display_options, config.purpose);
        break;
    default:
        UNIMPLEMENTED_MSG("Unknown profile_select_version = {}", profile_select_version);
        break;
    }

    frontend.SelectProfile(this, OnProfileSelected, &parameters);
}

void ProfileSelect::SelectionComplete(std::optional<Common::UUID> uuid)
{
    UiReturnArg output{};

    if (uuid.has_value() && uuid->IsValid())
    {
        output.result = 0;
        output.uuid_selected = *uuid;
    }
    else
    {
        status = Account::ResultCancelledByUser;
        output.result = Account::ResultCancelledByUser.raw;
        output.uuid_selected = Common::InvalidUUID;
    }

    final_data = std::vector<u8>(sizeof(UiReturnArg));
    std::memcpy(final_data.data(), &output, final_data.size());

    PushOutData(std::make_shared<IStorage>(system, std::move(final_data)));
    Exit();
}

Result ProfileSelect::RequestExit()
{
    frontend.Close();
    R_SUCCEED();
}

void CALL ProfileSelect::OnProfileSelected(void * user_data, bool has_uuid, const uint8_t uuid_bytes[16])
{
    ProfileSelect * const self = (ProfileSelect *)user_data;
    if (has_uuid)
    {
        std::array<u8, 16> uuid{};
        std::memcpy(uuid.data(), uuid_bytes, uuid.size());
        self->SelectionComplete(Common::UUID{uuid});
    }
    else
    {
        self->SelectionComplete(std::nullopt);
    }
}

} // namespace Service::AM::Frontend
