// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/set/setting_formats/system_settings.h"
#include "os_settings.h"

namespace Service::Set
{

void SetDefaultAudioOutputModes(SystemSettings& settings)
{
    // Surround on HDMI, so that a game reading the setting keeps its 5.1 mix: the host plays
    // what the player chose, mixing 5.1 down when needed. The console's own outputs are stereo.
    settings.audio_output_mode_hdmi = AudioOutputMode::ch_5_1;
    settings.audio_output_mode_speaker = AudioOutputMode::ch_2;
    settings.audio_output_mode_headphone = AudioOutputMode::ch_2;
    settings.audio_output_mode_type3 = AudioOutputMode::ch_2;
    settings.audio_output_mode_type4 = AudioOutputMode::ch_2;
}

SystemSettings DefaultSystemSettings()
{
    SystemSettings settings{};

    settings.version = 0x140000;
    settings.flags = 7;
    settings.mii_author_id = Common::UUID::MakeDefault();
    settings.color_set_id = ColorSet::BasicWhite;
    settings.notification_settings = {
        .flags{0x300},
        .volume = NotificationVolume::High,
        .start_time = {.hour = 9, .minute = 0},
        .stop_time = {.hour = 21, .minute = 0},
    };

    settings.tv_settings = {
        .flags = {0xC},
        .tv_resolution = TvResolution::Auto,
        .hdmi_content_type = HdmiContentType::Game,
        .rgb_range = RgbRange::Auto,
        .cmu_mode = CmuMode::None,
        .tv_underscan = {},
        .tv_gama = 1.0f,
        .contrast_ratio = 0.5f,
    };

    SetDefaultAudioOutputModes(settings);

    settings.initial_launch_settings_packed = {
        .flags = {0x10001},
        .timestamp = {},
    };

    settings.sleep_settings = {
        .flags = {0x3},
        .handheld_sleep_plan = HandheldSleepPlan::Sleep10Min,
        .console_sleep_plan = ConsoleSleepPlan::Sleep1Hour,
    };

    settings.device_time_zone_location_name = Service::PSC::Time::LocationName{"UTC"};
    settings.user_system_clock_automatic_correction_enabled = true;

    settings.primary_album_storage = PrimaryAlbumStorage::SdCard;
    settings.battery_percentage_flag = true;
    settings.chinese_traditional_input_method = ChineseTraditionalInputMethod::Unknown0;
    settings.vibration_master_volume = 1.0f;
    settings.touch_screen_mode = TouchScreenMode::Standard;
    settings.nfc_enable_flag = true;
    settings.bluetooth_enable_flag = true;
    settings.wireless_lan_enable_flag = true;

    const auto language_code = available_language_codes[static_cast<s32>(osSettings.language_index)];
    const auto key_code = std::find_if(language_to_layout.cbegin(), language_to_layout.cend(), [=](const auto & element) { return element.first == language_code; });

    settings.keyboard_layout = KeyboardLayout::EnglishUs;
    if (key_code != language_to_layout.end())
    {
        settings.keyboard_layout = key_code->second;
    }

    return settings;
}

} // namespace Service::Set
