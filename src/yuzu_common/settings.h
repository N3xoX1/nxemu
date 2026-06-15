// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "yuzu_common/common_types.h"
#include "yuzu_common/settings_common.h"
#include "yuzu_common/settings_enums.h"
#include "yuzu_common/settings_input.h"
#include "yuzu_common/settings_setting.h"

namespace Settings {

const char* TranslateCategory(Settings::Category category);

#ifndef CANNOT_EXPLICITLY_INSTANTIATE
// Instantiate the classes elsewhere (settings.cpp) to reduce compiler/linker work
#define SETTING(TYPE, RANGED) extern template class Setting<TYPE, RANGED>
#define SWITCHABLE(TYPE, RANGED) extern template class SwitchableSetting<TYPE, RANGED>

SETTING(AudioEngine, false);
SETTING(bool, false);
SETTING(int, false);
SETTING(s32, false);
SETTING(std::string, false);
SETTING(std::string, false);
SETTING(u16, false);
SWITCHABLE(AudioMode, true);
SWITCHABLE(CpuBackend, true);
SWITCHABLE(CpuAccuracy, true);
SWITCHABLE(Language, true);
SWITCHABLE(MemoryLayout, true);
SWITCHABLE(Region, true);
SWITCHABLE(TimeZone, true);
SWITCHABLE(bool, false);
SWITCHABLE(int, false);
SWITCHABLE(int, true);
SWITCHABLE(s64, false);
SWITCHABLE(u16, true);
SWITCHABLE(u32, false);
SWITCHABLE(u8, false);
SWITCHABLE(u8, true);

// Used in UISettings
// TODO see if we can move this to uisettings.h
SWITCHABLE(ConfirmStop, true);

#undef SETTING
#undef SWITCHABLE
#endif

/**
 * The InputSetting class allows for getting a reference to either the global or custom members.
 * This is required as we cannot easily modify the values of user-defined types within containers
 * using the SetValue() member function found in the Setting class. The primary purpose of this
 * class is to store an array of 10 PlayerInput structs for both the global and custom setting and
 * allows for easily accessing and modifying both settings.
 */
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
    bool use_global{true}; ///< The setting's global state
    Type global{};         ///< The setting
    Type custom{};         ///< The custom setting value
};

struct TouchFromButtonMap {
    std::string name;
    std::vector<std::string> buttons;
};

struct Values {
    Linkage linkage{};

    // Core
    SwitchableSetting<bool> use_multi_core{linkage, true, "use_multi_core", Category::Core};
    SwitchableSetting<MemoryLayout, true> memory_layout_mode{linkage,
                                                             MemoryLayout::Memory_4Gb,
                                                             MemoryLayout::Memory_4Gb,
                                                             MemoryLayout::Memory_8Gb,
                                                             "memory_layout_mode",
                                                             Category::Core};
    SwitchableSetting<bool> use_speed_limit{
        linkage, true, "use_speed_limit", Category::Core, Specialization::Paired, false, true};
    SwitchableSetting<u16, true> speed_limit{linkage,
                                             100,
                                             0,
                                             9999,
                                             "speed_limit",
                                             Category::Core,
                                             Specialization::Countable | Specialization::Percentage,
                                             true,
                                             true,
                                             &use_speed_limit};

    // Cpu
    SwitchableSetting<CpuBackend, true> cpu_backend{linkage,
#ifdef HAS_NCE
                                                    CpuBackend::Nce,
#else
                                                    CpuBackend::Dynarmic,
#endif
                                                    CpuBackend::Dynarmic,
#ifdef HAS_NCE
                                                    CpuBackend::Nce,
#else
                                                    CpuBackend::Dynarmic,
#endif
                                                    "cpu_backend",
                                                    Category::Cpu};
    SwitchableSetting<CpuAccuracy, true> cpu_accuracy{linkage,           CpuAccuracy::Auto,
                                                      CpuAccuracy::Auto, CpuAccuracy::Paranoid,
                                                      "cpu_accuracy",    Category::Cpu};
    SwitchableSetting<bool> cpu_debug_mode{linkage, false, "cpu_debug_mode", Category::CpuDebug};

    Setting<bool> cpuopt_page_tables{linkage, true, "cpuopt_page_tables", Category::CpuDebug};
    Setting<bool> cpuopt_block_linking{linkage, true, "cpuopt_block_linking", Category::CpuDebug};
    Setting<bool> cpuopt_return_stack_buffer{linkage, true, "cpuopt_return_stack_buffer",
                                             Category::CpuDebug};
    Setting<bool> cpuopt_fast_dispatcher{linkage, true, "cpuopt_fast_dispatcher",
                                         Category::CpuDebug};
    Setting<bool> cpuopt_context_elimination{linkage, true, "cpuopt_context_elimination",
                                             Category::CpuDebug};
    Setting<bool> cpuopt_const_prop{linkage, true, "cpuopt_const_prop", Category::CpuDebug};
    Setting<bool> cpuopt_misc_ir{linkage, true, "cpuopt_misc_ir", Category::CpuDebug};
    Setting<bool> cpuopt_reduce_misalign_checks{linkage, true, "cpuopt_reduce_misalign_checks",
                                                Category::CpuDebug};
    SwitchableSetting<bool> cpuopt_fastmem{linkage, true, "cpuopt_fastmem", Category::CpuDebug};
    SwitchableSetting<bool> cpuopt_fastmem_exclusives{linkage, true, "cpuopt_fastmem_exclusives",
                                                      Category::CpuDebug};
    Setting<bool> cpuopt_recompile_exclusives{linkage, true, "cpuopt_recompile_exclusives",
                                              Category::CpuDebug};
    Setting<bool> cpuopt_ignore_memory_aborts{linkage, true, "cpuopt_ignore_memory_aborts",
                                              Category::CpuDebug};

    SwitchableSetting<bool> cpuopt_unsafe_unfuse_fma{linkage, true, "cpuopt_unsafe_unfuse_fma",
                                                     Category::CpuUnsafe};
    SwitchableSetting<bool> cpuopt_unsafe_reduce_fp_error{
        linkage, true, "cpuopt_unsafe_reduce_fp_error", Category::CpuUnsafe};
    SwitchableSetting<bool> cpuopt_unsafe_ignore_standard_fpcr{
        linkage, true, "cpuopt_unsafe_ignore_standard_fpcr", Category::CpuUnsafe};
    SwitchableSetting<bool> cpuopt_unsafe_inaccurate_nan{
        linkage, true, "cpuopt_unsafe_inaccurate_nan", Category::CpuUnsafe};
    SwitchableSetting<bool> cpuopt_unsafe_fastmem_check{
        linkage, true, "cpuopt_unsafe_fastmem_check", Category::CpuUnsafe};
    SwitchableSetting<bool> cpuopt_unsafe_ignore_global_monitor{
        linkage, true, "cpuopt_unsafe_ignore_global_monitor", Category::CpuUnsafe};

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
    Setting<bool> enable_telemetry{linkage, true, "enable_telemetry", Category::WebService};
    Setting<std::string> web_api_url{linkage, "https://api.yuzu-emu.org", "web_api_url",
                                     Category::WebService};
    Setting<std::string> yuzu_username{linkage, std::string(), "yuzu_username",
                                       Category::WebService};
    Setting<std::string> yuzu_token{linkage, std::string(), "yuzu_token", Category::WebService};

    // Add-Ons
    std::map<u64, std::vector<std::string>> disabled_addons;
};

extern Values values;

bool IsFastmemEnabled();
void SetNceEnabled(bool is_64bit);
bool IsNceEnabled();

float Volume();

std::string GetTimeZoneString(TimeZone time_zone);

void TranslateResolutionInfo(ResolutionSetup setup, ResolutionScalingInfo& info);
void UpdateRescalingInfo();

// Restore the global state of all applicable settings in the Values struct
void RestoreGlobalState(bool is_powered_on);

bool IsConfiguringGlobal();
void SetConfiguringGlobal(bool is_global);

} // namespace Settings
