// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "applets/controller.h"
#include "os_settings_identifiers.h"
#include <nxemu-module-spec/operating_system.h>
#include "nxemu-os/os_settings.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/settings.h"
#include "yuzu_common/yuzu_assert.h"
#include "yuzu_hid_core/frontend/emulated_controller.h"
#include "yuzu_hid_core/hid_core.h"
#include "yuzu_hid_core/hid_types.h"

extern IModuleSettings * g_settings;

DefaultControllerApplet::DefaultControllerApplet(Core::HID::HIDCore & hid_core_) :
    hid_core{hid_core_}
{
}

DefaultControllerApplet::~DefaultControllerApplet() = default;

void DefaultControllerApplet::Close()
{
}

void DefaultControllerApplet::ReconfigureControllers(void * user_data, ControllerReconfigureFn on_complete, const ControllerHostParameters * parameters)
{
    LOG_INFO(Service_HID, "called, deducing the best configuration based on the given parameters!");

    if (parameters == nullptr)
    {
        if (on_complete != nullptr)
        {
            on_complete(user_data, false);
        }
        return;
    }

    const auto finish = [&](bool ok) {
        if (on_complete != nullptr) {
            on_complete(user_data, ok);
        }
    };
    if (!parameters->enable_single_mode &&
        (parameters->min_players < 0 || parameters->max_players < parameters->min_players ||
         static_cast<std::size_t>(parameters->max_players) > hid_core.available_controllers - 2)) {
        finish(false);
        return;
    }
    const std::size_t min_supported_players = parameters->enable_single_mode ? 1U :
        static_cast<std::size_t>(parameters->min_players);

    auto * handheld = hid_core.GetEmulatedController(NpadIdType::Handheld);
    const bool handheld_allowed = parameters->allow_handheld &&
        g_settings->GetInt(NXOsSetting::DockedMode) != static_cast<int32_t>(DockedMode::Docked);
    if (!parameters->keep_controllers_connected || !handheld_allowed) {
        handheld->Disconnect();
    }

    const auto is_compatible = [parameters](NpadStyleIndex style) {
        switch (style) {
        case NpadStyleIndex::Fullkey: return parameters->allow_pro_controller;
        case NpadStyleIndex::JoyconDual: return parameters->allow_dual_joycons;
        case NpadStyleIndex::JoyconLeft: return parameters->allow_left_joycon;
        case NpadStyleIndex::JoyconRight: return parameters->allow_right_joycon;
        case NpadStyleIndex::GameCube: return parameters->allow_gamecube_controller;
        default: return false;
        }
    };
    std::size_t connected_players = 0;
    const std::size_t max_supported_players = parameters->enable_single_mode ? 1U :
        static_cast<std::size_t>(parameters->max_players);
    for (std::size_t index = 0; index < hid_core.available_controllers - 2; ++index) {
        auto * controller = hid_core.GetEmulatedControllerByIndex(index);
        if (parameters->keep_controllers_connected && controller->IsConnected() &&
            connected_players < max_supported_players &&
            is_compatible(controller->GetNpadStyleIndex())) {
            ++connected_players;
            continue;
        }
        controller->Disconnect();
    }
    if (parameters->keep_controllers_connected && handheld_allowed && handheld->IsConnected() &&
        connected_players < max_supported_players) {
        ++connected_players;
    } else {
        handheld->Disconnect();
    }

    // Attach only the missing players, preserving supported controller assignments.
    for (std::size_t index = 0; index < hid_core.available_controllers - 2; ++index)
    {
        auto * controller = hid_core.GetEmulatedControllerByIndex(index);
        if (connected_players >= min_supported_players) {
            break;
        }
        if (controller->IsConnected()) {
            continue;
        }

        // Connect controllers based on the following priority list from highest to lowest priority:
        // Pro Controller -> Dual Joycons -> Left Joycon/Right Joycon -> GameCube -> Handheld
        if (parameters->allow_pro_controller)
        {
            controller->SetNpadStyleIndex(NpadStyleIndex::Fullkey);
            controller->Connect(true);
        }
        else if (parameters->allow_dual_joycons)
        {
            controller->SetNpadStyleIndex(NpadStyleIndex::JoyconDual);
            controller->Connect(true);
        }
        else if (parameters->allow_left_joycon || parameters->allow_right_joycon)
        {
            // Assign left joycons to even player indices and right joycons to odd player indices.
            // We do this since Captain Toad Treasure Tracker expects a left joycon for Player 1 and
            // a right Joycon for Player 2 in 2 Player Assist mode.
            if (parameters->allow_left_joycon && (!parameters->allow_right_joycon || index % 2 == 0))
            {
                controller->SetNpadStyleIndex(NpadStyleIndex::JoyconLeft);
                controller->Connect(true);
            }
            else
            {
                controller->SetNpadStyleIndex(NpadStyleIndex::JoyconRight);
                controller->Connect(true);
            }
        }
        else if (parameters->allow_gamecube_controller)
        {
            controller->SetNpadStyleIndex(NpadStyleIndex::GameCube);
            controller->Connect(true);
        }
        else if (index == 0 && parameters->enable_single_mode && parameters->allow_handheld && g_settings->GetInt(NXOsSetting::DockedMode) != static_cast<int32_t>(DockedMode::Docked))
        {
            // We should *never* reach here under any normal circumstances.
            handheld->SetNpadStyleIndex(NpadStyleIndex::Handheld);
            handheld->Connect(true);
            ++connected_players;
            break;
        }
        else
        {
            finish(false);
            return;
        }
        if (controller->IsConnected()) {
            ++connected_players;
        }
    }

    finish(connected_players >= min_supported_players);
}
