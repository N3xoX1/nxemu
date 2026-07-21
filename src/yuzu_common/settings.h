// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "yuzu_common/common_types.h"
#include "yuzu_common/settings_common.h"
#include "yuzu_common/settings_setting.h"

namespace Settings {

const char* TranslateCategory(Settings::Category category);

#ifndef CANNOT_EXPLICITLY_INSTANTIATE
#define SETTING(TYPE, RANGED) extern template class Setting<TYPE, RANGED>
#define SWITCHABLE(TYPE, RANGED) extern template class SwitchableSetting<TYPE, RANGED>

SETTING(bool, false);
SETTING(int, false);
SETTING(s32, false);
SETTING(std::string, false);
SETTING(u16, false);
SWITCHABLE(bool, false);
SWITCHABLE(int, false);
SWITCHABLE(int, true);
SWITCHABLE(s64, false);
SWITCHABLE(u16, true);
SWITCHABLE(u32, false);
SWITCHABLE(u8, false);
SWITCHABLE(u8, true);

#undef SETTING
#undef SWITCHABLE
#endif

template <typename Type>
class InputSetting final {
public:
    InputSetting() = default;
    explicit InputSetting(Type val) : Setting<Type>(val) {}
    ~InputSetting() = default;
    void SetGlobal(bool to_global) {
        use_global = to_global;
    }
    [[nodiscard]] bool UsingGlobal() const {
        return use_global;
    }
    [[nodiscard]] Type& GetValue(bool need_global = false) {
        if (use_global || need_global) {
            return global;
        }
        return custom;
    }

private:
    bool use_global{true};
    Type global{};
    Type custom{};
};

struct TouchFromButtonMap {
    std::string name;
    std::vector<std::string> buttons;
};

struct Values {
    Linkage linkage{};

    // Data Storage
    Setting<bool> use_virtual_sd{linkage, true, "use_virtual_sd", Category::DataStorage};
    Setting<bool> gamecard_inserted{linkage, false, "gamecard_inserted", Category::DataStorage};
    Setting<bool> gamecard_current_game{linkage, false, "gamecard_current_game",
                                        Category::DataStorage};
    Setting<std::string> gamecard_path{linkage, std::string(), "gamecard_path",
                                       Category::DataStorage};

    // Debugging
    bool record_frame_times;
    Setting<bool> use_gdbstub{linkage, false, "use_gdbstub", Category::Debugging};
    Setting<u16> gdbstub_port{linkage, 6543, "gdbstub_port", Category::Debugging};
    Setting<std::string> program_args{linkage, std::string(), "program_args", Category::Debugging};
    Setting<bool> dump_exefs{linkage, false, "dump_exefs", Category::Debugging};
    Setting<bool> dump_nso{linkage, false, "dump_nso", Category::Debugging};
    Setting<bool> dump_shaders{
        linkage, false, "dump_shaders", Category::DebuggingGraphics, Specialization::Default,
        false};
    Setting<bool> dump_macros{
        linkage, false, "dump_macros", Category::DebuggingGraphics, Specialization::Default, false};
    Setting<bool> enable_fs_access_log{linkage, false, "enable_fs_access_log", Category::Debugging};
    Setting<bool> reporting_services{
        linkage, false, "reporting_services", Category::Debugging, Specialization::Default, false};
    Setting<bool> quest_flag{linkage, false, "quest_flag", Category::Debugging};
    Setting<bool> disable_macro_jit{linkage, false, "disable_macro_jit",
                                    Category::DebuggingGraphics};
    Setting<bool> disable_macro_hle{linkage, false, "disable_macro_hle",
                                    Category::DebuggingGraphics};
    Setting<bool> extended_logging{
        linkage, false, "extended_logging", Category::Debugging, Specialization::Default, false};
    Setting<bool> use_debug_asserts{linkage, false, "use_debug_asserts", Category::Debugging};
    Setting<bool> use_auto_stub{
        linkage, false, "use_auto_stub", Category::Debugging, Specialization::Default, false};
    Setting<bool> enable_all_controllers{linkage, false, "enable_all_controllers",
                                         Category::Debugging};
    Setting<bool> perform_vulkan_check{linkage, true, "perform_vulkan_check", Category::Debugging};

    // Network
    Setting<std::string> network_interface{linkage, std::string(), "network_interface",
                                           Category::Network};

    // WebService
    Setting<std::string> web_api_url{linkage, "https://api.yuzu-emu.org", "web_api_url",
                                     Category::WebService};
    Setting<std::string> yuzu_username{linkage, std::string(), "yuzu_username",
                                       Category::WebService};
    Setting<std::string> yuzu_token{linkage, std::string(), "yuzu_token", Category::WebService};
};

extern Values values;

float Volume();

void RestoreGlobalState(bool is_powered_on);

bool IsConfiguringGlobal();
void SetConfiguringGlobal(bool is_global);

} // namespace Settings
