// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/uuid.h"
#include <nxemu-module-spec/base.h>
#include "os_settings_identifiers.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/am/applet_data_broker.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/event_observer.h"
#include "core/hle/service/am/frontend/applet_cabinet.h"
#include "core/hle/service/am/frontend/applet_controller.h"
#include "core/hle/service/am/frontend/applet_software_keyboard_types.h"
#include "core/hle/service/am/service/storage.h"
#include "core/hle/service/am/window_system.h"

extern IModuleSettings * g_settings;

namespace Service::AM {

namespace {

constexpr u32 LaunchParameterAccountPreselectedUserMagic = 0xC79497CA;

struct LaunchParameterAccountPreselectedUser {
    u32 magic;
    u32 is_account_selected;
    Common::UUID current_user;
    INSERT_PADDING_BYTES(0x70);
};
static_assert(sizeof(LaunchParameterAccountPreselectedUser) == 0x88);

AppletStorageChannel& InitializeFakeCallerApplet(Core::System& system,
                                                 std::shared_ptr<Applet>& applet) {
    applet->caller_applet_broker = std::make_shared<AppletDataBroker>(system);
    return applet->caller_applet_broker->GetInData();
}

void PushInShowQlaunch(Core::System& system, AppletStorageChannel& channel) {
    const CommonArguments arguments{
        .arguments_version = CommonArgumentVersion::Version3,
        .size = CommonArgumentSize::Version3,
        .library_version = 0,
        .theme_color = ThemeColor::BasicBlack,
        .play_startup_sound = true,
        .system_tick = system.CoreTiming().GetClockTicks(),
    };

    std::vector<u8> argument_data(sizeof(arguments));
    std::memcpy(argument_data.data(), &arguments, sizeof(arguments));
    channel.Push(std::make_shared<IStorage>(system, std::move(argument_data)));
}

void PushInShowAlbum(Core::System& system, AppletStorageChannel& channel) {
    const CommonArguments arguments{
        .arguments_version = CommonArgumentVersion::Version3,
        .size = CommonArgumentSize::Version3,
        .library_version = 1,
        .theme_color = ThemeColor::BasicBlack,
        .play_startup_sound = true,
        .system_tick = system.CoreTiming().GetClockTicks(),
    };

    std::vector<u8> argument_data(sizeof(arguments));
    std::vector<u8> settings_data{2};
    std::memcpy(argument_data.data(), &arguments, sizeof(arguments));
    channel.Push(std::make_shared<IStorage>(system, std::move(argument_data)));
    channel.Push(std::make_shared<IStorage>(system, std::move(settings_data)));
}

void PushInShowController(Core::System& system, AppletStorageChannel& channel) {
    UNIMPLEMENTED();
}

void PushInShowCabinetData(Core::System& system, AppletStorageChannel& channel) {
    UNIMPLEMENTED();
}

void PushInShowMiiEditData(Core::System& system, AppletStorageChannel& channel) {
    UNIMPLEMENTED();
}

void PushInShowSoftwareKeyboard(Core::System& system, AppletStorageChannel& channel) {
    const CommonArguments arguments{
        .arguments_version = CommonArgumentVersion::Version3,
        .size = CommonArgumentSize::Version3,
        .library_version = static_cast<u32>(Frontend::SwkbdAppletVersion::Version524301),
        .theme_color = ThemeColor::BasicBlack,
        .play_startup_sound = true,
        .system_tick = system.CoreTiming().GetClockTicks(),
    };

    std::vector<char16_t> initial_string(0);

    const Frontend::SwkbdConfigCommon swkbd_config{
        .type = Frontend::SwkbdType::Qwerty,
        .ok_text{},
        .left_optional_symbol_key{},
        .right_optional_symbol_key{},
        .use_prediction = false,
        .key_disable_flags{},
        .initial_cursor_position = Frontend::SwkbdInitialCursorPosition::Start,
        .header_text{},
        .sub_text{},
        .guide_text{},
        .max_text_length = 500,
        .min_text_length = 0,
        .password_mode = Frontend::SwkbdPasswordMode::Disabled,
        .text_draw_type = Frontend::SwkbdTextDrawType::Box,
        .enable_return_button = true,
        .use_utf8 = false,
        .use_blur_background = true,
        .initial_string_offset{},
        .initial_string_length = static_cast<u32>(initial_string.size()),
        .user_dictionary_offset{},
        .user_dictionary_entries{},
        .use_text_check = false,
    };

    Frontend::SwkbdConfigNew swkbd_config_new{};

    std::vector<u8> argument_data(sizeof(arguments));
    std::vector<u8> swkbd_data(sizeof(swkbd_config) + sizeof(swkbd_config_new));
    std::vector<u8> work_buffer(swkbd_config.initial_string_length * sizeof(char16_t));

    std::memcpy(argument_data.data(), &arguments, sizeof(arguments));
    std::memcpy(swkbd_data.data(), &swkbd_config, sizeof(swkbd_config));
    std::memcpy(swkbd_data.data() + sizeof(swkbd_config), &swkbd_config_new,
                sizeof(Frontend::SwkbdConfigNew));
    std::memcpy(work_buffer.data(), initial_string.data(),
                swkbd_config.initial_string_length * sizeof(char16_t));

    channel.Push(std::make_shared<IStorage>(system, std::move(argument_data)));
    channel.Push(std::make_shared<IStorage>(system, std::move(swkbd_data)));
    channel.Push(std::make_shared<IStorage>(system, std::move(work_buffer)));
}

} // namespace

AppletManager::AppletManager(Core::System& system)
    : m_system(system), m_window_system(std::make_unique<WindowSystem>(system)) {}

AppletManager::~AppletManager() {
    this->Reset();
}

void AppletManager::EnsureEventObserver() {
    std::scoped_lock lk{m_event_observer_mutex};
    if (!m_event_observer) {
        m_event_observer = std::make_unique<EventObserver>(m_system, *m_window_system);
    }
}

void AppletManager::InsertApplet(std::shared_ptr<Applet> applet) {
    EnsureEventObserver();
    const bool is_application = applet->type == AppletType::Application;
    m_window_system->TrackApplet(std::move(applet), is_application);
}

void AppletManager::NotifyAppletStarted(AppletResourceUserId aruid) {
    const auto applet = m_window_system->GetByAppletResourceUserId(aruid);
    if (applet) {
        m_window_system->NotifyAppletStarted(*applet);
    }
}

bool AppletManager::EnsureHidRegistered(AppletResourceUserId aruid) {
    const auto applet = m_window_system->GetByAppletResourceUserId(aruid);
    if (!applet) {
        return false;
    }
    std::scoped_lock lk{applet->lock};
    return applet->hid_registration.EnsureRegistered();
}

void AppletManager::CreateAndInsertByFrontendAppletParameters(
    AppletResourceUserId aruid, const FrontendAppletParameters& params,
    Kernel::KProcess* lifecycle_process) {
    auto applet = std::make_shared<Applet>(
        m_system, std::make_unique<Process>(m_system), params.applet_id == AppletId::Application,
        lifecycle_process);

    applet->SetAppletResourceUserId(aruid);
    applet->program_id = params.program_id;
    applet->applet_id = params.applet_id;
    applet->type = params.applet_type;
    applet->program_index = params.program_index;
    applet->previous_program_index = params.previous_program_index;

    // Push UserChannel data from previous application
    if (params.launch_type == LaunchType::ApplicationInitiated) {
        applet->user_channel_launch_parameter.swap(m_system.GetUserChannel());
    }

    // TODO: Read whether we need a preselected user from NACP?
    // TODO: This can be done quite easily from loader
    {
        LaunchParameterAccountPreselectedUser lp{};

        lp.magic = LaunchParameterAccountPreselectedUserMagic;
        lp.is_account_selected = 1;

        Account::ProfileManager profile_manager{};
        const auto uuid = profile_manager.GetUser(static_cast<std::size_t>(
            g_settings->GetInt(NXOsSetting::CurrentUser)));
        ASSERT(uuid.has_value() && uuid->IsValid());
        lp.current_user = *uuid;

        std::vector<u8> buffer(sizeof(LaunchParameterAccountPreselectedUser));
        std::memcpy(buffer.data(), &lp, buffer.size());

        applet->preselected_user_launch_parameter.push_back(std::move(buffer));
    }

    // Starting from frontend, some applets require input data.
    switch (applet->applet_id) {
    case AppletId::QLaunch:
        PushInShowQlaunch(m_system, InitializeFakeCallerApplet(m_system, applet));
        break;
    case AppletId::Cabinet:
        PushInShowCabinetData(m_system, InitializeFakeCallerApplet(m_system, applet));
        break;
    case AppletId::MiiEdit:
        PushInShowMiiEditData(m_system, InitializeFakeCallerApplet(m_system, applet));
        break;
    case AppletId::PhotoViewer:
        PushInShowAlbum(m_system, InitializeFakeCallerApplet(m_system, applet));
        break;
    case AppletId::SoftwareKeyboard:
        PushInShowSoftwareKeyboard(m_system, InitializeFakeCallerApplet(m_system, applet));
        break;
    case AppletId::Controller:
        PushInShowController(m_system, InitializeFakeCallerApplet(m_system, applet));
        break;
    default:
        break;
    }

    applet->lifecycle_manager.SetFocusState(FocusState::InFocus);
    if (applet->applet_id == AppletId::QLaunch) {
        applet->lifecycle_manager.SetFocusHandlingMode(false);
        applet->lifecycle_manager.SetOutOfFocusSuspendingEnabled(false);
    }

    const bool is_application = applet->applet_id != AppletId::QLaunch;
    EnsureEventObserver();
    m_window_system->TrackApplet(applet, is_application);
    if (applet->applet_id == AppletId::QLaunch) {
        m_window_system->RequestHomeMenuToGetForeground();
    } else {
        m_window_system->RequestApplicationToGetForeground();
    }
}

std::shared_ptr<Applet> AppletManager::GetByAppletResourceUserId(AppletResourceUserId aruid) const {
    return m_window_system->GetByAppletResourceUserId(aruid);
}

std::shared_ptr<Applet> AppletManager::GetApplicationApplet() const {
    return m_window_system->GetApplicationApplet();
}

void AppletManager::StopEventObserver() {
    std::scoped_lock lk{m_event_observer_mutex};
    m_event_observer.reset();
}

void AppletManager::Reset() {
    StopEventObserver();
    m_window_system->Reset();
}

void AppletManager::RequestExit() {
    m_window_system->OnExitRequested();
}

void AppletManager::RequestResume() {
    m_window_system->OnResumeRequested();
}

void AppletManager::OperationModeChanged() {
    m_window_system->OnOperationModeChanged();
}

void AppletManager::FocusStateChanged() {
    m_window_system->Update();
}

WindowSystem& AppletManager::GetWindowSystem() {
    return *m_window_system;
}

const WindowSystem& AppletManager::GetWindowSystem() const {
    return *m_window_system;
}

} // namespace Service::AM
