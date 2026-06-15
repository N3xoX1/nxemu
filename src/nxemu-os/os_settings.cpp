#include "os_settings.h"
#include "os_settings_identifiers.h"
#include <common/json_util.h>
#include <common/std_string.h>
#include <nxemu-module-spec/base.h>
#include <yuzu_common/settings_enums.h>
#include <yuzu_common/settings.h>
#include <yuzu_common/yuzu_assert.h>

extern IModuleSettings * g_settings;

OSSettings osSettings = {};

namespace
{
    enum class SettingType
    {
        StringSetting,
        StringValue,
        AudioEngine,
        AudioMode,
        Language,
        DockedMode,
        U8,
        U16,
        BooleanSwitchable,
        BooleanSetting,
        BooleanValue,
        UnsignedInt,
        Float,
        ControllerType,
        S32Setting,
        U32Switchable,
        S64Switchable
    };

    class OsSetting
    {
    public:
        OsSetting(const char * id, const char * path, Settings::SwitchableSetting<std::string> * val);
        OsSetting(const char * id, const char * path, Settings::SwitchableSetting<Settings::AudioEngine> * val);
        OsSetting(const char * id, const char * path, Settings::SwitchableSetting<Settings::AudioMode, true>* val);
        OsSetting(const char * id, const char * path, Settings::SwitchableSetting<Settings::Language, true> * val);
        OsSetting(const char * id, const char * path, Settings::SwitchableSetting<Settings::DockedMode> * val);
        OsSetting(const char * id, const char * path, Settings::SwitchableSetting<u8, true> * val);
        OsSetting(const char * id, const char * path, Settings::SwitchableSetting<u16, true> * val);
        OsSetting(const char * id, const char * path, Settings::SwitchableSetting<bool> * val);
        OsSetting(const char * id, const char * path, Settings::Setting<bool, false> * val);        
        OsSetting(const char * id, const char * path, bool * val, bool defaultValue);
        OsSetting(const char * id, const char * path, int * val, int defaultValue);
        OsSetting(const char * id, const char * path, float * val, float defaultValue);
        OsSetting(const char * id, const char * path, uint32_t * val, uint32_t defaultValue);
        OsSetting(const char * id, const char * path, std::string * val, const char * defaultValue);
        OsSetting(const char * id, const char * path, InputSettings::ControllerType * val, InputSettings::ControllerType defaultValue);
        OsSetting(const char * id, const char * path, Settings::Setting<s32> * val);
        OsSetting(const char * id, const char * path, Settings::SwitchableSetting<u32> * val);
        OsSetting(const char * id, const char * path, Settings::SwitchableSetting<s64, true> * val);

        const char * identifier;
        const char * json_path;
        SettingType settingType;
        union
        {
            Settings::SwitchableSetting<std::string> * stringSetting;
            Settings::SwitchableSetting<Settings::AudioEngine> * audioEngine;
            Settings::SwitchableSetting<Settings::AudioMode, true> * audioMode;
            Settings::SwitchableSetting<Settings::Language, true> * languageIndex;
            Settings::SwitchableSetting<Settings::DockedMode> * dockedMode;
            Settings::SwitchableSetting<u8, true> * u8;
            Settings::SwitchableSetting<u16, true> * u16;
            Settings::SwitchableSetting<bool> * booleanSwitchable;
            Settings::Setting<bool, false> * booleanSetting;
            bool * boolValue;
            uint32_t * uint32Value;
            float * floatValue;
            std::string * stringValue;
            InputSettings::ControllerType * controllerType;
            Settings::Setting<s32> * s32Setting;
            Settings::SwitchableSetting<u32> * u32Switchable;
            Settings::SwitchableSetting<s64, true> * s64Switchable;
        } setting;
        union
        {
            bool boolValue;
            uint32_t uint32Value;
            float floatValue;
            const char * sringValue;
            InputSettings::ControllerType controllerType;
        } defaultValue;
    };

    static OsSetting settings[] = {
        { nullptr, "controller\\player_0\\Connected", &osSettings.players.GetValue(true)[0].connected, true },
        { nullptr, "controller\\player_0\\ControllerType", &osSettings.players.GetValue(true)[0].controller_type, InputSettings::ControllerType::ProController },
        { nullptr, "controller\\player_0\\Button\\A", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::A], "engine:keyboard,code:67,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\B", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::B], "engine:keyboard,code:88,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\X", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::X], "engine:keyboard,code:86,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\Y", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::Y], "engine:keyboard,code:90,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\LStick", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::LStick], "engine:keyboard,code:70,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\RStick", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::RStick], "engine:keyboard,code:71,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\L", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::L], "engine:keyboard,code:81,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\R", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::R], "engine:keyboard,code:69,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\ZL", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::ZL], "engine:keyboard,code:82,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\ZR", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::ZR], "engine:keyboard,code:84,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\Plus", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::Plus], "engine:keyboard,code:77,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\Minus", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::Minus], "engine:keyboard,code:78,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\DLeft", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::DLeft], "engine:keyboard,code:37,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\DUp", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::DUp], "engine:keyboard,code:38,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\DRight", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::DRight], "engine:keyboard,code:39,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\DDown", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::DDown], "engine:keyboard,code:40,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\SLLeft", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::SLLeft], "engine:keyboard,code:81,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\SRLeft", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::SRLeft], "engine:keyboard,code:69,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\Home", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::Home], "engine:keyboard,code:0,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\Screenshot", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::Screenshot], "engine:keyboard,code:0,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\SLRight", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::SLRight], "engine:keyboard,code:81,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\SRRight", &osSettings.players.GetValue(true)[0].buttons[(size_t)NativeButtonValues::SRRight], "engine:keyboard,code:69,toggle:0" },
        { nullptr, "controller\\player_0\\Analog\\LStick", &osSettings.players.GetValue(true)[0].analogs[(size_t)NativeAnalogValues::LStick], "engine:analog_from_button,up:engine$0keyboard$1code$087$1toggle$00,left:engine$0keyboard$1code$065$1toggle$00,modifier:engine$0keyboard$1code$016$1toggle$00,down:engine$0keyboard$1code$083$1toggle$00,right:engine$0keyboard$1code$068$1toggle$00" },
        { nullptr, "controller\\player_0\\Analog\\RStick", &osSettings.players.GetValue(true)[0].analogs[(size_t)NativeAnalogValues::RStick], "engine:analog_from_button,up:engine$0keyboard$1code$073$1toggle$00,left:engine$0keyboard$1code$074$1toggle$00,modifier:engine$0keyboard$1code$00$1toggle$00,down:engine$0keyboard$1code$075$1toggle$00,right:engine$0keyboard$1code$076$1toggle$00" },
        { nullptr, "controller\\player_0\\Motion\\Left", &osSettings.players.GetValue(true)[0].motions[(size_t)NativeMotionValues::MotionLeft], "engine:keyboard,code:55,toggle:0" },
        { nullptr, "controller\\player_0\\Motion\\Right", &osSettings.players.GetValue(true)[0].motions[(size_t)NativeMotionValues::MotionRight], "engine:keyboard,code:56,toggle:0" },
        { nullptr, "controller\\player_0\\Vibration\\Enabled", &osSettings.players.GetValue(true)[0].vibration_enabled, false },
        { nullptr, "controller\\player_0\\Vibration\\Strength", &osSettings.players.GetValue(true)[0].vibration_strength, 0 },
        { nullptr, "controller\\player_0\\BodyColor\\Left", &osSettings.players.GetValue(true)[0].body_color_left, 0 },
        { nullptr, "controller\\player_0\\BodyColor\\Right", &osSettings.players.GetValue(true)[0].body_color_right, 0 },
        { nullptr, "controller\\player_0\\ButtonColor\\Left", &osSettings.players.GetValue(true)[0].button_color_left, 0 },
        { nullptr, "controller\\player_0\\ButtonColor\\Right", &osSettings.players.GetValue(true)[0].button_color_right, 0 },
        { nullptr, "controller\\player_0\\ProfileName", &osSettings.players.GetValue(true)[0].profile_name, "" },
        { nullptr, "controller\\player_0\\Vibration\\UseSystem", &osSettings.players.GetValue(true)[0].use_system_vibrator, false },

        { nullptr, "controller\\player_1\\Connected", &osSettings.players.GetValue(true)[1].connected, false },
        { nullptr, "controller\\player_1\\ControllerType", &osSettings.players.GetValue(true)[1].controller_type, InputSettings::ControllerType::ProController },
        { nullptr, "controller\\player_1\\Button\\A", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::A], "" },
        { nullptr, "controller\\player_1\\Button\\B", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::B], "" },
        { nullptr, "controller\\player_1\\Button\\X", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::X], "" },
        { nullptr, "controller\\player_1\\Button\\Y", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::Y], "" },
        { nullptr, "controller\\player_1\\Button\\LStick", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::LStick], "" },
        { nullptr, "controller\\player_1\\Button\\RStick", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::RStick], "" },
        { nullptr, "controller\\player_1\\Button\\L", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::L], "" },
        { nullptr, "controller\\player_1\\Button\\R", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::R], "" },
        { nullptr, "controller\\player_1\\Button\\ZL", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::ZL], "" },
        { nullptr, "controller\\player_1\\Button\\ZR", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::ZR], "" },
        { nullptr, "controller\\player_1\\Button\\Plus", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::Plus], "" },
        { nullptr, "controller\\player_1\\Button\\Minus", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::Minus], "" },
        { nullptr, "controller\\player_1\\Button\\DLeft", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::DLeft], "" },
        { nullptr, "controller\\player_1\\Button\\DUp", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::DUp], "" },
        { nullptr, "controller\\player_1\\Button\\DRight", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::DRight], "" },
        { nullptr, "controller\\player_1\\Button\\DDown", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::DDown], "" },
        { nullptr, "controller\\player_1\\Button\\SLLeft", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::SLLeft], "" },
        { nullptr, "controller\\player_1\\Button\\SRLeft", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::SRLeft], "" },
        { nullptr, "controller\\player_1\\Button\\Home", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::Home], "" },
        { nullptr, "controller\\player_1\\Button\\Screenshot", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::Screenshot], "" },
        { nullptr, "controller\\player_1\\Button\\SLRight", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::SLRight], "" },
        { nullptr, "controller\\player_1\\Button\\SRRight", &osSettings.players.GetValue(true)[1].buttons[(size_t)NativeButtonValues::SRRight], "" },
        { nullptr, "controller\\player_1\\Analog\\LStick", &osSettings.players.GetValue(true)[1].analogs[(size_t)NativeAnalogValues::LStick], "" },
        { nullptr, "controller\\player_1\\Analog\\RStick", &osSettings.players.GetValue(true)[1].analogs[(size_t)NativeAnalogValues::RStick], "" },
        { nullptr, "controller\\player_1\\Motion\\Left", &osSettings.players.GetValue(true)[1].motions[(size_t)NativeMotionValues::MotionLeft], "" },
        { nullptr, "controller\\player_1\\Motion\\Right", &osSettings.players.GetValue(true)[1].motions[(size_t)NativeMotionValues::MotionRight], "" },
        { nullptr, "controller\\player_1\\Vibration\\Enabled", &osSettings.players.GetValue(true)[1].vibration_enabled, false },
        { nullptr, "controller\\player_1\\Vibration\\Strength", &osSettings.players.GetValue(true)[1].vibration_strength, 0 },
        { nullptr, "controller\\player_1\\BodyColor\\Left", &osSettings.players.GetValue(true)[1].body_color_left, 0 },
        { nullptr, "controller\\player_1\\BodyColor\\Right", &osSettings.players.GetValue(true)[1].body_color_right, 0 },
        { nullptr, "controller\\player_1\\ButtonColor\\Left", &osSettings.players.GetValue(true)[1].button_color_left, 0 },
        { nullptr, "controller\\player_1\\ButtonColor\\Right", &osSettings.players.GetValue(true)[1].button_color_right, 0 },
        { nullptr, "controller\\player_1\\ProfileName", &osSettings.players.GetValue(true)[1].profile_name, "" },
        { nullptr, "controller\\player_1\\Vibration\\UseSystem", &osSettings.players.GetValue(true)[1].use_system_vibrator, false },

        {nullptr, "controller\\player_2\\Connected", &osSettings.players.GetValue(true)[2].connected, false},
        {nullptr, "controller\\player_2\\ControllerType", &osSettings.players.GetValue(true)[2].controller_type, InputSettings::ControllerType::ProController},
        {nullptr, "controller\\player_2\\Button\\A", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::A], ""},
        {nullptr, "controller\\player_2\\Button\\B", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::B], ""},
        {nullptr, "controller\\player_2\\Button\\X", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::X], ""},
        {nullptr, "controller\\player_2\\Button\\Y", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::Y], ""},
        {nullptr, "controller\\player_2\\Button\\LStick", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::LStick], ""},
        {nullptr, "controller\\player_2\\Button\\RStick", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::RStick], ""},
        {nullptr, "controller\\player_2\\Button\\L", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::L], ""},
        {nullptr, "controller\\player_2\\Button\\R", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::R], ""},
        {nullptr, "controller\\player_2\\Button\\ZL", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::ZL], ""},
        {nullptr, "controller\\player_2\\Button\\ZR", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::ZR], ""},
        {nullptr, "controller\\player_2\\Button\\Plus", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::Plus], ""},
        {nullptr, "controller\\player_2\\Button\\Minus", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::Minus], ""},
        {nullptr, "controller\\player_2\\Button\\DLeft", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::DLeft], ""},
        {nullptr, "controller\\player_2\\Button\\DUp", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::DUp], ""},
        {nullptr, "controller\\player_2\\Button\\DRight", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::DRight], ""},
        {nullptr, "controller\\player_2\\Button\\DDown", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::DDown], ""},
        {nullptr, "controller\\player_2\\Button\\SLLeft", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::SLLeft], ""},
        {nullptr, "controller\\player_2\\Button\\SRLeft", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::SRLeft], ""},
        {nullptr, "controller\\player_2\\Button\\Home", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::Home], ""},
        {nullptr, "controller\\player_2\\Button\\Screenshot", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::Screenshot], ""},
        {nullptr, "controller\\player_2\\Button\\SLRight", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::SLRight], ""},
        {nullptr, "controller\\player_2\\Button\\SRRight", &osSettings.players.GetValue(true)[2].buttons[(size_t)NativeButtonValues::SRRight], ""},
        {nullptr, "controller\\player_2\\Analog\\LStick", &osSettings.players.GetValue(true)[2].analogs[(size_t)NativeAnalogValues::LStick], ""},
        {nullptr, "controller\\player_2\\Analog\\RStick", &osSettings.players.GetValue(true)[2].analogs[(size_t)NativeAnalogValues::RStick], ""},
        {nullptr, "controller\\player_2\\Motion\\Left", &osSettings.players.GetValue(true)[2].motions[(size_t)NativeMotionValues::MotionLeft], ""},
        {nullptr, "controller\\player_2\\Motion\\Right", &osSettings.players.GetValue(true)[2].motions[(size_t)NativeMotionValues::MotionRight], ""},
        {nullptr, "controller\\player_2\\Vibration\\Enabled", &osSettings.players.GetValue(true)[2].vibration_enabled, false},
        {nullptr, "controller\\player_2\\Vibration\\Strength", &osSettings.players.GetValue(true)[2].vibration_strength, 0},
        {nullptr, "controller\\player_2\\BodyColor\\Left", &osSettings.players.GetValue(true)[2].body_color_left, 0},
        {nullptr, "controller\\player_2\\BodyColor\\Right", &osSettings.players.GetValue(true)[2].body_color_right, 0},
        {nullptr, "controller\\player_2\\ButtonColor\\Left", &osSettings.players.GetValue(true)[2].button_color_left, 0},
        {nullptr, "controller\\player_2\\ButtonColor\\Right", &osSettings.players.GetValue(true)[2].button_color_right, 0},
        {nullptr, "controller\\player_2\\ProfileName", &osSettings.players.GetValue(true)[2].profile_name, ""},
        {nullptr, "controller\\player_2\\Vibration\\UseSystem", &osSettings.players.GetValue(true)[2].use_system_vibrator, false},

        {nullptr, "controller\\player_3\\Connected", &osSettings.players.GetValue(true)[3].connected, false},
        {nullptr, "controller\\player_3\\ControllerType", &osSettings.players.GetValue(true)[3].controller_type, InputSettings::ControllerType::ProController},
        {nullptr, "controller\\player_3\\Button\\A", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::A], ""},
        {nullptr, "controller\\player_3\\Button\\B", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::B], ""},
        {nullptr, "controller\\player_3\\Button\\X", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::X], ""},
        {nullptr, "controller\\player_3\\Button\\Y", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::Y], ""},
        {nullptr, "controller\\player_3\\Button\\LStick", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::LStick], ""},
        {nullptr, "controller\\player_3\\Button\\RStick", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::RStick], ""},
        {nullptr, "controller\\player_3\\Button\\L", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::L], ""},
        {nullptr, "controller\\player_3\\Button\\R", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::R], ""},
        {nullptr, "controller\\player_3\\Button\\ZL", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::ZL], ""},
        {nullptr, "controller\\player_3\\Button\\ZR", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::ZR], ""},
        {nullptr, "controller\\player_3\\Button\\Plus", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::Plus], ""},
        {nullptr, "controller\\player_3\\Button\\Minus", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::Minus], ""},
        {nullptr, "controller\\player_3\\Button\\DLeft", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::DLeft], ""},
        {nullptr, "controller\\player_3\\Button\\DUp", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::DUp], ""},
        {nullptr, "controller\\player_3\\Button\\DRight", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::DRight], ""},
        {nullptr, "controller\\player_3\\Button\\DDown", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::DDown], ""},
        {nullptr, "controller\\player_3\\Button\\SLLeft", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::SLLeft], ""},
        {nullptr, "controller\\player_3\\Button\\SRLeft", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::SRLeft], ""},
        {nullptr, "controller\\player_3\\Button\\Home", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::Home], ""},
        {nullptr, "controller\\player_3\\Button\\Screenshot", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::Screenshot], ""},
        {nullptr, "controller\\player_3\\Button\\SLRight", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::SLRight], ""},
        {nullptr, "controller\\player_3\\Button\\SRRight", &osSettings.players.GetValue(true)[3].buttons[(size_t)NativeButtonValues::SRRight], ""},
        {nullptr, "controller\\player_3\\Analog\\LStick", &osSettings.players.GetValue(true)[3].analogs[(size_t)NativeAnalogValues::LStick], ""},
        {nullptr, "controller\\player_3\\Analog\\RStick", &osSettings.players.GetValue(true)[3].analogs[(size_t)NativeAnalogValues::RStick], ""},
        {nullptr, "controller\\player_3\\Motion\\Left", &osSettings.players.GetValue(true)[3].motions[(size_t)NativeMotionValues::MotionLeft], ""},
        {nullptr, "controller\\player_3\\Motion\\Right", &osSettings.players.GetValue(true)[3].motions[(size_t)NativeMotionValues::MotionRight], ""},
        {nullptr, "controller\\player_3\\Vibration\\Enabled", &osSettings.players.GetValue(true)[3].vibration_enabled, false},
        {nullptr, "controller\\player_3\\Vibration\\Strength", &osSettings.players.GetValue(true)[3].vibration_strength, 0},
        {nullptr, "controller\\player_3\\BodyColor\\Left", &osSettings.players.GetValue(true)[3].body_color_left, 0},
        {nullptr, "controller\\player_3\\BodyColor\\Right", &osSettings.players.GetValue(true)[3].body_color_right, 0},
        {nullptr, "controller\\player_3\\ButtonColor\\Left", &osSettings.players.GetValue(true)[3].button_color_left, 0},
        {nullptr, "controller\\player_3\\ButtonColor\\Right", &osSettings.players.GetValue(true)[3].button_color_right, 0},
        {nullptr, "controller\\player_3\\ProfileName", &osSettings.players.GetValue(true)[3].profile_name, ""},
        {nullptr, "controller\\player_3\\Vibration\\UseSystem", &osSettings.players.GetValue(true)[3].use_system_vibrator, false},

        {nullptr, "controller\\player_4\\Connected", &osSettings.players.GetValue(true)[4].connected, false},
        {nullptr, "controller\\player_4\\ControllerType", &osSettings.players.GetValue(true)[4].controller_type, InputSettings::ControllerType::ProController},
        {nullptr, "controller\\player_4\\Button\\A", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::A], ""},
        {nullptr, "controller\\player_4\\Button\\B", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::B], ""},
        {nullptr, "controller\\player_4\\Button\\X", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::X], ""},
        {nullptr, "controller\\player_4\\Button\\Y", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::Y], ""},
        {nullptr, "controller\\player_4\\Button\\LStick", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::LStick], ""},
        {nullptr, "controller\\player_4\\Button\\RStick", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::RStick], ""},
        {nullptr, "controller\\player_4\\Button\\L", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::L], ""},
        {nullptr, "controller\\player_4\\Button\\R", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::R], ""},
        {nullptr, "controller\\player_4\\Button\\ZL", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::ZL], ""},
        {nullptr, "controller\\player_4\\Button\\ZR", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::ZR], ""},
        {nullptr, "controller\\player_4\\Button\\Plus", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::Plus], ""},
        {nullptr, "controller\\player_4\\Button\\Minus", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::Minus], ""},
        {nullptr, "controller\\player_4\\Button\\DLeft", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::DLeft], ""},
        {nullptr, "controller\\player_4\\Button\\DUp", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::DUp], ""},
        {nullptr, "controller\\player_4\\Button\\DRight", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::DRight], ""},
        {nullptr, "controller\\player_4\\Button\\DDown", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::DDown], ""},
        {nullptr, "controller\\player_4\\Button\\SLLeft", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::SLLeft], ""},
        {nullptr, "controller\\player_4\\Button\\SRLeft", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::SRLeft], ""},
        {nullptr, "controller\\player_4\\Button\\Home", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::Home], ""},
        {nullptr, "controller\\player_4\\Button\\Screenshot", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::Screenshot], ""},
        {nullptr, "controller\\player_4\\Button\\SLRight", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::SLRight], ""},
        {nullptr, "controller\\player_4\\Button\\SRRight", &osSettings.players.GetValue(true)[4].buttons[(size_t)NativeButtonValues::SRRight], ""},
        {nullptr, "controller\\player_4\\Analog\\LStick", &osSettings.players.GetValue(true)[4].analogs[(size_t)NativeAnalogValues::LStick], ""},
        {nullptr, "controller\\player_4\\Analog\\RStick", &osSettings.players.GetValue(true)[4].analogs[(size_t)NativeAnalogValues::RStick], ""},
        {nullptr, "controller\\player_4\\Motion\\Left", &osSettings.players.GetValue(true)[4].motions[(size_t)NativeMotionValues::MotionLeft], ""},
        {nullptr, "controller\\player_4\\Motion\\Right", &osSettings.players.GetValue(true)[4].motions[(size_t)NativeMotionValues::MotionRight], ""},
        {nullptr, "controller\\player_4\\Vibration\\Enabled", &osSettings.players.GetValue(true)[4].vibration_enabled, false},
        {nullptr, "controller\\player_4\\Vibration\\Strength", &osSettings.players.GetValue(true)[4].vibration_strength, 0},
        {nullptr, "controller\\player_4\\BodyColor\\Left", &osSettings.players.GetValue(true)[4].body_color_left, 0},
        {nullptr, "controller\\player_4\\BodyColor\\Right", &osSettings.players.GetValue(true)[4].body_color_right, 0},
        {nullptr, "controller\\player_4\\ButtonColor\\Left", &osSettings.players.GetValue(true)[4].button_color_left, 0},
        {nullptr, "controller\\player_4\\ButtonColor\\Right", &osSettings.players.GetValue(true)[4].button_color_right, 0},
        {nullptr, "controller\\player_4\\ProfileName", &osSettings.players.GetValue(true)[4].profile_name, ""},
        {nullptr, "controller\\player_4\\Vibration\\UseSystem", &osSettings.players.GetValue(true)[4].use_system_vibrator, false},

        {nullptr, "controller\\player_5\\Connected", &osSettings.players.GetValue(true)[5].connected, false},
        {nullptr, "controller\\player_5\\ControllerType", &osSettings.players.GetValue(true)[5].controller_type, InputSettings::ControllerType::ProController},
        {nullptr, "controller\\player_5\\Button\\A", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::A], ""},
        {nullptr, "controller\\player_5\\Button\\B", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::B], ""},
        {nullptr, "controller\\player_5\\Button\\X", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::X], ""},
        {nullptr, "controller\\player_5\\Button\\Y", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::Y], ""},
        {nullptr, "controller\\player_5\\Button\\LStick", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::LStick], ""},
        {nullptr, "controller\\player_5\\Button\\RStick", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::RStick], ""},
        {nullptr, "controller\\player_5\\Button\\L", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::L], ""},
        {nullptr, "controller\\player_5\\Button\\R", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::R], ""},
        {nullptr, "controller\\player_5\\Button\\ZL", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::ZL], ""},
        {nullptr, "controller\\player_5\\Button\\ZR", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::ZR], ""},
        {nullptr, "controller\\player_5\\Button\\Plus", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::Plus], ""},
        {nullptr, "controller\\player_5\\Button\\Minus", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::Minus], ""},
        {nullptr, "controller\\player_5\\Button\\DLeft", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::DLeft], ""},
        {nullptr, "controller\\player_5\\Button\\DUp", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::DUp], ""},
        {nullptr, "controller\\player_5\\Button\\DRight", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::DRight], ""},
        {nullptr, "controller\\player_5\\Button\\DDown", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::DDown], ""},
        {nullptr, "controller\\player_5\\Button\\SLLeft", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::SLLeft], ""},
        {nullptr, "controller\\player_5\\Button\\SRLeft", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::SRLeft], ""},
        {nullptr, "controller\\player_5\\Button\\Home", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::Home], ""},
        {nullptr, "controller\\player_5\\Button\\Screenshot", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::Screenshot], ""},
        {nullptr, "controller\\player_5\\Button\\SLRight", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::SLRight], ""},
        {nullptr, "controller\\player_5\\Button\\SRRight", &osSettings.players.GetValue(true)[5].buttons[(size_t)NativeButtonValues::SRRight], ""},
        {nullptr, "controller\\player_5\\Analog\\LStick", &osSettings.players.GetValue(true)[5].analogs[(size_t)NativeAnalogValues::LStick], ""},
        {nullptr, "controller\\player_5\\Analog\\RStick", &osSettings.players.GetValue(true)[5].analogs[(size_t)NativeAnalogValues::RStick], ""},
        {nullptr, "controller\\player_5\\Motion\\Left", &osSettings.players.GetValue(true)[5].motions[(size_t)NativeMotionValues::MotionLeft], ""},
        {nullptr, "controller\\player_5\\Motion\\Right", &osSettings.players.GetValue(true)[5].motions[(size_t)NativeMotionValues::MotionRight], ""},
        {nullptr, "controller\\player_5\\Vibration\\Enabled", &osSettings.players.GetValue(true)[5].vibration_enabled, false},
        {nullptr, "controller\\player_5\\Vibration\\Strength", &osSettings.players.GetValue(true)[5].vibration_strength, 0},
        {nullptr, "controller\\player_5\\BodyColor\\Left", &osSettings.players.GetValue(true)[5].body_color_left, 0},
        {nullptr, "controller\\player_5\\BodyColor\\Right", &osSettings.players.GetValue(true)[5].body_color_right, 0},
        {nullptr, "controller\\player_5\\ButtonColor\\Left", &osSettings.players.GetValue(true)[5].button_color_left, 0},
        {nullptr, "controller\\player_5\\ButtonColor\\Right", &osSettings.players.GetValue(true)[5].button_color_right, 0},
        {nullptr, "controller\\player_5\\ProfileName", &osSettings.players.GetValue(true)[5].profile_name, ""},
        {nullptr, "controller\\player_5\\Vibration\\UseSystem", &osSettings.players.GetValue(true)[5].use_system_vibrator, false},

        {nullptr, "controller\\player_6\\Connected", &osSettings.players.GetValue(true)[6].connected, false},
        {nullptr, "controller\\player_6\\ControllerType", &osSettings.players.GetValue(true)[6].controller_type, InputSettings::ControllerType::ProController},
        {nullptr, "controller\\player_6\\Button\\A", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::A], ""},
        {nullptr, "controller\\player_6\\Button\\B", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::B], ""},
        {nullptr, "controller\\player_6\\Button\\X", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::X], ""},
        {nullptr, "controller\\player_6\\Button\\Y", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::Y], ""},
        {nullptr, "controller\\player_6\\Button\\LStick", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::LStick], ""},
        {nullptr, "controller\\player_6\\Button\\RStick", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::RStick], ""},
        {nullptr, "controller\\player_6\\Button\\L", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::L], ""},
        {nullptr, "controller\\player_6\\Button\\R", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::R], ""},
        {nullptr, "controller\\player_6\\Button\\ZL", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::ZL], ""},
        {nullptr, "controller\\player_6\\Button\\ZR", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::ZR], ""},
        {nullptr, "controller\\player_6\\Button\\Plus", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::Plus], ""},
        {nullptr, "controller\\player_6\\Button\\Minus", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::Minus], ""},
        {nullptr, "controller\\player_6\\Button\\DLeft", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::DLeft], ""},
        {nullptr, "controller\\player_6\\Button\\DUp", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::DUp], ""},
        {nullptr, "controller\\player_6\\Button\\DRight", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::DRight], ""},
        {nullptr, "controller\\player_6\\Button\\DDown", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::DDown], ""},
        {nullptr, "controller\\player_6\\Button\\SLLeft", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::SLLeft], ""},
        {nullptr, "controller\\player_6\\Button\\SRLeft", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::SRLeft], ""},
        {nullptr, "controller\\player_6\\Button\\Home", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::Home], ""},
        {nullptr, "controller\\player_6\\Button\\Screenshot", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::Screenshot], ""},
        {nullptr, "controller\\player_6\\Button\\SLRight", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::SLRight], ""},
        {nullptr, "controller\\player_6\\Button\\SRRight", &osSettings.players.GetValue(true)[6].buttons[(size_t)NativeButtonValues::SRRight], ""},
        {nullptr, "controller\\player_6\\Analog\\LStick", &osSettings.players.GetValue(true)[6].analogs[(size_t)NativeAnalogValues::LStick], ""},
        {nullptr, "controller\\player_6\\Analog\\RStick", &osSettings.players.GetValue(true)[6].analogs[(size_t)NativeAnalogValues::RStick], ""},
        {nullptr, "controller\\player_6\\Motion\\Left", &osSettings.players.GetValue(true)[6].motions[(size_t)NativeMotionValues::MotionLeft], ""},
        {nullptr, "controller\\player_6\\Motion\\Right", &osSettings.players.GetValue(true)[6].motions[(size_t)NativeMotionValues::MotionRight], ""},
        {nullptr, "controller\\player_6\\Vibration\\Enabled", &osSettings.players.GetValue(true)[6].vibration_enabled, false},
        {nullptr, "controller\\player_6\\Vibration\\Strength", &osSettings.players.GetValue(true)[6].vibration_strength, 0},
        {nullptr, "controller\\player_6\\BodyColor\\Left", &osSettings.players.GetValue(true)[6].body_color_left, 0},
        {nullptr, "controller\\player_6\\BodyColor\\Right", &osSettings.players.GetValue(true)[6].body_color_right, 0},
        {nullptr, "controller\\player_6\\ButtonColor\\Left", &osSettings.players.GetValue(true)[6].button_color_left, 0},
        {nullptr, "controller\\player_6\\ButtonColor\\Right", &osSettings.players.GetValue(true)[6].button_color_right, 0},
        {nullptr, "controller\\player_6\\ProfileName", &osSettings.players.GetValue(true)[6].profile_name, ""},
        {nullptr, "controller\\player_6\\Vibration\\UseSystem", &osSettings.players.GetValue(true)[6].use_system_vibrator, false},

        {nullptr, "controller\\player_7\\Connected", &osSettings.players.GetValue(true)[7].connected, false},
        {nullptr, "controller\\player_7\\ControllerType", &osSettings.players.GetValue(true)[7].controller_type, InputSettings::ControllerType::ProController},
        {nullptr, "controller\\player_7\\Button\\A", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::A], ""},
        {nullptr, "controller\\player_7\\Button\\B", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::B], ""},
        {nullptr, "controller\\player_7\\Button\\X", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::X], ""},
        {nullptr, "controller\\player_7\\Button\\Y", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::Y], ""},
        {nullptr, "controller\\player_7\\Button\\LStick", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::LStick], ""},
        {nullptr, "controller\\player_7\\Button\\RStick", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::RStick], ""},
        {nullptr, "controller\\player_7\\Button\\L", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::L], ""},
        {nullptr, "controller\\player_7\\Button\\R", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::R], ""},
        {nullptr, "controller\\player_7\\Button\\ZL", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::ZL], ""},
        {nullptr, "controller\\player_7\\Button\\ZR", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::ZR], ""},
        {nullptr, "controller\\player_7\\Button\\Plus", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::Plus], ""},
        {nullptr, "controller\\player_7\\Button\\Minus", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::Minus], ""},
        {nullptr, "controller\\player_7\\Button\\DLeft", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::DLeft], ""},
        {nullptr, "controller\\player_7\\Button\\DUp", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::DUp], ""},
        {nullptr, "controller\\player_7\\Button\\DRight", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::DRight], ""},
        {nullptr, "controller\\player_7\\Button\\DDown", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::DDown], ""},
        {nullptr, "controller\\player_7\\Button\\SLLeft", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::SLLeft], ""},
        {nullptr, "controller\\player_7\\Button\\SRLeft", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::SRLeft], ""},
        {nullptr, "controller\\player_7\\Button\\Home", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::Home], ""},
        {nullptr, "controller\\player_7\\Button\\Screenshot", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::Screenshot], ""},
        {nullptr, "controller\\player_7\\Button\\SLRight", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::SLRight], ""},
        {nullptr, "controller\\player_7\\Button\\SRRight", &osSettings.players.GetValue(true)[7].buttons[(size_t)NativeButtonValues::SRRight], ""},
        {nullptr, "controller\\player_7\\Analog\\LStick", &osSettings.players.GetValue(true)[7].analogs[(size_t)NativeAnalogValues::LStick], ""},
        {nullptr, "controller\\player_7\\Analog\\RStick", &osSettings.players.GetValue(true)[7].analogs[(size_t)NativeAnalogValues::RStick], ""},
        {nullptr, "controller\\player_7\\Motion\\Left", &osSettings.players.GetValue(true)[7].motions[(size_t)NativeMotionValues::MotionLeft], ""},
        {nullptr, "controller\\player_7\\Motion\\Right", &osSettings.players.GetValue(true)[7].motions[(size_t)NativeMotionValues::MotionRight], ""},
        {nullptr, "controller\\player_7\\Vibration\\Enabled", &osSettings.players.GetValue(true)[7].vibration_enabled, false},
        {nullptr, "controller\\player_7\\Vibration\\Strength", &osSettings.players.GetValue(true)[7].vibration_strength, 0},
        {nullptr, "controller\\player_7\\BodyColor\\Left", &osSettings.players.GetValue(true)[7].body_color_left, 0},
        {nullptr, "controller\\player_7\\BodyColor\\Right", &osSettings.players.GetValue(true)[7].body_color_right, 0},
        {nullptr, "controller\\player_7\\ButtonColor\\Left", &osSettings.players.GetValue(true)[7].button_color_left, 0},
        {nullptr, "controller\\player_7\\ButtonColor\\Right", &osSettings.players.GetValue(true)[7].button_color_right, 0},
        {nullptr, "controller\\player_7\\ProfileName", &osSettings.players.GetValue(true)[7].profile_name, ""},
        {nullptr, "controller\\player_7\\Vibration\\UseSystem", &osSettings.players.GetValue(true)[7].use_system_vibrator, false},

        {nullptr, "controller\\player_8\\Connected", &osSettings.players.GetValue(true)[8].connected, false},
        {nullptr, "controller\\player_8\\ControllerType", &osSettings.players.GetValue(true)[8].controller_type, InputSettings::ControllerType::ProController},
        {nullptr, "controller\\player_8\\Button\\A", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::A], ""},
        {nullptr, "controller\\player_8\\Button\\B", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::B], ""},
        {nullptr, "controller\\player_8\\Button\\X", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::X], ""},
        {nullptr, "controller\\player_8\\Button\\Y", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::Y], ""},
        {nullptr, "controller\\player_8\\Button\\LStick", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::LStick], ""},
        {nullptr, "controller\\player_8\\Button\\RStick", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::RStick], ""},
        {nullptr, "controller\\player_8\\Button\\L", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::L], ""},
        {nullptr, "controller\\player_8\\Button\\R", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::R], ""},
        {nullptr, "controller\\player_8\\Button\\ZL", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::ZL], ""},
        {nullptr, "controller\\player_8\\Button\\ZR", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::ZR], ""},
        {nullptr, "controller\\player_8\\Button\\Plus", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::Plus], ""},
        {nullptr, "controller\\player_8\\Button\\Minus", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::Minus], ""},
        {nullptr, "controller\\player_8\\Button\\DLeft", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::DLeft], ""},
        {nullptr, "controller\\player_8\\Button\\DUp", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::DUp], ""},
        {nullptr, "controller\\player_8\\Button\\DRight", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::DRight], ""},
        {nullptr, "controller\\player_8\\Button\\DDown", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::DDown], ""},
        {nullptr, "controller\\player_8\\Button\\SLLeft", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::SLLeft], ""},
        {nullptr, "controller\\player_8\\Button\\SRLeft", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::SRLeft], ""},
        {nullptr, "controller\\player_8\\Button\\Home", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::Home], ""},
        {nullptr, "controller\\player_8\\Button\\Screenshot", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::Screenshot], ""},
        {nullptr, "controller\\player_8\\Button\\SLRight", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::SLRight], ""},
        {nullptr, "controller\\player_8\\Button\\SRRight", &osSettings.players.GetValue(true)[8].buttons[(size_t)NativeButtonValues::SRRight], ""},
        {nullptr, "controller\\player_8\\Analog\\LStick", &osSettings.players.GetValue(true)[8].analogs[(size_t)NativeAnalogValues::LStick], ""},
        {nullptr, "controller\\player_8\\Analog\\RStick", &osSettings.players.GetValue(true)[8].analogs[(size_t)NativeAnalogValues::RStick], ""},
        {nullptr, "controller\\player_8\\Motion\\Left", &osSettings.players.GetValue(true)[8].motions[(size_t)NativeMotionValues::MotionLeft], ""},
        {nullptr, "controller\\player_8\\Motion\\Right", &osSettings.players.GetValue(true)[8].motions[(size_t)NativeMotionValues::MotionRight], ""},
        {nullptr, "controller\\player_8\\Vibration\\Enabled", &osSettings.players.GetValue(true)[8].vibration_enabled, false},
        {nullptr, "controller\\player_8\\Vibration\\Strength", &osSettings.players.GetValue(true)[8].vibration_strength, 0},
        {nullptr, "controller\\player_8\\BodyColor\\Left", &osSettings.players.GetValue(true)[8].body_color_left, 0},
        {nullptr, "controller\\player_8\\BodyColor\\Right", &osSettings.players.GetValue(true)[8].body_color_right, 0},
        {nullptr, "controller\\player_8\\ButtonColor\\Left", &osSettings.players.GetValue(true)[8].button_color_left, 0},
        {nullptr, "controller\\player_8\\ButtonColor\\Right", &osSettings.players.GetValue(true)[8].button_color_right, 0},
        {nullptr, "controller\\player_8\\ProfileName", &osSettings.players.GetValue(true)[8].profile_name, ""},
        {nullptr, "controller\\player_8\\Vibration\\UseSystem", &osSettings.players.GetValue(true)[8].use_system_vibrator, false},

        {nullptr, "controller\\player_9\\Connected", &osSettings.players.GetValue(true)[9].connected, false},
        {nullptr, "controller\\player_9\\ControllerType", &osSettings.players.GetValue(true)[9].controller_type, InputSettings::ControllerType::ProController},
        {nullptr, "controller\\player_9\\Button\\A", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::A], ""},
        {nullptr, "controller\\player_9\\Button\\B", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::B], ""},
        {nullptr, "controller\\player_9\\Button\\X", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::X], ""},
        {nullptr, "controller\\player_9\\Button\\Y", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::Y], ""},
        {nullptr, "controller\\player_9\\Button\\LStick", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::LStick], ""},
        {nullptr, "controller\\player_9\\Button\\RStick", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::RStick], ""},
        {nullptr, "controller\\player_9\\Button\\L", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::L], ""},
        {nullptr, "controller\\player_9\\Button\\R", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::R], ""},
        {nullptr, "controller\\player_9\\Button\\ZL", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::ZL], ""},
        {nullptr, "controller\\player_9\\Button\\ZR", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::ZR], ""},
        {nullptr, "controller\\player_9\\Button\\Plus", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::Plus], ""},
        {nullptr, "controller\\player_9\\Button\\Minus", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::Minus], ""},
        {nullptr, "controller\\player_9\\Button\\DLeft", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::DLeft], ""},
        {nullptr, "controller\\player_9\\Button\\DUp", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::DUp], ""},
        {nullptr, "controller\\player_9\\Button\\DRight", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::DRight], ""},
        {nullptr, "controller\\player_9\\Button\\DDown", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::DDown], ""},
        {nullptr, "controller\\player_9\\Button\\SLLeft", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::SLLeft], ""},
        {nullptr, "controller\\player_9\\Button\\SRLeft", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::SRLeft], ""},
        {nullptr, "controller\\player_9\\Button\\Home", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::Home], ""},
        {nullptr, "controller\\player_9\\Button\\Screenshot", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::Screenshot], ""},
        {nullptr, "controller\\player_9\\Button\\SLRight", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::SLRight], ""},
        {nullptr, "controller\\player_9\\Button\\SRRight", &osSettings.players.GetValue(true)[9].buttons[(size_t)NativeButtonValues::SRRight], ""},
        {nullptr, "controller\\player_9\\Analog\\LStick", &osSettings.players.GetValue(true)[9].analogs[(size_t)NativeAnalogValues::LStick], ""},
        {nullptr, "controller\\player_9\\Analog\\RStick", &osSettings.players.GetValue(true)[9].analogs[(size_t)NativeAnalogValues::RStick], ""},
        {nullptr, "controller\\player_9\\Motion\\Left", &osSettings.players.GetValue(true)[9].motions[(size_t)NativeMotionValues::MotionLeft], ""},
        {nullptr, "controller\\player_9\\Motion\\Right", &osSettings.players.GetValue(true)[9].motions[(size_t)NativeMotionValues::MotionRight], ""},
        {nullptr, "controller\\player_9\\Vibration\\Enabled", &osSettings.players.GetValue(true)[9].vibration_enabled, false},
        {nullptr, "controller\\player_9\\Vibration\\Strength", &osSettings.players.GetValue(true)[9].vibration_strength, 0},
        {nullptr, "controller\\player_9\\BodyColor\\Left", &osSettings.players.GetValue(true)[9].body_color_left, 0},
        {nullptr, "controller\\player_9\\BodyColor\\Right", &osSettings.players.GetValue(true)[9].body_color_right, 0},
        {nullptr, "controller\\player_9\\ButtonColor\\Left", &osSettings.players.GetValue(true)[9].button_color_left, 0},
        {nullptr, "controller\\player_9\\ButtonColor\\Right", &osSettings.players.GetValue(true)[9].button_color_right, 0},
        {nullptr, "controller\\player_9\\ProfileName", &osSettings.players.GetValue(true)[9].profile_name, ""},
        {nullptr, "controller\\player_9\\Vibration\\UseSystem", &osSettings.players.GetValue(true)[9].use_system_vibrator, false},

        {NXOsSetting::AudioSinkId, "audio\\sink_id", &osSettings.sink_id},
        {NXOsSetting::AudioOutputDeviceId, "audio\\output_device_id", &osSettings.audio_output_device_id},
        {NXOsSetting::AudioInputDeviceId, "audio\\input_device_id", &osSettings.audio_input_device_id},
        {NXOsSetting::AudioMode, "audio\\mode", &osSettings.sound_index},
        {NXOsSetting::AudioVolume, "audio\\volume", &osSettings.volume},
        {NXOsSetting::AudioMuted, "audio\\muted", &osSettings.audio_muted},
        {NXOsSetting::SpeedLimit, "system\\speed_limit", &Settings::values.speed_limit},
        {NXOsSetting::UseMultiCore, "system\\use_multi_core", &Settings::values.use_multi_core},
        {NXOsSetting::UseSpeedLimit, "system\\use_speed_limit", &Settings::values.use_speed_limit},
        {NXOsSetting::LanguageIndex, "system\\language_index", &osSettings.language_index},
        {NXOsSetting::CurrentUser, "system\\current_user", &osSettings.current_user},
        {NXOsSetting::RngSeedEnabled, "system\\rng_seed_enabled", &osSettings.rng_seed_enabled},
        {NXOsSetting::RngSeed, "system\\rng_seed", &osSettings.rng_seed},
        {NXOsSetting::CustomRtcEnabled, "system\\custom_rtc_enabled", &osSettings.custom_rtc_enabled},
        {NXOsSetting::CustomRtcOffset, "system\\custom_rtc_offset", &osSettings.custom_rtc_offset},
        {NXOsSetting::DockedMode, "system\\docked_mode", &osSettings.use_docked_mode},
    };

    template <typename T>
    std::vector<std::pair<std::string, T>> Canonicalizations();

    // Specialization for ControllerType
    template <>
    std::vector<std::pair<std::string, InputSettings::ControllerType>> Canonicalizations<InputSettings::ControllerType>()
    {
        return {
            {"ProController", InputSettings::ControllerType::ProController},
            {"DualJoyconDetached", InputSettings::ControllerType::DualJoyconDetached},
            {"LeftJoycon", InputSettings::ControllerType::LeftJoycon},
            {"RightJoycon", InputSettings::ControllerType::RightJoycon},
            {"Handheld", InputSettings::ControllerType::Handheld},
            {"GameCube", InputSettings::ControllerType::GameCube},
            {"Pokeball", InputSettings::ControllerType::Pokeball},
            {"NES", InputSettings::ControllerType::NES},
            {"SNES", InputSettings::ControllerType::SNES},
            {"N64", InputSettings::ControllerType::N64},
            {"SegaGenesis", InputSettings::ControllerType::SegaGenesis},
        };
    };

    template <typename T>
    std::string CanonicalizeEnum(T value)
    {
        using CanonicalItem = std::pair<std::string, T>;
        using CanonicalList = std::vector<CanonicalItem>;
        const CanonicalList group = Canonicalizations<T>();

        for (const CanonicalItem & item : group)
        {
            if (item.second == value)
            {
                return item.first;
            }
        }
        return "";
    }

    template <typename T>
    T ParseEnum(const std::string& canonicalization)
    {
        using CanonicalItem = std::pair<std::string, T>;
        using CanonicalList = std::vector<CanonicalItem>;
        const CanonicalList group = Canonicalizations<T>();

        for (const CanonicalItem& item : group)
        {
            if (item.first == canonicalization)
            {
                return item.second;
            }
        }
        return {};
    }
}

void OsSettingChanged(const char * setting, void * /*userData*/)
{
    for (const OsSetting & osSetting : settings)
    {
        if (osSetting.identifier == nullptr)
        {
            continue;
        }

        if (strcmp(osSetting.identifier, setting) != 0)
        {
            continue;
        }
        switch (osSetting.settingType)
        {
        case SettingType::StringSetting:
            osSetting.setting.stringSetting->SetValue(g_settings->GetString(setting));
            break;
        case SettingType::AudioEngine:
            osSetting.setting.audioEngine->SetValue((Settings::AudioEngine)g_settings->GetInt(setting));
            break;
        case SettingType::AudioMode:
            osSetting.setting.audioMode->SetValue((Settings::AudioMode)g_settings->GetInt(setting));
            break;
        case SettingType::Language:
            osSetting.setting.languageIndex->SetValue((Settings::Language)g_settings->GetInt(setting));
            break;
        case SettingType::DockedMode:
            osSetting.setting.dockedMode->SetValue((Settings::DockedMode)g_settings->GetInt(setting));
            break;
        case SettingType::U8:
            osSetting.setting.u8->SetValue(g_settings->GetInt(setting));
            break;
        case SettingType::BooleanSetting:
            osSetting.setting.booleanSetting->SetValue(g_settings->GetBool(setting));
            break;
        case SettingType::BooleanSwitchable:
            osSetting.setting.booleanSwitchable->SetValue(g_settings->GetBool(setting));
            break;
        case SettingType::BooleanValue:
            *osSetting.setting.boolValue = g_settings->GetBool(setting);
            break;
        case SettingType::Float:
            *osSetting.setting.floatValue = g_settings->GetFloat(setting);
            break;
        case SettingType::S32Setting:
            osSetting.setting.s32Setting->SetValue(g_settings->GetInt(setting));
            break;
        case SettingType::U32Switchable:
            osSetting.setting.u32Switchable->SetValue(static_cast<u32>(g_settings->GetInt(setting)));
            break;
        case SettingType::S64Switchable:
            osSetting.setting.s64Switchable->SetValue(static_cast<s64>(g_settings->GetInt(setting)));
            break;
        default:
            UNIMPLEMENTED();
        }
    }
}

void SetupOsSetting(void)
{
    for (const OsSetting & osSetting : settings)
    {
        switch (osSetting.settingType)
        {
        case SettingType::StringSetting:
            osSetting.setting.stringSetting->SetValue(osSetting.setting.stringSetting->GetDefault());
            break;
        case SettingType::AudioEngine:
            osSetting.setting.audioEngine->SetValue(osSetting.setting.audioEngine->GetDefault());
            break;
        case SettingType::AudioMode:
            osSetting.setting.audioMode->SetValue(osSetting.setting.audioMode->GetDefault());
            break;
        case SettingType::Language:
            osSetting.setting.languageIndex->SetValue(osSetting.setting.languageIndex->GetDefault());
            break;
        case SettingType::DockedMode:
            osSetting.setting.dockedMode->SetValue(osSetting.setting.dockedMode->GetDefault());
            break;
        case SettingType::U8:
            osSetting.setting.u8->SetValue(osSetting.setting.u8->GetDefault());
            break;
        case SettingType::U16:
            osSetting.setting.u16->SetValue(osSetting.setting.u16->GetDefault());
            break;
        case SettingType::BooleanSwitchable:
            osSetting.setting.booleanSwitchable->SetValue(osSetting.setting.booleanSwitchable->GetDefault());
            break;
        case SettingType::BooleanSetting:
            osSetting.setting.booleanSetting->SetValue(osSetting.setting.booleanSetting->GetDefault());
            break;
        case SettingType::BooleanValue:
            *osSetting.setting.boolValue = osSetting.defaultValue.boolValue;
            break;
        case SettingType::UnsignedInt:
            *osSetting.setting.uint32Value = osSetting.defaultValue.uint32Value;
            break;
        case SettingType::Float:
            *osSetting.setting.floatValue = osSetting.defaultValue.floatValue;
            break;
        case SettingType::StringValue:
            *osSetting.setting.stringValue = osSetting.defaultValue.sringValue;
            break;
        case SettingType::ControllerType:
            *osSetting.setting.controllerType = osSetting.defaultValue.controllerType;
            break;
        case SettingType::S32Setting:
            osSetting.setting.s32Setting->SetValue(osSetting.setting.s32Setting->GetDefault());
            break;
        case SettingType::U32Switchable:
            osSetting.setting.u32Switchable->SetValue(osSetting.setting.u32Switchable->GetDefault());
            break;
        case SettingType::S64Switchable:
            osSetting.setting.s64Switchable->SetValue(osSetting.setting.s64Switchable->GetDefault());
            break;
        default:
            UNIMPLEMENTED();
        }
    }

    JsonValue root;
    JsonReader reader;
    std::string json = g_settings->GetSectionSettings("nxemu-os");

    if (!json.empty() && reader.Parse(json.data(), json.data() + json.size(), root))
    {
        for (const OsSetting & osSetting : settings)
        {
            JsonValue value = JsonGetNestedValue(root, osSetting.json_path);
            switch (osSetting.settingType)
            {
            case SettingType::StringSetting:
                if (value.isString())
                {
                    osSetting.setting.stringSetting->SetValue(value.asString());
                }
                break;
            case SettingType::AudioEngine:
                if (value.isString())
                {
                    osSetting.setting.audioEngine->SetValue(Settings::ToEnum<Settings::AudioEngine>(value.asString()));
                }
                break;
            case SettingType::AudioMode:
                if (value.isString())
                {
                    osSetting.setting.audioMode->SetValue(Settings::ToEnum<Settings::AudioMode>(value.asString()));
                }
                break;
            case SettingType::Language:
                if (value.isString())
                {
                    osSetting.setting.languageIndex->SetValue(Settings::ToEnum<Settings::Language>(value.asString()));
                }
                break;
            case SettingType::DockedMode:
                if (value.isString())
                {
                    osSetting.setting.dockedMode->SetValue(Settings::ToEnum<Settings::DockedMode>(value.asString()));
                }
                break;
            case SettingType::U8:
                if (value.isInt())
                {
                    osSetting.setting.u8->SetValue((uint8_t)value.asInt64());
                }
                break;
            case SettingType::U16:
                if (value.isInt())
                {
                    osSetting.setting.u16->SetValue((uint16_t)value.asInt64());
                }
                break;
            case SettingType::BooleanSwitchable:
                if (value.isBool())
                {
                    osSetting.setting.booleanSwitchable->SetValue(value.asBool());
                }
                break;
            case SettingType::BooleanSetting:
                if (value.isBool())
                {
                    osSetting.setting.booleanSetting->SetValue(value.asBool());
                }
                break;
            case SettingType::BooleanValue:
                if (value.isBool())
                {
                    *osSetting.setting.boolValue = value.asBool();
                }
                break;
            case SettingType::UnsignedInt:
                if (value.isInt())
                {
                    *osSetting.setting.uint32Value = (uint32_t)value.asUInt64();
                }
                break;
            case SettingType::Float:
                if (value.isDouble())
                {
                    *osSetting.setting.floatValue = (float)value.asDouble();
                }
                else if (value.isInt())
                {
                    *osSetting.setting.floatValue = (float)value.asInt64();
                }
                break;
            case SettingType::StringValue:
                if (value.isString())
                {
                    *osSetting.setting.stringValue = value.asString();
                }
                break;
            case SettingType::ControllerType:
                if (value.isString())
                {
                    *osSetting.setting.controllerType = ParseEnum<InputSettings::ControllerType>(value.asString());
                }
                break;
            case SettingType::S32Setting:
                if (value.isInt())
                {
                    osSetting.setting.s32Setting->SetValue(static_cast<s32>(value.asInt64()));
                }
                break;
            case SettingType::U32Switchable:
                if (value.isInt())
                {
                    osSetting.setting.u32Switchable->SetValue(static_cast<u32>(value.asUInt64()));
                }
                break;
            case SettingType::S64Switchable:
                if (value.isInt())
                {
                    osSetting.setting.s64Switchable->SetValue(static_cast<s64>(value.asInt64()));
                }
                break;
            default:
                UNIMPLEMENTED();
            }
        }
    }

    for (const OsSetting & osSetting : settings)
    {
        if (osSetting.identifier == nullptr)
        {
            continue;
        }

        switch (osSetting.settingType)
        {
        case SettingType::StringSetting:
            g_settings->SetDefaultString(osSetting.identifier, osSetting.setting.stringSetting->GetDefault().c_str());
            g_settings->SetString(osSetting.identifier, osSetting.setting.stringSetting->GetValue().c_str());
            break;
        case SettingType::AudioEngine:
            g_settings->SetDefaultInt(osSetting.identifier, (int32_t)osSetting.setting.audioEngine->GetDefault());
            g_settings->SetInt(osSetting.identifier, (int32_t)osSetting.setting.audioEngine->GetValue());
            break;
        case SettingType::AudioMode:
            g_settings->SetDefaultInt(osSetting.identifier, (int32_t)osSetting.setting.audioMode->GetDefault());
            g_settings->SetInt(osSetting.identifier, (int32_t)osSetting.setting.audioMode->GetValue());
            break;
        case SettingType::Language:
            g_settings->SetDefaultInt(osSetting.identifier, (int32_t)osSetting.setting.languageIndex->GetDefault());
            g_settings->SetInt(osSetting.identifier, (int32_t)osSetting.setting.languageIndex->GetValue());
            break;
        case SettingType::DockedMode:
            g_settings->SetDefaultInt(osSetting.identifier, (int32_t)osSetting.setting.dockedMode->GetDefault());
            g_settings->SetInt(osSetting.identifier, (int32_t)osSetting.setting.dockedMode->GetValue());
            break;
        case SettingType::U8:
            g_settings->SetDefaultInt(osSetting.identifier, (int32_t)osSetting.setting.u8->GetDefault());
            g_settings->SetInt(osSetting.identifier, (int32_t)osSetting.setting.u8->GetValue());
            break;
        case SettingType::U16:
            g_settings->SetDefaultInt(osSetting.identifier, (int32_t)osSetting.setting.u16->GetDefault());
            g_settings->SetInt(osSetting.identifier, (int32_t)osSetting.setting.u16->GetValue());
            break;
        case SettingType::BooleanSwitchable:
            g_settings->SetDefaultBool(osSetting.identifier, osSetting.setting.booleanSwitchable->GetDefault() != 0);
            g_settings->SetBool(osSetting.identifier, osSetting.setting.booleanSwitchable->GetValue() != 0);
            break;
        case SettingType::BooleanSetting:
            g_settings->SetDefaultBool(osSetting.identifier, osSetting.setting.booleanSetting->GetDefault() != 0);
            g_settings->SetBool(osSetting.identifier, osSetting.setting.booleanSetting->GetValue() != 0);
            break;
        case SettingType::BooleanValue:
            g_settings->SetDefaultBool(osSetting.identifier, osSetting.defaultValue.boolValue);
            g_settings->SetBool(osSetting.identifier, *osSetting.setting.boolValue);
            break;
        case SettingType::Float:
            g_settings->SetDefaultFloat(osSetting.identifier, osSetting.defaultValue.floatValue);
            g_settings->SetFloat(osSetting.identifier, *osSetting.setting.floatValue);
            break;
        case SettingType::S32Setting:
            g_settings->SetDefaultInt(osSetting.identifier, osSetting.setting.s32Setting->GetDefault());
            g_settings->SetInt(osSetting.identifier, osSetting.setting.s32Setting->GetValue());
            break;
        case SettingType::U32Switchable:
            g_settings->SetDefaultInt(osSetting.identifier, static_cast<int32_t>(osSetting.setting.u32Switchable->GetDefault()));
            g_settings->SetInt(osSetting.identifier, static_cast<int32_t>(osSetting.setting.u32Switchable->GetValue()));
            break;
        case SettingType::S64Switchable:
            g_settings->SetDefaultInt(osSetting.identifier, static_cast<int32_t>(osSetting.setting.s64Switchable->GetDefault()));
            g_settings->SetInt(osSetting.identifier, static_cast<int32_t>(osSetting.setting.s64Switchable->GetValue()));
            break;
        default:
            UNIMPLEMENTED();
        }
        g_settings->RegisterCallback(osSetting.identifier, OsSettingChanged, nullptr);
    }
}

void SaveOsSettings(void)
{
    JsonValue root;

    for (const OsSetting & osSetting : settings)
    {
        switch (osSetting.settingType)
        {
        case SettingType::StringSetting:
            if (osSetting.setting.stringSetting->GetValue() != osSetting.setting.stringSetting->GetDefault())
            {
                JsonSetNestedValue(root, osSetting.json_path, osSetting.setting.stringSetting->GetValue());
            }
            break;
        case SettingType::AudioEngine:
            if (osSetting.setting.audioEngine->GetValue() != osSetting.setting.audioEngine->GetDefault())
            {
                JsonSetNestedValue(root, osSetting.json_path, Settings::CanonicalizeEnum(osSetting.setting.audioEngine->GetValue()));
            }
            break;
        case SettingType::AudioMode:
            if (osSetting.setting.audioMode->GetValue() != osSetting.setting.audioMode->GetDefault())
            {
                JsonSetNestedValue(root, osSetting.json_path, Settings::CanonicalizeEnum(osSetting.setting.audioMode->GetValue()));
            }
            break;
        case SettingType::Language:
            if (osSetting.setting.languageIndex->GetValue() != osSetting.setting.languageIndex->GetDefault())
            {
                JsonSetNestedValue(root, osSetting.json_path, Settings::CanonicalizeEnum(osSetting.setting.languageIndex->GetValue()));
            }
            break;
        case SettingType::DockedMode:
            if (osSetting.setting.dockedMode->GetValue() != osSetting.setting.dockedMode->GetDefault())
            {
                JsonSetNestedValue(root, osSetting.json_path, Settings::CanonicalizeEnum(osSetting.setting.dockedMode->GetValue()));
            }
            break;
        case SettingType::U8:
            if (osSetting.setting.u8->GetValue() != osSetting.setting.u8->GetDefault())
            {
                JsonSetNestedValue(root, osSetting.json_path, (int32_t)osSetting.setting.u8->GetValue());
            }
            break;
        case SettingType::U16:
            if (osSetting.setting.u16->GetValue() != osSetting.setting.u16->GetDefault())
            {
                JsonSetNestedValue(root, osSetting.json_path, (int32_t)osSetting.setting.u16->GetValue());
            }
            break;
        case SettingType::BooleanSwitchable:
            if (osSetting.setting.booleanSwitchable->GetValue() != osSetting.setting.booleanSwitchable->GetDefault())
            {
                JsonSetNestedValue(root, osSetting.json_path, osSetting.setting.booleanSwitchable->GetValue() != 0);
            }
            break;
        case SettingType::BooleanSetting:
            if (osSetting.setting.booleanSetting->GetValue() != osSetting.setting.booleanSetting->GetDefault())
            {
                JsonSetNestedValue(root, osSetting.json_path, osSetting.setting.booleanSetting->GetValue() != 0);
            }
            break;
        case SettingType::BooleanValue:
            if (*osSetting.setting.boolValue != osSetting.defaultValue.boolValue)
            {
                JsonSetNestedValue(root, osSetting.json_path, *osSetting.setting.boolValue != 0);
            }
            break;
        case SettingType::UnsignedInt:
            if (*osSetting.setting.uint32Value != osSetting.defaultValue.uint32Value)
            {
                JsonSetNestedValue(root, osSetting.json_path, *osSetting.setting.uint32Value);
            }
            break;
        case SettingType::Float:
            if (*osSetting.setting.floatValue != osSetting.defaultValue.floatValue)
            {
                JsonSetNestedValue(root, osSetting.json_path, *osSetting.setting.floatValue);
            }
            break;
        case SettingType::StringValue:
            if (*osSetting.setting.stringValue != osSetting.defaultValue.sringValue)
            {
                JsonSetNestedValue(root, osSetting.json_path, *osSetting.setting.stringValue);
            }
            break;
        case SettingType::ControllerType:
            if (*osSetting.setting.controllerType != osSetting.defaultValue.controllerType)
            {
                JsonSetNestedValue(root, osSetting.json_path, CanonicalizeEnum(*osSetting.setting.controllerType));
            }
            break;
        case SettingType::S32Setting:
            if (osSetting.setting.s32Setting->GetValue() != osSetting.setting.s32Setting->GetDefault())
            {
                JsonSetNestedValue(root, osSetting.json_path, osSetting.setting.s32Setting->GetValue());
            }
            break;
        case SettingType::U32Switchable:
            if (osSetting.setting.u32Switchable->GetValue() != osSetting.setting.u32Switchable->GetDefault())
            {
                JsonSetNestedValue(root, osSetting.json_path,
                                   static_cast<int32_t>(osSetting.setting.u32Switchable->GetValue()));
            }
            break;
        case SettingType::S64Switchable:
            if (osSetting.setting.s64Switchable->GetValue() != osSetting.setting.s64Switchable->GetDefault())
            {
                JsonSetNestedValue(root, osSetting.json_path,
                                   static_cast<int32_t>(osSetting.setting.s64Switchable->GetValue()));
            }
            break;
        default:
            UNIMPLEMENTED();
        }
    }
    g_settings->SetSectionSettings("nxemu-os", root.isNull() ? "" : JsonStyledWriter().write(root));
}

namespace
{
    OsSetting::OsSetting(const char * id,const char * path, Settings::SwitchableSetting<std::string> * val) :
        identifier(id),
        json_path(path),
        settingType(SettingType::StringSetting)
    {
        setting.stringSetting = val;
    }

    OsSetting::OsSetting(const char * id, const char * path, Settings::SwitchableSetting<Settings::AudioEngine> * val) :
        identifier(id),
        json_path(path),
        settingType(SettingType::AudioEngine)
    {
        setting.audioEngine = val;
    }

    OsSetting::OsSetting(const char * id, const char * path, Settings::SwitchableSetting<Settings::AudioMode, true> * val) :
        identifier(id),
        json_path(path),
        settingType(SettingType::AudioMode)
    {
        setting.audioMode = val;
    }

    OsSetting::OsSetting(const char * id, const char * path, Settings::SwitchableSetting<Settings::Language, true> * val) :
        identifier(id),
        json_path(path),
        settingType(SettingType::Language)
    {
        setting.languageIndex = val;
    }

    OsSetting::OsSetting(const char * id, const char * path, Settings::SwitchableSetting<Settings::DockedMode> * val) :
        identifier(id),
        json_path(path),
        settingType(SettingType::DockedMode)
    {
        setting.dockedMode = val;
    }

    OsSetting::OsSetting(const char * id, const char * path, Settings::SwitchableSetting<u8, true> * val) :
        identifier(id),
        json_path(path),
        settingType(SettingType::U8)
    {
        setting.u8 = val;
    }

    OsSetting::OsSetting(const char * id, const char * path, Settings::SwitchableSetting<u16, true> * val) :
        identifier(id),
        json_path(path),
        settingType(SettingType::U16)
    {
        setting.u16 = val;
    }

    OsSetting::OsSetting(const char * id, const char * path, Settings::SwitchableSetting<bool> * val) :
        identifier(id),
        json_path(path),
        settingType(SettingType::BooleanSwitchable)
    {
        setting.booleanSwitchable = val;
    }

    OsSetting::OsSetting(const char * id, const char * path, Settings::Setting<bool, false> * val) :
        identifier(id),
        json_path(path),
        settingType(SettingType::BooleanSetting)
    {
        setting.booleanSetting = val;
    }

    OsSetting::OsSetting(const char * id, const char * path, bool * val, bool defaultValue_) :
        identifier(id),
        json_path(path),
        settingType(SettingType::BooleanValue)
    {
        setting.boolValue = val;
        defaultValue.boolValue = defaultValue_;
    }

    OsSetting::OsSetting(const char * id, const char * path, int * val, int defaultValue_) :
        identifier(id),
        json_path(path),
        settingType(SettingType::UnsignedInt)
    {
        setting.uint32Value = (uint32_t *)val;
        defaultValue.uint32Value = defaultValue_;
    }

    OsSetting::OsSetting(const char * id, const char * path, float * val, float defaultValue_) :
        identifier(id),
        json_path(path),
        settingType(SettingType::Float)
    {
        setting.floatValue = val;
        defaultValue.floatValue = defaultValue_;
    }

    OsSetting::OsSetting(const char * id, const char * path, uint32_t * val, uint32_t defaultValue_) :
        identifier(id),
        json_path(path),
        settingType(SettingType::UnsignedInt)
    {
        setting.uint32Value = val;
        defaultValue.uint32Value = defaultValue_;
    }

    OsSetting::OsSetting(const char * id, const char * path, std::string * val, const char * defaultValue_) :
        identifier(id),
        json_path(path),
        settingType(SettingType::StringValue)
    {
        setting.stringValue = val;
        defaultValue.sringValue = defaultValue_;
    }

    OsSetting::OsSetting(const char * id, const char * path, InputSettings::ControllerType * val, InputSettings::ControllerType defaultValue_) :
        identifier(id),
        json_path(path),
        settingType(SettingType::ControllerType)
    {
        setting.controllerType = val;
        defaultValue.controllerType = defaultValue_;
    }

    OsSetting::OsSetting(const char * id, const char * path, Settings::Setting<s32> * val) :
        identifier(id),
        json_path(path),
        settingType(SettingType::S32Setting)
    {
        setting.s32Setting = val;
    }

    OsSetting::OsSetting(const char * id, const char * path, Settings::SwitchableSetting<u32> * val) :
        identifier(id),
        json_path(path),
        settingType(SettingType::U32Switchable)
    {
        setting.u32Switchable = val;
    }

    OsSetting::OsSetting(const char * id, const char * path, Settings::SwitchableSetting<s64, true> * val) :
        identifier(id),
        json_path(path),
        settingType(SettingType::S64Switchable)
    {
        setting.s64Switchable = val;
    }
}