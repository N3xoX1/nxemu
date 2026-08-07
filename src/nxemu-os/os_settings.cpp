#include "os_settings.h"
#include "os_enum_strings.h"
#include "os_settings_identifiers.h"
#include <algorithm>
#include <climits>
#include <common/json_util.h>
#include <cstring>
#include <nxemu-module-spec/base.h>
#include <yuzu_common/yuzu_assert.h>

extern IModuleSettings * g_settings;

OSSettings osSettings{};

namespace
{
enum class SettingType
{
    Boolean,
    IntValue,
    IntValueRanged,
    S32,
    S64Ranged,
    U32,
    Float,
    String,
    ControllerType,
    AudioEngine,
    AudioMode,
    Language,
    DockedMode,
    MemoryLayout,
};

enum class RangedWidth
{
    None,
    U8,
    U16,
};

class OsSetting
{
public:
    OsSetting(const char * id, const char * path, bool * val, bool defaultValue);
    OsSetting(const char * id, const char * path, float * val, float defaultValue);
    OsSetting(const char * id, const char * path, std::string * val, const char * defaultValue);
    OsSetting(const char * id, const char * path, ControllerType * val, ControllerType defaultValue);
    OsSetting(const char * id, const char * path, AudioCore::Sink::AudioEngine * val, AudioCore::Sink::AudioEngine defaultValue);
    OsSetting(const char * id, const char * path, AudioMode * val, AudioMode defaultValue);
    OsSetting(const char * id, const char * path, Language * val, Language defaultValue);
    OsSetting(const char * id, const char * path, DockedMode * val, DockedMode defaultValue);
    OsSetting(const char * id, const char * path, MemoryLayout * val, MemoryLayout defaultValue);
    OsSetting(const char * id, const char * path, u8 * val, int32_t defaultValue, int32_t minValue, int32_t maxValue);
    OsSetting(const char * id, const char * path, u16 * val, int32_t defaultValue, int32_t minValue, int32_t maxValue);
    OsSetting(const char * id, const char * path, s32 * val, s32 defaultValue);
    OsSetting(const char * id, const char * path, u32 * val, u32 defaultValue);
    OsSetting(const char * id, const char * path, s64 * val, s64 defaultValue, s64 minValue, s64 maxValue);

    const char * identifier;
    const char * json_path;
    SettingType settingType;
    RangedWidth rangedWidth{RangedWidth::None};
    int32_t minValue{};
    int32_t maxValue{};
    s64 minValueS64{};
    s64 maxValueS64{};
    union
    {
        bool boolValue;
        int32_t intValue;
        s32 s32Value;
        u32 u32Value;
        s64 s64Value;
        float floatValue;
        const char * stringValue;
        ControllerType controllerType;
        AudioCore::Sink::AudioEngine audioEngine;
        AudioMode audioMode;
        Language language;
        DockedMode dockedMode;
        MemoryLayout memoryLayout;
    } defaults;
    union
    {
        bool * boolValue;
        int32_t * intValue;
        u8 * u8Value;
        u16 * u16Value;
        s32 * s32Value;
        u32 * u32Value;
        s64 * s64Value;
        float * floatValue;
        std::string * stringValue;
        ControllerType * controllerType;
        AudioCore::Sink::AudioEngine * audioEngine;
        AudioMode * audioMode;
        Language * language;
        DockedMode * dockedMode;
        MemoryLayout * memoryLayout;
    } value;
};

static OsSetting settings[] = {
        { nullptr, "controller\\player_0\\Connected", &osSettings.players[0].connected, true },
        { nullptr, "controller\\player_0\\ControllerType", &osSettings.players[0].controller_type, ControllerType::ProController },
        { nullptr, "controller\\player_0\\Button\\A", &osSettings.players[0].buttons[(size_t)NativeButtonValues::A], "engine:keyboard,code:67,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\B", &osSettings.players[0].buttons[(size_t)NativeButtonValues::B], "engine:keyboard,code:88,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\X", &osSettings.players[0].buttons[(size_t)NativeButtonValues::X], "engine:keyboard,code:86,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\Y", &osSettings.players[0].buttons[(size_t)NativeButtonValues::Y], "engine:keyboard,code:90,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\LStick", &osSettings.players[0].buttons[(size_t)NativeButtonValues::LStick], "engine:keyboard,code:70,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\RStick", &osSettings.players[0].buttons[(size_t)NativeButtonValues::RStick], "engine:keyboard,code:71,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\L", &osSettings.players[0].buttons[(size_t)NativeButtonValues::L], "engine:keyboard,code:81,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\R", &osSettings.players[0].buttons[(size_t)NativeButtonValues::R], "engine:keyboard,code:69,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\ZL", &osSettings.players[0].buttons[(size_t)NativeButtonValues::ZL], "engine:keyboard,code:82,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\ZR", &osSettings.players[0].buttons[(size_t)NativeButtonValues::ZR], "engine:keyboard,code:84,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\Plus", &osSettings.players[0].buttons[(size_t)NativeButtonValues::Plus], "engine:keyboard,code:77,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\Minus", &osSettings.players[0].buttons[(size_t)NativeButtonValues::Minus], "engine:keyboard,code:78,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\DLeft", &osSettings.players[0].buttons[(size_t)NativeButtonValues::DLeft], "engine:keyboard,code:37,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\DUp", &osSettings.players[0].buttons[(size_t)NativeButtonValues::DUp], "engine:keyboard,code:38,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\DRight", &osSettings.players[0].buttons[(size_t)NativeButtonValues::DRight], "engine:keyboard,code:39,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\DDown", &osSettings.players[0].buttons[(size_t)NativeButtonValues::DDown], "engine:keyboard,code:40,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\SLLeft", &osSettings.players[0].buttons[(size_t)NativeButtonValues::SLLeft], "engine:keyboard,code:81,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\SRLeft", &osSettings.players[0].buttons[(size_t)NativeButtonValues::SRLeft], "engine:keyboard,code:69,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\Home", &osSettings.players[0].buttons[(size_t)NativeButtonValues::Home], "engine:keyboard,code:0,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\Screenshot", &osSettings.players[0].buttons[(size_t)NativeButtonValues::Screenshot], "engine:keyboard,code:0,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\SLRight", &osSettings.players[0].buttons[(size_t)NativeButtonValues::SLRight], "engine:keyboard,code:81,toggle:0" },
        { nullptr, "controller\\player_0\\Button\\SRRight", &osSettings.players[0].buttons[(size_t)NativeButtonValues::SRRight], "engine:keyboard,code:69,toggle:0" },
        { nullptr, "controller\\player_0\\Analog\\LStick", &osSettings.players[0].analogs[(size_t)NativeAnalogValues::LStick], "engine:analog_from_button,up:engine$0keyboard$1code$087$1toggle$00,left:engine$0keyboard$1code$065$1toggle$00,modifier:engine$0keyboard$1code$016$1toggle$00,down:engine$0keyboard$1code$083$1toggle$00,right:engine$0keyboard$1code$068$1toggle$00" },
        { nullptr, "controller\\player_0\\Analog\\RStick", &osSettings.players[0].analogs[(size_t)NativeAnalogValues::RStick], "engine:analog_from_button,up:engine$0keyboard$1code$073$1toggle$00,left:engine$0keyboard$1code$074$1toggle$00,modifier:engine$0keyboard$1code$00$1toggle$00,down:engine$0keyboard$1code$075$1toggle$00,right:engine$0keyboard$1code$076$1toggle$00" },
        { nullptr, "controller\\player_0\\Motion\\Left", &osSettings.players[0].motions[(size_t)NativeMotionValues::MotionLeft], "engine:keyboard,code:55,toggle:0" },
        { nullptr, "controller\\player_0\\Motion\\Right", &osSettings.players[0].motions[(size_t)NativeMotionValues::MotionRight], "engine:keyboard,code:56,toggle:0" },
        { NXOsSetting::Player0VibrationEnabled, "controller\\player_0\\Vibration\\Enabled", &osSettings.players[0].vibration_enabled, true },
        { NXOsSetting::Player0VibrationStrength, "controller\\player_0\\Vibration\\Strength", &osSettings.players[0].vibration_strength, 100 },
        { nullptr, "controller\\player_0\\BodyColor\\Left", &osSettings.players[0].body_color_left, 0 },
        { nullptr, "controller\\player_0\\BodyColor\\Right", &osSettings.players[0].body_color_right, 0 },
        { nullptr, "controller\\player_0\\ButtonColor\\Left", &osSettings.players[0].button_color_left, 0 },
        { nullptr, "controller\\player_0\\ButtonColor\\Right", &osSettings.players[0].button_color_right, 0 },
        { nullptr, "controller\\player_0\\ProfileName", &osSettings.players[0].profile_name, "" },
        { nullptr, "controller\\player_0\\Vibration\\UseSystem", &osSettings.players[0].use_system_vibrator, false },

        { nullptr, "controller\\player_1\\Connected", &osSettings.players[1].connected, false },
        { nullptr, "controller\\player_1\\ControllerType", &osSettings.players[1].controller_type, ControllerType::ProController },
        { nullptr, "controller\\player_1\\Button\\A", &osSettings.players[1].buttons[(size_t)NativeButtonValues::A], "" },
        { nullptr, "controller\\player_1\\Button\\B", &osSettings.players[1].buttons[(size_t)NativeButtonValues::B], "" },
        { nullptr, "controller\\player_1\\Button\\X", &osSettings.players[1].buttons[(size_t)NativeButtonValues::X], "" },
        { nullptr, "controller\\player_1\\Button\\Y", &osSettings.players[1].buttons[(size_t)NativeButtonValues::Y], "" },
        { nullptr, "controller\\player_1\\Button\\LStick", &osSettings.players[1].buttons[(size_t)NativeButtonValues::LStick], "" },
        { nullptr, "controller\\player_1\\Button\\RStick", &osSettings.players[1].buttons[(size_t)NativeButtonValues::RStick], "" },
        { nullptr, "controller\\player_1\\Button\\L", &osSettings.players[1].buttons[(size_t)NativeButtonValues::L], "" },
        { nullptr, "controller\\player_1\\Button\\R", &osSettings.players[1].buttons[(size_t)NativeButtonValues::R], "" },
        { nullptr, "controller\\player_1\\Button\\ZL", &osSettings.players[1].buttons[(size_t)NativeButtonValues::ZL], "" },
        { nullptr, "controller\\player_1\\Button\\ZR", &osSettings.players[1].buttons[(size_t)NativeButtonValues::ZR], "" },
        { nullptr, "controller\\player_1\\Button\\Plus", &osSettings.players[1].buttons[(size_t)NativeButtonValues::Plus], "" },
        { nullptr, "controller\\player_1\\Button\\Minus", &osSettings.players[1].buttons[(size_t)NativeButtonValues::Minus], "" },
        { nullptr, "controller\\player_1\\Button\\DLeft", &osSettings.players[1].buttons[(size_t)NativeButtonValues::DLeft], "" },
        { nullptr, "controller\\player_1\\Button\\DUp", &osSettings.players[1].buttons[(size_t)NativeButtonValues::DUp], "" },
        { nullptr, "controller\\player_1\\Button\\DRight", &osSettings.players[1].buttons[(size_t)NativeButtonValues::DRight], "" },
        { nullptr, "controller\\player_1\\Button\\DDown", &osSettings.players[1].buttons[(size_t)NativeButtonValues::DDown], "" },
        { nullptr, "controller\\player_1\\Button\\SLLeft", &osSettings.players[1].buttons[(size_t)NativeButtonValues::SLLeft], "" },
        { nullptr, "controller\\player_1\\Button\\SRLeft", &osSettings.players[1].buttons[(size_t)NativeButtonValues::SRLeft], "" },
        { nullptr, "controller\\player_1\\Button\\Home", &osSettings.players[1].buttons[(size_t)NativeButtonValues::Home], "" },
        { nullptr, "controller\\player_1\\Button\\Screenshot", &osSettings.players[1].buttons[(size_t)NativeButtonValues::Screenshot], "" },
        { nullptr, "controller\\player_1\\Button\\SLRight", &osSettings.players[1].buttons[(size_t)NativeButtonValues::SLRight], "" },
        { nullptr, "controller\\player_1\\Button\\SRRight", &osSettings.players[1].buttons[(size_t)NativeButtonValues::SRRight], "" },
        { nullptr, "controller\\player_1\\Analog\\LStick", &osSettings.players[1].analogs[(size_t)NativeAnalogValues::LStick], "" },
        { nullptr, "controller\\player_1\\Analog\\RStick", &osSettings.players[1].analogs[(size_t)NativeAnalogValues::RStick], "" },
        { nullptr, "controller\\player_1\\Motion\\Left", &osSettings.players[1].motions[(size_t)NativeMotionValues::MotionLeft], "" },
        { nullptr, "controller\\player_1\\Motion\\Right", &osSettings.players[1].motions[(size_t)NativeMotionValues::MotionRight], "" },
        { NXOsSetting::Player1VibrationEnabled, "controller\\player_1\\Vibration\\Enabled", &osSettings.players[1].vibration_enabled, true },
        { NXOsSetting::Player1VibrationStrength, "controller\\player_1\\Vibration\\Strength", &osSettings.players[1].vibration_strength, 100 },
        { nullptr, "controller\\player_1\\BodyColor\\Left", &osSettings.players[1].body_color_left, 0 },
        { nullptr, "controller\\player_1\\BodyColor\\Right", &osSettings.players[1].body_color_right, 0 },
        { nullptr, "controller\\player_1\\ButtonColor\\Left", &osSettings.players[1].button_color_left, 0 },
        { nullptr, "controller\\player_1\\ButtonColor\\Right", &osSettings.players[1].button_color_right, 0 },
        { nullptr, "controller\\player_1\\ProfileName", &osSettings.players[1].profile_name, "" },
        { nullptr, "controller\\player_1\\Vibration\\UseSystem", &osSettings.players[1].use_system_vibrator, false },

        {nullptr, "controller\\player_2\\Connected", &osSettings.players[2].connected, false},
        {nullptr, "controller\\player_2\\ControllerType", &osSettings.players[2].controller_type, ControllerType::ProController},
        {nullptr, "controller\\player_2\\Button\\A", &osSettings.players[2].buttons[(size_t)NativeButtonValues::A], ""},
        {nullptr, "controller\\player_2\\Button\\B", &osSettings.players[2].buttons[(size_t)NativeButtonValues::B], ""},
        {nullptr, "controller\\player_2\\Button\\X", &osSettings.players[2].buttons[(size_t)NativeButtonValues::X], ""},
        {nullptr, "controller\\player_2\\Button\\Y", &osSettings.players[2].buttons[(size_t)NativeButtonValues::Y], ""},
        {nullptr, "controller\\player_2\\Button\\LStick", &osSettings.players[2].buttons[(size_t)NativeButtonValues::LStick], ""},
        {nullptr, "controller\\player_2\\Button\\RStick", &osSettings.players[2].buttons[(size_t)NativeButtonValues::RStick], ""},
        {nullptr, "controller\\player_2\\Button\\L", &osSettings.players[2].buttons[(size_t)NativeButtonValues::L], ""},
        {nullptr, "controller\\player_2\\Button\\R", &osSettings.players[2].buttons[(size_t)NativeButtonValues::R], ""},
        {nullptr, "controller\\player_2\\Button\\ZL", &osSettings.players[2].buttons[(size_t)NativeButtonValues::ZL], ""},
        {nullptr, "controller\\player_2\\Button\\ZR", &osSettings.players[2].buttons[(size_t)NativeButtonValues::ZR], ""},
        {nullptr, "controller\\player_2\\Button\\Plus", &osSettings.players[2].buttons[(size_t)NativeButtonValues::Plus], ""},
        {nullptr, "controller\\player_2\\Button\\Minus", &osSettings.players[2].buttons[(size_t)NativeButtonValues::Minus], ""},
        {nullptr, "controller\\player_2\\Button\\DLeft", &osSettings.players[2].buttons[(size_t)NativeButtonValues::DLeft], ""},
        {nullptr, "controller\\player_2\\Button\\DUp", &osSettings.players[2].buttons[(size_t)NativeButtonValues::DUp], ""},
        {nullptr, "controller\\player_2\\Button\\DRight", &osSettings.players[2].buttons[(size_t)NativeButtonValues::DRight], ""},
        {nullptr, "controller\\player_2\\Button\\DDown", &osSettings.players[2].buttons[(size_t)NativeButtonValues::DDown], ""},
        {nullptr, "controller\\player_2\\Button\\SLLeft", &osSettings.players[2].buttons[(size_t)NativeButtonValues::SLLeft], ""},
        {nullptr, "controller\\player_2\\Button\\SRLeft", &osSettings.players[2].buttons[(size_t)NativeButtonValues::SRLeft], ""},
        {nullptr, "controller\\player_2\\Button\\Home", &osSettings.players[2].buttons[(size_t)NativeButtonValues::Home], ""},
        {nullptr, "controller\\player_2\\Button\\Screenshot", &osSettings.players[2].buttons[(size_t)NativeButtonValues::Screenshot], ""},
        {nullptr, "controller\\player_2\\Button\\SLRight", &osSettings.players[2].buttons[(size_t)NativeButtonValues::SLRight], ""},
        {nullptr, "controller\\player_2\\Button\\SRRight", &osSettings.players[2].buttons[(size_t)NativeButtonValues::SRRight], ""},
        {nullptr, "controller\\player_2\\Analog\\LStick", &osSettings.players[2].analogs[(size_t)NativeAnalogValues::LStick], ""},
        {nullptr, "controller\\player_2\\Analog\\RStick", &osSettings.players[2].analogs[(size_t)NativeAnalogValues::RStick], ""},
        {nullptr, "controller\\player_2\\Motion\\Left", &osSettings.players[2].motions[(size_t)NativeMotionValues::MotionLeft], ""},
        {nullptr, "controller\\player_2\\Motion\\Right", &osSettings.players[2].motions[(size_t)NativeMotionValues::MotionRight], ""},
        {NXOsSetting::Player2VibrationEnabled, "controller\\player_2\\Vibration\\Enabled", &osSettings.players[2].vibration_enabled, true},
        {NXOsSetting::Player2VibrationStrength, "controller\\player_2\\Vibration\\Strength", &osSettings.players[2].vibration_strength, 100},
        {nullptr, "controller\\player_2\\BodyColor\\Left", &osSettings.players[2].body_color_left, 0},
        {nullptr, "controller\\player_2\\BodyColor\\Right", &osSettings.players[2].body_color_right, 0},
        {nullptr, "controller\\player_2\\ButtonColor\\Left", &osSettings.players[2].button_color_left, 0},
        {nullptr, "controller\\player_2\\ButtonColor\\Right", &osSettings.players[2].button_color_right, 0},
        {nullptr, "controller\\player_2\\ProfileName", &osSettings.players[2].profile_name, ""},
        {nullptr, "controller\\player_2\\Vibration\\UseSystem", &osSettings.players[2].use_system_vibrator, false},

        {nullptr, "controller\\player_3\\Connected", &osSettings.players[3].connected, false},
        {nullptr, "controller\\player_3\\ControllerType", &osSettings.players[3].controller_type, ControllerType::ProController},
        {nullptr, "controller\\player_3\\Button\\A", &osSettings.players[3].buttons[(size_t)NativeButtonValues::A], ""},
        {nullptr, "controller\\player_3\\Button\\B", &osSettings.players[3].buttons[(size_t)NativeButtonValues::B], ""},
        {nullptr, "controller\\player_3\\Button\\X", &osSettings.players[3].buttons[(size_t)NativeButtonValues::X], ""},
        {nullptr, "controller\\player_3\\Button\\Y", &osSettings.players[3].buttons[(size_t)NativeButtonValues::Y], ""},
        {nullptr, "controller\\player_3\\Button\\LStick", &osSettings.players[3].buttons[(size_t)NativeButtonValues::LStick], ""},
        {nullptr, "controller\\player_3\\Button\\RStick", &osSettings.players[3].buttons[(size_t)NativeButtonValues::RStick], ""},
        {nullptr, "controller\\player_3\\Button\\L", &osSettings.players[3].buttons[(size_t)NativeButtonValues::L], ""},
        {nullptr, "controller\\player_3\\Button\\R", &osSettings.players[3].buttons[(size_t)NativeButtonValues::R], ""},
        {nullptr, "controller\\player_3\\Button\\ZL", &osSettings.players[3].buttons[(size_t)NativeButtonValues::ZL], ""},
        {nullptr, "controller\\player_3\\Button\\ZR", &osSettings.players[3].buttons[(size_t)NativeButtonValues::ZR], ""},
        {nullptr, "controller\\player_3\\Button\\Plus", &osSettings.players[3].buttons[(size_t)NativeButtonValues::Plus], ""},
        {nullptr, "controller\\player_3\\Button\\Minus", &osSettings.players[3].buttons[(size_t)NativeButtonValues::Minus], ""},
        {nullptr, "controller\\player_3\\Button\\DLeft", &osSettings.players[3].buttons[(size_t)NativeButtonValues::DLeft], ""},
        {nullptr, "controller\\player_3\\Button\\DUp", &osSettings.players[3].buttons[(size_t)NativeButtonValues::DUp], ""},
        {nullptr, "controller\\player_3\\Button\\DRight", &osSettings.players[3].buttons[(size_t)NativeButtonValues::DRight], ""},
        {nullptr, "controller\\player_3\\Button\\DDown", &osSettings.players[3].buttons[(size_t)NativeButtonValues::DDown], ""},
        {nullptr, "controller\\player_3\\Button\\SLLeft", &osSettings.players[3].buttons[(size_t)NativeButtonValues::SLLeft], ""},
        {nullptr, "controller\\player_3\\Button\\SRLeft", &osSettings.players[3].buttons[(size_t)NativeButtonValues::SRLeft], ""},
        {nullptr, "controller\\player_3\\Button\\Home", &osSettings.players[3].buttons[(size_t)NativeButtonValues::Home], ""},
        {nullptr, "controller\\player_3\\Button\\Screenshot", &osSettings.players[3].buttons[(size_t)NativeButtonValues::Screenshot], ""},
        {nullptr, "controller\\player_3\\Button\\SLRight", &osSettings.players[3].buttons[(size_t)NativeButtonValues::SLRight], ""},
        {nullptr, "controller\\player_3\\Button\\SRRight", &osSettings.players[3].buttons[(size_t)NativeButtonValues::SRRight], ""},
        {nullptr, "controller\\player_3\\Analog\\LStick", &osSettings.players[3].analogs[(size_t)NativeAnalogValues::LStick], ""},
        {nullptr, "controller\\player_3\\Analog\\RStick", &osSettings.players[3].analogs[(size_t)NativeAnalogValues::RStick], ""},
        {nullptr, "controller\\player_3\\Motion\\Left", &osSettings.players[3].motions[(size_t)NativeMotionValues::MotionLeft], ""},
        {nullptr, "controller\\player_3\\Motion\\Right", &osSettings.players[3].motions[(size_t)NativeMotionValues::MotionRight], ""},
        {NXOsSetting::Player3VibrationEnabled, "controller\\player_3\\Vibration\\Enabled", &osSettings.players[3].vibration_enabled, true},
        {NXOsSetting::Player3VibrationStrength, "controller\\player_3\\Vibration\\Strength", &osSettings.players[3].vibration_strength, 100},
        {nullptr, "controller\\player_3\\BodyColor\\Left", &osSettings.players[3].body_color_left, 0},
        {nullptr, "controller\\player_3\\BodyColor\\Right", &osSettings.players[3].body_color_right, 0},
        {nullptr, "controller\\player_3\\ButtonColor\\Left", &osSettings.players[3].button_color_left, 0},
        {nullptr, "controller\\player_3\\ButtonColor\\Right", &osSettings.players[3].button_color_right, 0},
        {nullptr, "controller\\player_3\\ProfileName", &osSettings.players[3].profile_name, ""},
        {nullptr, "controller\\player_3\\Vibration\\UseSystem", &osSettings.players[3].use_system_vibrator, false},

        {nullptr, "controller\\player_4\\Connected", &osSettings.players[4].connected, false},
        {nullptr, "controller\\player_4\\ControllerType", &osSettings.players[4].controller_type, ControllerType::ProController},
        {nullptr, "controller\\player_4\\Button\\A", &osSettings.players[4].buttons[(size_t)NativeButtonValues::A], ""},
        {nullptr, "controller\\player_4\\Button\\B", &osSettings.players[4].buttons[(size_t)NativeButtonValues::B], ""},
        {nullptr, "controller\\player_4\\Button\\X", &osSettings.players[4].buttons[(size_t)NativeButtonValues::X], ""},
        {nullptr, "controller\\player_4\\Button\\Y", &osSettings.players[4].buttons[(size_t)NativeButtonValues::Y], ""},
        {nullptr, "controller\\player_4\\Button\\LStick", &osSettings.players[4].buttons[(size_t)NativeButtonValues::LStick], ""},
        {nullptr, "controller\\player_4\\Button\\RStick", &osSettings.players[4].buttons[(size_t)NativeButtonValues::RStick], ""},
        {nullptr, "controller\\player_4\\Button\\L", &osSettings.players[4].buttons[(size_t)NativeButtonValues::L], ""},
        {nullptr, "controller\\player_4\\Button\\R", &osSettings.players[4].buttons[(size_t)NativeButtonValues::R], ""},
        {nullptr, "controller\\player_4\\Button\\ZL", &osSettings.players[4].buttons[(size_t)NativeButtonValues::ZL], ""},
        {nullptr, "controller\\player_4\\Button\\ZR", &osSettings.players[4].buttons[(size_t)NativeButtonValues::ZR], ""},
        {nullptr, "controller\\player_4\\Button\\Plus", &osSettings.players[4].buttons[(size_t)NativeButtonValues::Plus], ""},
        {nullptr, "controller\\player_4\\Button\\Minus", &osSettings.players[4].buttons[(size_t)NativeButtonValues::Minus], ""},
        {nullptr, "controller\\player_4\\Button\\DLeft", &osSettings.players[4].buttons[(size_t)NativeButtonValues::DLeft], ""},
        {nullptr, "controller\\player_4\\Button\\DUp", &osSettings.players[4].buttons[(size_t)NativeButtonValues::DUp], ""},
        {nullptr, "controller\\player_4\\Button\\DRight", &osSettings.players[4].buttons[(size_t)NativeButtonValues::DRight], ""},
        {nullptr, "controller\\player_4\\Button\\DDown", &osSettings.players[4].buttons[(size_t)NativeButtonValues::DDown], ""},
        {nullptr, "controller\\player_4\\Button\\SLLeft", &osSettings.players[4].buttons[(size_t)NativeButtonValues::SLLeft], ""},
        {nullptr, "controller\\player_4\\Button\\SRLeft", &osSettings.players[4].buttons[(size_t)NativeButtonValues::SRLeft], ""},
        {nullptr, "controller\\player_4\\Button\\Home", &osSettings.players[4].buttons[(size_t)NativeButtonValues::Home], ""},
        {nullptr, "controller\\player_4\\Button\\Screenshot", &osSettings.players[4].buttons[(size_t)NativeButtonValues::Screenshot], ""},
        {nullptr, "controller\\player_4\\Button\\SLRight", &osSettings.players[4].buttons[(size_t)NativeButtonValues::SLRight], ""},
        {nullptr, "controller\\player_4\\Button\\SRRight", &osSettings.players[4].buttons[(size_t)NativeButtonValues::SRRight], ""},
        {nullptr, "controller\\player_4\\Analog\\LStick", &osSettings.players[4].analogs[(size_t)NativeAnalogValues::LStick], ""},
        {nullptr, "controller\\player_4\\Analog\\RStick", &osSettings.players[4].analogs[(size_t)NativeAnalogValues::RStick], ""},
        {nullptr, "controller\\player_4\\Motion\\Left", &osSettings.players[4].motions[(size_t)NativeMotionValues::MotionLeft], ""},
        {nullptr, "controller\\player_4\\Motion\\Right", &osSettings.players[4].motions[(size_t)NativeMotionValues::MotionRight], ""},
        {NXOsSetting::Player4VibrationEnabled, "controller\\player_4\\Vibration\\Enabled", &osSettings.players[4].vibration_enabled, true},
        {NXOsSetting::Player4VibrationStrength, "controller\\player_4\\Vibration\\Strength", &osSettings.players[4].vibration_strength, 100},
        {nullptr, "controller\\player_4\\BodyColor\\Left", &osSettings.players[4].body_color_left, 0},
        {nullptr, "controller\\player_4\\BodyColor\\Right", &osSettings.players[4].body_color_right, 0},
        {nullptr, "controller\\player_4\\ButtonColor\\Left", &osSettings.players[4].button_color_left, 0},
        {nullptr, "controller\\player_4\\ButtonColor\\Right", &osSettings.players[4].button_color_right, 0},
        {nullptr, "controller\\player_4\\ProfileName", &osSettings.players[4].profile_name, ""},
        {nullptr, "controller\\player_4\\Vibration\\UseSystem", &osSettings.players[4].use_system_vibrator, false},

        {nullptr, "controller\\player_5\\Connected", &osSettings.players[5].connected, false},
        {nullptr, "controller\\player_5\\ControllerType", &osSettings.players[5].controller_type, ControllerType::ProController},
        {nullptr, "controller\\player_5\\Button\\A", &osSettings.players[5].buttons[(size_t)NativeButtonValues::A], ""},
        {nullptr, "controller\\player_5\\Button\\B", &osSettings.players[5].buttons[(size_t)NativeButtonValues::B], ""},
        {nullptr, "controller\\player_5\\Button\\X", &osSettings.players[5].buttons[(size_t)NativeButtonValues::X], ""},
        {nullptr, "controller\\player_5\\Button\\Y", &osSettings.players[5].buttons[(size_t)NativeButtonValues::Y], ""},
        {nullptr, "controller\\player_5\\Button\\LStick", &osSettings.players[5].buttons[(size_t)NativeButtonValues::LStick], ""},
        {nullptr, "controller\\player_5\\Button\\RStick", &osSettings.players[5].buttons[(size_t)NativeButtonValues::RStick], ""},
        {nullptr, "controller\\player_5\\Button\\L", &osSettings.players[5].buttons[(size_t)NativeButtonValues::L], ""},
        {nullptr, "controller\\player_5\\Button\\R", &osSettings.players[5].buttons[(size_t)NativeButtonValues::R], ""},
        {nullptr, "controller\\player_5\\Button\\ZL", &osSettings.players[5].buttons[(size_t)NativeButtonValues::ZL], ""},
        {nullptr, "controller\\player_5\\Button\\ZR", &osSettings.players[5].buttons[(size_t)NativeButtonValues::ZR], ""},
        {nullptr, "controller\\player_5\\Button\\Plus", &osSettings.players[5].buttons[(size_t)NativeButtonValues::Plus], ""},
        {nullptr, "controller\\player_5\\Button\\Minus", &osSettings.players[5].buttons[(size_t)NativeButtonValues::Minus], ""},
        {nullptr, "controller\\player_5\\Button\\DLeft", &osSettings.players[5].buttons[(size_t)NativeButtonValues::DLeft], ""},
        {nullptr, "controller\\player_5\\Button\\DUp", &osSettings.players[5].buttons[(size_t)NativeButtonValues::DUp], ""},
        {nullptr, "controller\\player_5\\Button\\DRight", &osSettings.players[5].buttons[(size_t)NativeButtonValues::DRight], ""},
        {nullptr, "controller\\player_5\\Button\\DDown", &osSettings.players[5].buttons[(size_t)NativeButtonValues::DDown], ""},
        {nullptr, "controller\\player_5\\Button\\SLLeft", &osSettings.players[5].buttons[(size_t)NativeButtonValues::SLLeft], ""},
        {nullptr, "controller\\player_5\\Button\\SRLeft", &osSettings.players[5].buttons[(size_t)NativeButtonValues::SRLeft], ""},
        {nullptr, "controller\\player_5\\Button\\Home", &osSettings.players[5].buttons[(size_t)NativeButtonValues::Home], ""},
        {nullptr, "controller\\player_5\\Button\\Screenshot", &osSettings.players[5].buttons[(size_t)NativeButtonValues::Screenshot], ""},
        {nullptr, "controller\\player_5\\Button\\SLRight", &osSettings.players[5].buttons[(size_t)NativeButtonValues::SLRight], ""},
        {nullptr, "controller\\player_5\\Button\\SRRight", &osSettings.players[5].buttons[(size_t)NativeButtonValues::SRRight], ""},
        {nullptr, "controller\\player_5\\Analog\\LStick", &osSettings.players[5].analogs[(size_t)NativeAnalogValues::LStick], ""},
        {nullptr, "controller\\player_5\\Analog\\RStick", &osSettings.players[5].analogs[(size_t)NativeAnalogValues::RStick], ""},
        {nullptr, "controller\\player_5\\Motion\\Left", &osSettings.players[5].motions[(size_t)NativeMotionValues::MotionLeft], ""},
        {nullptr, "controller\\player_5\\Motion\\Right", &osSettings.players[5].motions[(size_t)NativeMotionValues::MotionRight], ""},
        {NXOsSetting::Player5VibrationEnabled, "controller\\player_5\\Vibration\\Enabled", &osSettings.players[5].vibration_enabled, true},
        {NXOsSetting::Player5VibrationStrength, "controller\\player_5\\Vibration\\Strength", &osSettings.players[5].vibration_strength, 100},
        {nullptr, "controller\\player_5\\BodyColor\\Left", &osSettings.players[5].body_color_left, 0},
        {nullptr, "controller\\player_5\\BodyColor\\Right", &osSettings.players[5].body_color_right, 0},
        {nullptr, "controller\\player_5\\ButtonColor\\Left", &osSettings.players[5].button_color_left, 0},
        {nullptr, "controller\\player_5\\ButtonColor\\Right", &osSettings.players[5].button_color_right, 0},
        {nullptr, "controller\\player_5\\ProfileName", &osSettings.players[5].profile_name, ""},
        {nullptr, "controller\\player_5\\Vibration\\UseSystem", &osSettings.players[5].use_system_vibrator, false},

        {nullptr, "controller\\player_6\\Connected", &osSettings.players[6].connected, false},
        {nullptr, "controller\\player_6\\ControllerType", &osSettings.players[6].controller_type, ControllerType::ProController},
        {nullptr, "controller\\player_6\\Button\\A", &osSettings.players[6].buttons[(size_t)NativeButtonValues::A], ""},
        {nullptr, "controller\\player_6\\Button\\B", &osSettings.players[6].buttons[(size_t)NativeButtonValues::B], ""},
        {nullptr, "controller\\player_6\\Button\\X", &osSettings.players[6].buttons[(size_t)NativeButtonValues::X], ""},
        {nullptr, "controller\\player_6\\Button\\Y", &osSettings.players[6].buttons[(size_t)NativeButtonValues::Y], ""},
        {nullptr, "controller\\player_6\\Button\\LStick", &osSettings.players[6].buttons[(size_t)NativeButtonValues::LStick], ""},
        {nullptr, "controller\\player_6\\Button\\RStick", &osSettings.players[6].buttons[(size_t)NativeButtonValues::RStick], ""},
        {nullptr, "controller\\player_6\\Button\\L", &osSettings.players[6].buttons[(size_t)NativeButtonValues::L], ""},
        {nullptr, "controller\\player_6\\Button\\R", &osSettings.players[6].buttons[(size_t)NativeButtonValues::R], ""},
        {nullptr, "controller\\player_6\\Button\\ZL", &osSettings.players[6].buttons[(size_t)NativeButtonValues::ZL], ""},
        {nullptr, "controller\\player_6\\Button\\ZR", &osSettings.players[6].buttons[(size_t)NativeButtonValues::ZR], ""},
        {nullptr, "controller\\player_6\\Button\\Plus", &osSettings.players[6].buttons[(size_t)NativeButtonValues::Plus], ""},
        {nullptr, "controller\\player_6\\Button\\Minus", &osSettings.players[6].buttons[(size_t)NativeButtonValues::Minus], ""},
        {nullptr, "controller\\player_6\\Button\\DLeft", &osSettings.players[6].buttons[(size_t)NativeButtonValues::DLeft], ""},
        {nullptr, "controller\\player_6\\Button\\DUp", &osSettings.players[6].buttons[(size_t)NativeButtonValues::DUp], ""},
        {nullptr, "controller\\player_6\\Button\\DRight", &osSettings.players[6].buttons[(size_t)NativeButtonValues::DRight], ""},
        {nullptr, "controller\\player_6\\Button\\DDown", &osSettings.players[6].buttons[(size_t)NativeButtonValues::DDown], ""},
        {nullptr, "controller\\player_6\\Button\\SLLeft", &osSettings.players[6].buttons[(size_t)NativeButtonValues::SLLeft], ""},
        {nullptr, "controller\\player_6\\Button\\SRLeft", &osSettings.players[6].buttons[(size_t)NativeButtonValues::SRLeft], ""},
        {nullptr, "controller\\player_6\\Button\\Home", &osSettings.players[6].buttons[(size_t)NativeButtonValues::Home], ""},
        {nullptr, "controller\\player_6\\Button\\Screenshot", &osSettings.players[6].buttons[(size_t)NativeButtonValues::Screenshot], ""},
        {nullptr, "controller\\player_6\\Button\\SLRight", &osSettings.players[6].buttons[(size_t)NativeButtonValues::SLRight], ""},
        {nullptr, "controller\\player_6\\Button\\SRRight", &osSettings.players[6].buttons[(size_t)NativeButtonValues::SRRight], ""},
        {nullptr, "controller\\player_6\\Analog\\LStick", &osSettings.players[6].analogs[(size_t)NativeAnalogValues::LStick], ""},
        {nullptr, "controller\\player_6\\Analog\\RStick", &osSettings.players[6].analogs[(size_t)NativeAnalogValues::RStick], ""},
        {nullptr, "controller\\player_6\\Motion\\Left", &osSettings.players[6].motions[(size_t)NativeMotionValues::MotionLeft], ""},
        {nullptr, "controller\\player_6\\Motion\\Right", &osSettings.players[6].motions[(size_t)NativeMotionValues::MotionRight], ""},
        {NXOsSetting::Player6VibrationEnabled, "controller\\player_6\\Vibration\\Enabled", &osSettings.players[6].vibration_enabled, true},
        {NXOsSetting::Player6VibrationStrength, "controller\\player_6\\Vibration\\Strength", &osSettings.players[6].vibration_strength, 100},
        {nullptr, "controller\\player_6\\BodyColor\\Left", &osSettings.players[6].body_color_left, 0},
        {nullptr, "controller\\player_6\\BodyColor\\Right", &osSettings.players[6].body_color_right, 0},
        {nullptr, "controller\\player_6\\ButtonColor\\Left", &osSettings.players[6].button_color_left, 0},
        {nullptr, "controller\\player_6\\ButtonColor\\Right", &osSettings.players[6].button_color_right, 0},
        {nullptr, "controller\\player_6\\ProfileName", &osSettings.players[6].profile_name, ""},
        {nullptr, "controller\\player_6\\Vibration\\UseSystem", &osSettings.players[6].use_system_vibrator, false},

        {nullptr, "controller\\player_7\\Connected", &osSettings.players[7].connected, false},
        {nullptr, "controller\\player_7\\ControllerType", &osSettings.players[7].controller_type, ControllerType::ProController},
        {nullptr, "controller\\player_7\\Button\\A", &osSettings.players[7].buttons[(size_t)NativeButtonValues::A], ""},
        {nullptr, "controller\\player_7\\Button\\B", &osSettings.players[7].buttons[(size_t)NativeButtonValues::B], ""},
        {nullptr, "controller\\player_7\\Button\\X", &osSettings.players[7].buttons[(size_t)NativeButtonValues::X], ""},
        {nullptr, "controller\\player_7\\Button\\Y", &osSettings.players[7].buttons[(size_t)NativeButtonValues::Y], ""},
        {nullptr, "controller\\player_7\\Button\\LStick", &osSettings.players[7].buttons[(size_t)NativeButtonValues::LStick], ""},
        {nullptr, "controller\\player_7\\Button\\RStick", &osSettings.players[7].buttons[(size_t)NativeButtonValues::RStick], ""},
        {nullptr, "controller\\player_7\\Button\\L", &osSettings.players[7].buttons[(size_t)NativeButtonValues::L], ""},
        {nullptr, "controller\\player_7\\Button\\R", &osSettings.players[7].buttons[(size_t)NativeButtonValues::R], ""},
        {nullptr, "controller\\player_7\\Button\\ZL", &osSettings.players[7].buttons[(size_t)NativeButtonValues::ZL], ""},
        {nullptr, "controller\\player_7\\Button\\ZR", &osSettings.players[7].buttons[(size_t)NativeButtonValues::ZR], ""},
        {nullptr, "controller\\player_7\\Button\\Plus", &osSettings.players[7].buttons[(size_t)NativeButtonValues::Plus], ""},
        {nullptr, "controller\\player_7\\Button\\Minus", &osSettings.players[7].buttons[(size_t)NativeButtonValues::Minus], ""},
        {nullptr, "controller\\player_7\\Button\\DLeft", &osSettings.players[7].buttons[(size_t)NativeButtonValues::DLeft], ""},
        {nullptr, "controller\\player_7\\Button\\DUp", &osSettings.players[7].buttons[(size_t)NativeButtonValues::DUp], ""},
        {nullptr, "controller\\player_7\\Button\\DRight", &osSettings.players[7].buttons[(size_t)NativeButtonValues::DRight], ""},
        {nullptr, "controller\\player_7\\Button\\DDown", &osSettings.players[7].buttons[(size_t)NativeButtonValues::DDown], ""},
        {nullptr, "controller\\player_7\\Button\\SLLeft", &osSettings.players[7].buttons[(size_t)NativeButtonValues::SLLeft], ""},
        {nullptr, "controller\\player_7\\Button\\SRLeft", &osSettings.players[7].buttons[(size_t)NativeButtonValues::SRLeft], ""},
        {nullptr, "controller\\player_7\\Button\\Home", &osSettings.players[7].buttons[(size_t)NativeButtonValues::Home], ""},
        {nullptr, "controller\\player_7\\Button\\Screenshot", &osSettings.players[7].buttons[(size_t)NativeButtonValues::Screenshot], ""},
        {nullptr, "controller\\player_7\\Button\\SLRight", &osSettings.players[7].buttons[(size_t)NativeButtonValues::SLRight], ""},
        {nullptr, "controller\\player_7\\Button\\SRRight", &osSettings.players[7].buttons[(size_t)NativeButtonValues::SRRight], ""},
        {nullptr, "controller\\player_7\\Analog\\LStick", &osSettings.players[7].analogs[(size_t)NativeAnalogValues::LStick], ""},
        {nullptr, "controller\\player_7\\Analog\\RStick", &osSettings.players[7].analogs[(size_t)NativeAnalogValues::RStick], ""},
        {nullptr, "controller\\player_7\\Motion\\Left", &osSettings.players[7].motions[(size_t)NativeMotionValues::MotionLeft], ""},
        {nullptr, "controller\\player_7\\Motion\\Right", &osSettings.players[7].motions[(size_t)NativeMotionValues::MotionRight], ""},
        {NXOsSetting::Player7VibrationEnabled, "controller\\player_7\\Vibration\\Enabled", &osSettings.players[7].vibration_enabled, true},
        {NXOsSetting::Player7VibrationStrength, "controller\\player_7\\Vibration\\Strength", &osSettings.players[7].vibration_strength, 100},
        {nullptr, "controller\\player_7\\BodyColor\\Left", &osSettings.players[7].body_color_left, 0},
        {nullptr, "controller\\player_7\\BodyColor\\Right", &osSettings.players[7].body_color_right, 0},
        {nullptr, "controller\\player_7\\ButtonColor\\Left", &osSettings.players[7].button_color_left, 0},
        {nullptr, "controller\\player_7\\ButtonColor\\Right", &osSettings.players[7].button_color_right, 0},
        {nullptr, "controller\\player_7\\ProfileName", &osSettings.players[7].profile_name, ""},
        {nullptr, "controller\\player_7\\Vibration\\UseSystem", &osSettings.players[7].use_system_vibrator, false},

        {nullptr, "controller\\player_8\\Connected", &osSettings.players[8].connected, false},
        {nullptr, "controller\\player_8\\ControllerType", &osSettings.players[8].controller_type, ControllerType::ProController},
        {nullptr, "controller\\player_8\\Button\\A", &osSettings.players[8].buttons[(size_t)NativeButtonValues::A], ""},
        {nullptr, "controller\\player_8\\Button\\B", &osSettings.players[8].buttons[(size_t)NativeButtonValues::B], ""},
        {nullptr, "controller\\player_8\\Button\\X", &osSettings.players[8].buttons[(size_t)NativeButtonValues::X], ""},
        {nullptr, "controller\\player_8\\Button\\Y", &osSettings.players[8].buttons[(size_t)NativeButtonValues::Y], ""},
        {nullptr, "controller\\player_8\\Button\\LStick", &osSettings.players[8].buttons[(size_t)NativeButtonValues::LStick], ""},
        {nullptr, "controller\\player_8\\Button\\RStick", &osSettings.players[8].buttons[(size_t)NativeButtonValues::RStick], ""},
        {nullptr, "controller\\player_8\\Button\\L", &osSettings.players[8].buttons[(size_t)NativeButtonValues::L], ""},
        {nullptr, "controller\\player_8\\Button\\R", &osSettings.players[8].buttons[(size_t)NativeButtonValues::R], ""},
        {nullptr, "controller\\player_8\\Button\\ZL", &osSettings.players[8].buttons[(size_t)NativeButtonValues::ZL], ""},
        {nullptr, "controller\\player_8\\Button\\ZR", &osSettings.players[8].buttons[(size_t)NativeButtonValues::ZR], ""},
        {nullptr, "controller\\player_8\\Button\\Plus", &osSettings.players[8].buttons[(size_t)NativeButtonValues::Plus], ""},
        {nullptr, "controller\\player_8\\Button\\Minus", &osSettings.players[8].buttons[(size_t)NativeButtonValues::Minus], ""},
        {nullptr, "controller\\player_8\\Button\\DLeft", &osSettings.players[8].buttons[(size_t)NativeButtonValues::DLeft], ""},
        {nullptr, "controller\\player_8\\Button\\DUp", &osSettings.players[8].buttons[(size_t)NativeButtonValues::DUp], ""},
        {nullptr, "controller\\player_8\\Button\\DRight", &osSettings.players[8].buttons[(size_t)NativeButtonValues::DRight], ""},
        {nullptr, "controller\\player_8\\Button\\DDown", &osSettings.players[8].buttons[(size_t)NativeButtonValues::DDown], ""},
        {nullptr, "controller\\player_8\\Button\\SLLeft", &osSettings.players[8].buttons[(size_t)NativeButtonValues::SLLeft], ""},
        {nullptr, "controller\\player_8\\Button\\SRLeft", &osSettings.players[8].buttons[(size_t)NativeButtonValues::SRLeft], ""},
        {nullptr, "controller\\player_8\\Button\\Home", &osSettings.players[8].buttons[(size_t)NativeButtonValues::Home], ""},
        {nullptr, "controller\\player_8\\Button\\Screenshot", &osSettings.players[8].buttons[(size_t)NativeButtonValues::Screenshot], ""},
        {nullptr, "controller\\player_8\\Button\\SLRight", &osSettings.players[8].buttons[(size_t)NativeButtonValues::SLRight], ""},
        {nullptr, "controller\\player_8\\Button\\SRRight", &osSettings.players[8].buttons[(size_t)NativeButtonValues::SRRight], ""},
        {nullptr, "controller\\player_8\\Analog\\LStick", &osSettings.players[8].analogs[(size_t)NativeAnalogValues::LStick], ""},
        {nullptr, "controller\\player_8\\Analog\\RStick", &osSettings.players[8].analogs[(size_t)NativeAnalogValues::RStick], ""},
        {nullptr, "controller\\player_8\\Motion\\Left", &osSettings.players[8].motions[(size_t)NativeMotionValues::MotionLeft], ""},
        {nullptr, "controller\\player_8\\Motion\\Right", &osSettings.players[8].motions[(size_t)NativeMotionValues::MotionRight], ""},
        {nullptr, "controller\\player_8\\Vibration\\Enabled", &osSettings.players[8].vibration_enabled, true},
        {nullptr, "controller\\player_8\\Vibration\\Strength", &osSettings.players[8].vibration_strength, 100},
        {nullptr, "controller\\player_8\\BodyColor\\Left", &osSettings.players[8].body_color_left, 0},
        {nullptr, "controller\\player_8\\BodyColor\\Right", &osSettings.players[8].body_color_right, 0},
        {nullptr, "controller\\player_8\\ButtonColor\\Left", &osSettings.players[8].button_color_left, 0},
        {nullptr, "controller\\player_8\\ButtonColor\\Right", &osSettings.players[8].button_color_right, 0},
        {nullptr, "controller\\player_8\\ProfileName", &osSettings.players[8].profile_name, ""},
        {nullptr, "controller\\player_8\\Vibration\\UseSystem", &osSettings.players[8].use_system_vibrator, false},

        {nullptr, "controller\\player_9\\Connected", &osSettings.players[9].connected, false},
        {nullptr, "controller\\player_9\\ControllerType", &osSettings.players[9].controller_type, ControllerType::ProController},
        {nullptr, "controller\\player_9\\Button\\A", &osSettings.players[9].buttons[(size_t)NativeButtonValues::A], ""},
        {nullptr, "controller\\player_9\\Button\\B", &osSettings.players[9].buttons[(size_t)NativeButtonValues::B], ""},
        {nullptr, "controller\\player_9\\Button\\X", &osSettings.players[9].buttons[(size_t)NativeButtonValues::X], ""},
        {nullptr, "controller\\player_9\\Button\\Y", &osSettings.players[9].buttons[(size_t)NativeButtonValues::Y], ""},
        {nullptr, "controller\\player_9\\Button\\LStick", &osSettings.players[9].buttons[(size_t)NativeButtonValues::LStick], ""},
        {nullptr, "controller\\player_9\\Button\\RStick", &osSettings.players[9].buttons[(size_t)NativeButtonValues::RStick], ""},
        {nullptr, "controller\\player_9\\Button\\L", &osSettings.players[9].buttons[(size_t)NativeButtonValues::L], ""},
        {nullptr, "controller\\player_9\\Button\\R", &osSettings.players[9].buttons[(size_t)NativeButtonValues::R], ""},
        {nullptr, "controller\\player_9\\Button\\ZL", &osSettings.players[9].buttons[(size_t)NativeButtonValues::ZL], ""},
        {nullptr, "controller\\player_9\\Button\\ZR", &osSettings.players[9].buttons[(size_t)NativeButtonValues::ZR], ""},
        {nullptr, "controller\\player_9\\Button\\Plus", &osSettings.players[9].buttons[(size_t)NativeButtonValues::Plus], ""},
        {nullptr, "controller\\player_9\\Button\\Minus", &osSettings.players[9].buttons[(size_t)NativeButtonValues::Minus], ""},
        {nullptr, "controller\\player_9\\Button\\DLeft", &osSettings.players[9].buttons[(size_t)NativeButtonValues::DLeft], ""},
        {nullptr, "controller\\player_9\\Button\\DUp", &osSettings.players[9].buttons[(size_t)NativeButtonValues::DUp], ""},
        {nullptr, "controller\\player_9\\Button\\DRight", &osSettings.players[9].buttons[(size_t)NativeButtonValues::DRight], ""},
        {nullptr, "controller\\player_9\\Button\\DDown", &osSettings.players[9].buttons[(size_t)NativeButtonValues::DDown], ""},
        {nullptr, "controller\\player_9\\Button\\SLLeft", &osSettings.players[9].buttons[(size_t)NativeButtonValues::SLLeft], ""},
        {nullptr, "controller\\player_9\\Button\\SRLeft", &osSettings.players[9].buttons[(size_t)NativeButtonValues::SRLeft], ""},
        {nullptr, "controller\\player_9\\Button\\Home", &osSettings.players[9].buttons[(size_t)NativeButtonValues::Home], ""},
        {nullptr, "controller\\player_9\\Button\\Screenshot", &osSettings.players[9].buttons[(size_t)NativeButtonValues::Screenshot], ""},
        {nullptr, "controller\\player_9\\Button\\SLRight", &osSettings.players[9].buttons[(size_t)NativeButtonValues::SLRight], ""},
        {nullptr, "controller\\player_9\\Button\\SRRight", &osSettings.players[9].buttons[(size_t)NativeButtonValues::SRRight], ""},
        {nullptr, "controller\\player_9\\Analog\\LStick", &osSettings.players[9].analogs[(size_t)NativeAnalogValues::LStick], ""},
        {nullptr, "controller\\player_9\\Analog\\RStick", &osSettings.players[9].analogs[(size_t)NativeAnalogValues::RStick], ""},
        {nullptr, "controller\\player_9\\Motion\\Left", &osSettings.players[9].motions[(size_t)NativeMotionValues::MotionLeft], ""},
        {nullptr, "controller\\player_9\\Motion\\Right", &osSettings.players[9].motions[(size_t)NativeMotionValues::MotionRight], ""},
        {nullptr, "controller\\player_9\\Vibration\\Enabled", &osSettings.players[9].vibration_enabled, true},
        {nullptr, "controller\\player_9\\Vibration\\Strength", &osSettings.players[9].vibration_strength, 100},
        {nullptr, "controller\\player_9\\BodyColor\\Left", &osSettings.players[9].body_color_left, 0},
        {nullptr, "controller\\player_9\\BodyColor\\Right", &osSettings.players[9].body_color_right, 0},
        {nullptr, "controller\\player_9\\ButtonColor\\Left", &osSettings.players[9].button_color_left, 0},
        {nullptr, "controller\\player_9\\ButtonColor\\Right", &osSettings.players[9].button_color_right, 0},
        {nullptr, "controller\\player_9\\ProfileName", &osSettings.players[9].profile_name, ""},
        {nullptr, "controller\\player_9\\Vibration\\UseSystem", &osSettings.players[9].use_system_vibrator, false},

        {NXOsSetting::VibrationEnabled, "controller\\vibration_enabled", &osSettings.vibration_enabled, true},
        {NXOsSetting::EnableAccurateVibrations, "controller\\enable_accurate_vibrations", &osSettings.enable_accurate_vibrations, false},
        {NXOsSetting::AudioSinkId, "audio\\sink_id", &osSettings.sink_id, AudioCore::Sink::AudioEngine::Auto},
        {NXOsSetting::AudioOutputDeviceId, "audio\\output_device_id", &osSettings.audio_output_device_id, "auto"},
        {NXOsSetting::AudioInputDeviceId, "audio\\input_device_id", &osSettings.audio_input_device_id, "auto"},
        {NXOsSetting::AudioMode, "audio\\mode", &osSettings.sound_index, AudioMode::Stereo},
        {NXOsSetting::AudioVolume, "audio\\volume", &osSettings.volume, 100, 0, 200},
        {NXOsSetting::AudioMuted, "audio\\muted", &osSettings.audio_muted, false},
        {NXOsSetting::SpeedLimit, "system\\speed_limit", &osSettings.speed_limit, 100, 0, 9999},
        {NXOsSetting::UseMultiCore, "system\\use_multi_core", &osSettings.use_multi_core, true},
        {NXOsSetting::UseSpeedLimit, nullptr, &osSettings.use_speed_limit, true},
        {NXOsSetting::MemoryLayout, "system\\memory_layout_mode", &osSettings.memory_layout_mode, MemoryLayout::Memory_4Gb},
        {NXOsSetting::LanguageIndex, "system\\language_index", &osSettings.language_index, Language::EnglishAmerican},
        {NXOsSetting::CurrentUser, "system\\current_user", &osSettings.current_user, 0},
        {NXOsSetting::RngSeedEnabled, "system\\rng_seed_enabled", &osSettings.rng_seed_enabled, false},
        {NXOsSetting::RngSeed, "system\\rng_seed", &osSettings.rng_seed, 0u},
        {NXOsSetting::CustomRtcEnabled, "system\\custom_rtc_enabled", &osSettings.custom_rtc_enabled, false},
        {NXOsSetting::CustomRtcOffset, "system\\custom_rtc_offset", &osSettings.custom_rtc_offset, 0, INT_MIN, INT_MAX},
#ifdef ANDROID
        {NXOsSetting::DockedMode, "system\\docked_mode", &osSettings.use_docked_mode, DockedMode::Handheld},
#else
        {NXOsSetting::DockedMode, "system\\docked_mode", &osSettings.use_docked_mode, DockedMode::Docked},
#endif
    };

void ApplyRangedInt(OsSetting & osSetting, int32_t value)
{
    const int32_t clamped = std::clamp(value, osSetting.minValue, osSetting.maxValue);
    if (osSetting.rangedWidth == RangedWidth::U8)
    {
        *osSetting.value.u8Value = static_cast<u8>(clamped);
    }
    else
    {
        *osSetting.value.u16Value = static_cast<u16>(clamped);
    }
}

void ApplyRangedS64(OsSetting & osSetting, s64 value)
{
    *osSetting.value.s64Value = std::clamp(value, osSetting.minValueS64, osSetting.maxValueS64);
}

} // namespace

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
        case SettingType::Boolean:
            *osSetting.value.boolValue = g_settings->GetBool(setting);
            break;
        case SettingType::IntValue:
            *osSetting.value.intValue = g_settings->GetInt(setting);
            break;
        case SettingType::IntValueRanged:
            ApplyRangedInt(const_cast<OsSetting &>(osSetting), g_settings->GetInt(setting));
            break;
        case SettingType::S32:
            *osSetting.value.s32Value = g_settings->GetInt(setting);
            break;
        case SettingType::S64Ranged:
            ApplyRangedS64(const_cast<OsSetting &>(osSetting), static_cast<s64>(g_settings->GetInt(setting)));
            break;
        case SettingType::U32:
            *osSetting.value.u32Value = static_cast<u32>(g_settings->GetInt(setting));
            break;
        case SettingType::Float:
            *osSetting.value.floatValue = g_settings->GetFloat(setting);
            break;
        case SettingType::String:
            *osSetting.value.stringValue = g_settings->GetString(setting);
            break;
        case SettingType::ControllerType:
            *osSetting.value.controllerType = static_cast<ControllerType>(g_settings->GetInt(setting));
            break;
        case SettingType::AudioEngine:
            *osSetting.value.audioEngine = static_cast<AudioCore::Sink::AudioEngine>(g_settings->GetInt(setting));
            break;
        case SettingType::AudioMode:
            *osSetting.value.audioMode = static_cast<AudioMode>(g_settings->GetInt(setting));
            break;
        case SettingType::Language:
            *osSetting.value.language = static_cast<Language>(g_settings->GetInt(setting));
            break;
        case SettingType::DockedMode:
            *osSetting.value.dockedMode = static_cast<DockedMode>(g_settings->GetInt(setting));
            break;
        case SettingType::MemoryLayout:
            *osSetting.value.memoryLayout = static_cast<MemoryLayout>(g_settings->GetInt(setting));
            break;
        default:
            UNIMPLEMENTED();
        }
    }
}

namespace
{

void InitializeOsSettingDefaults()
{
    osSettings.cabinet_applet_mode = AppletMode::LLE;
    osSettings.controller_applet_mode = AppletMode::HLE;
    osSettings.data_erase_applet_mode = AppletMode::HLE;
    osSettings.error_applet_mode = AppletMode::LLE;
    osSettings.net_connect_applet_mode = AppletMode::HLE;
    osSettings.player_select_applet_mode = AppletMode::HLE;
    osSettings.swkbd_applet_mode = AppletMode::LLE;
    osSettings.mii_edit_applet_mode = AppletMode::LLE;
    osSettings.web_applet_mode = AppletMode::HLE;
    osSettings.shop_applet_mode = AppletMode::HLE;
    osSettings.photo_viewer_applet_mode = AppletMode::LLE;
    osSettings.offline_web_applet_mode = AppletMode::LLE;
    osSettings.login_share_applet_mode = AppletMode::HLE;
    osSettings.wifi_web_auth_applet_mode = AppletMode::HLE;
    osSettings.my_page_applet_mode = AppletMode::LLE;

    osSettings.dump_audio_commands = false;
    osSettings.region_index = Region::Usa;
    osSettings.time_zone_index = TimeZone::Auto;
    osSettings.custom_rtc = 0;
    osSettings.device_name = "NxEmu";
    osSettings.enable_gamemode = true;
    osSettings.enable_raw_input = false;
    osSettings.controller_navigation = true;
    osSettings.enable_joycon_driver = true;
    osSettings.enable_procon_driver = false;
    osSettings.vibration_enabled = true;
    osSettings.enable_accurate_vibrations = false;
    osSettings.motion_enabled = true;
    osSettings.udp_input_servers = "127.0.0.1:26760";
    osSettings.enable_udp_controller = false;
    osSettings.pause_tas_on_load = true;
    osSettings.tas_enable = false;
    osSettings.tas_loop = false;
    osSettings.mouse_panning = false;
    osSettings.mouse_panning_sensitivity = 50;
    osSettings.mouse_enabled = false;
    osSettings.mouse_panning_x_sensitivity = 50;
    osSettings.mouse_panning_y_sensitivity = 50;
    osSettings.mouse_panning_deadzone_counterweight = 20;
    osSettings.mouse_panning_decay_strength = 18;
    osSettings.mouse_panning_min_decay = 6;
    osSettings.emulate_analog_keyboard = false;
    osSettings.keyboard_enabled = false;
    osSettings.debug_pad_enabled = false;
    osSettings.touch_device = "min_x:100,min_y:50,max_x:1800,max_y:850";
    osSettings.touch_from_button_map_index = 0;
    osSettings.enable_ring_controller = true;
    osSettings.enable_ir_sensor = false;
    osSettings.ir_sensor_device = "auto";
    osSettings.random_amiibo_id = false;
}

} // namespace

void SetupOsSetting(void)
{
    InitializeOsSettingDefaults();

    for (const OsSetting & osSetting : settings)
    {
        switch (osSetting.settingType)
        {
        case SettingType::Boolean:
            *osSetting.value.boolValue = osSetting.defaults.boolValue;
            break;
        case SettingType::IntValue:
            *osSetting.value.intValue = osSetting.defaults.intValue;
            break;
        case SettingType::IntValueRanged:
            ApplyRangedInt(const_cast<OsSetting &>(osSetting), osSetting.defaults.intValue);
            break;
        case SettingType::S32:
            *osSetting.value.s32Value = osSetting.defaults.s32Value;
            break;
        case SettingType::S64Ranged:
            ApplyRangedS64(const_cast<OsSetting &>(osSetting), osSetting.defaults.s64Value);
            break;
        case SettingType::U32:
            *osSetting.value.u32Value = osSetting.defaults.u32Value;
            break;
        case SettingType::Float:
            *osSetting.value.floatValue = osSetting.defaults.floatValue;
            break;
        case SettingType::String:
            *osSetting.value.stringValue = osSetting.defaults.stringValue;
            break;
        case SettingType::ControllerType:
            *osSetting.value.controllerType = osSetting.defaults.controllerType;
            break;
        case SettingType::AudioEngine:
            *osSetting.value.audioEngine = osSetting.defaults.audioEngine;
            break;
        case SettingType::AudioMode:
            *osSetting.value.audioMode = osSetting.defaults.audioMode;
            break;
        case SettingType::Language:
            *osSetting.value.language = osSetting.defaults.language;
            break;
        case SettingType::DockedMode:
            *osSetting.value.dockedMode = osSetting.defaults.dockedMode;
            break;
        case SettingType::MemoryLayout:
            *osSetting.value.memoryLayout = osSetting.defaults.memoryLayout;
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
            if (osSetting.json_path == nullptr)
            {
                continue;
            }
            JsonValue value = JsonGetNestedValue(root, osSetting.json_path);
            switch (osSetting.settingType)
            {
            case SettingType::Boolean:
                if (value.isBool())
                {
                    *osSetting.value.boolValue = value.asBool();
                }
                break;
            case SettingType::IntValue:
                if (value.isInt())
                {
                    *osSetting.value.intValue = static_cast<int32_t>(value.asInt64());
                }
                break;
            case SettingType::IntValueRanged:
                if (value.isInt())
                {
                    ApplyRangedInt(const_cast<OsSetting &>(osSetting), static_cast<int32_t>(value.asInt64()));
                }
                break;
            case SettingType::S32:
                if (value.isInt())
                {
                    *osSetting.value.s32Value = static_cast<s32>(value.asInt64());
                }
                break;
            case SettingType::S64Ranged:
                if (value.isInt())
                {
                    ApplyRangedS64(const_cast<OsSetting &>(osSetting), static_cast<s64>(value.asInt64()));
                }
                break;
            case SettingType::U32:
                if (value.isInt())
                {
                    *osSetting.value.u32Value = static_cast<u32>(value.asUInt64());
                }
                break;
            case SettingType::Float:
                if (value.isDouble())
                {
                    *osSetting.value.floatValue = static_cast<float>(value.asDouble());
                }
                else if (value.isInt())
                {
                    *osSetting.value.floatValue = static_cast<float>(value.asInt64());
                }
                break;
            case SettingType::String:
                if (value.isString())
                {
                    *osSetting.value.stringValue = value.asString();
                }
                break;
            case SettingType::ControllerType:
                if (value.isString())
                {
                    *osSetting.value.controllerType = ControllerTypeFromString(value.asString());
                }
                break;
            case SettingType::AudioEngine:
                if (value.isString())
                {
                    *osSetting.value.audioEngine = AudioEngineFromString(value.asString());
                }
                break;
            case SettingType::AudioMode:
                if (value.isString())
                {
                    *osSetting.value.audioMode = AudioModeFromString(value.asString());
                }
                break;
            case SettingType::Language:
                if (value.isString())
                {
                    *osSetting.value.language = LanguageFromString(value.asString());
                }
                break;
            case SettingType::DockedMode:
                if (value.isString())
                {
                    *osSetting.value.dockedMode = DockedModeFromString(value.asString());
                }
                break;
            case SettingType::MemoryLayout:
                if (value.isString())
                {
                    *osSetting.value.memoryLayout = MemoryLayoutFromString(value.asString());
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
        case SettingType::Boolean:
            g_settings->SetDefaultBool(osSetting.identifier, osSetting.defaults.boolValue);
            g_settings->SetBool(osSetting.identifier, *osSetting.value.boolValue);
            break;
        case SettingType::IntValue:
            g_settings->SetDefaultInt(osSetting.identifier, osSetting.defaults.intValue);
            g_settings->SetInt(osSetting.identifier, *osSetting.value.intValue);
            break;
        case SettingType::IntValueRanged:
            g_settings->SetDefaultInt(osSetting.identifier, osSetting.defaults.intValue);
            ApplyRangedInt(const_cast<OsSetting &>(osSetting),
                             osSetting.rangedWidth == RangedWidth::U8 ? static_cast<int32_t>(*osSetting.value.u8Value)
                                                                      : static_cast<int32_t>(*osSetting.value.u16Value));
            g_settings->SetInt(osSetting.identifier,
                               osSetting.rangedWidth == RangedWidth::U8 ? static_cast<int32_t>(*osSetting.value.u8Value)
                                                                       : static_cast<int32_t>(*osSetting.value.u16Value));
            break;
        case SettingType::S32:
            g_settings->SetDefaultInt(osSetting.identifier, osSetting.defaults.s32Value);
            g_settings->SetInt(osSetting.identifier, *osSetting.value.s32Value);
            break;
        case SettingType::S64Ranged:
            g_settings->SetDefaultInt(osSetting.identifier, static_cast<int32_t>(osSetting.defaults.s64Value));
            ApplyRangedS64(const_cast<OsSetting &>(osSetting), *osSetting.value.s64Value);
            g_settings->SetInt(osSetting.identifier, static_cast<int32_t>(*osSetting.value.s64Value));
            break;
        case SettingType::U32:
            g_settings->SetDefaultInt(osSetting.identifier, static_cast<int32_t>(osSetting.defaults.u32Value));
            g_settings->SetInt(osSetting.identifier, static_cast<int32_t>(*osSetting.value.u32Value));
            break;
        case SettingType::Float:
            g_settings->SetDefaultFloat(osSetting.identifier, osSetting.defaults.floatValue);
            g_settings->SetFloat(osSetting.identifier, *osSetting.value.floatValue);
            break;
        case SettingType::String:
            g_settings->SetDefaultString(osSetting.identifier, osSetting.defaults.stringValue);
            g_settings->SetString(osSetting.identifier, osSetting.value.stringValue->c_str());
            break;
        case SettingType::ControllerType:
            g_settings->SetDefaultInt(osSetting.identifier, static_cast<int32_t>(osSetting.defaults.controllerType));
            g_settings->SetInt(osSetting.identifier, static_cast<int32_t>(*osSetting.value.controllerType));
            break;
        case SettingType::AudioEngine:
            g_settings->SetDefaultInt(osSetting.identifier, static_cast<int32_t>(osSetting.defaults.audioEngine));
            g_settings->SetInt(osSetting.identifier, static_cast<int32_t>(*osSetting.value.audioEngine));
            break;
        case SettingType::AudioMode:
            g_settings->SetDefaultInt(osSetting.identifier, static_cast<int32_t>(osSetting.defaults.audioMode));
            g_settings->SetInt(osSetting.identifier, static_cast<int32_t>(*osSetting.value.audioMode));
            break;
        case SettingType::Language:
            g_settings->SetDefaultInt(osSetting.identifier, static_cast<int32_t>(osSetting.defaults.language));
            g_settings->SetInt(osSetting.identifier, static_cast<int32_t>(*osSetting.value.language));
            break;
        case SettingType::DockedMode:
            g_settings->SetDefaultInt(osSetting.identifier, static_cast<int32_t>(osSetting.defaults.dockedMode));
            g_settings->SetInt(osSetting.identifier, static_cast<int32_t>(*osSetting.value.dockedMode));
            break;
        case SettingType::MemoryLayout:
            g_settings->SetDefaultInt(osSetting.identifier, static_cast<int32_t>(osSetting.defaults.memoryLayout));
            g_settings->SetInt(osSetting.identifier, static_cast<int32_t>(*osSetting.value.memoryLayout));
            break;
        default:
            UNIMPLEMENTED();
        }
        g_settings->RegisterCallback(osSetting.identifier, OsSettingChanged, nullptr);
    }
}

void CleanupOsSetting(void)
{
    for (const OsSetting & osSetting : settings)
    {
        if (osSetting.identifier == nullptr)
        {
            continue;
        }
        g_settings->UnregisterCallback(osSetting.identifier, OsSettingChanged, nullptr);
    }
}

void SaveOsSettings(void)
{
    JsonValue root;

    for (const OsSetting & osSetting : settings)
    {
        if (osSetting.json_path == nullptr)
        {
            continue;
        }
        switch (osSetting.settingType)
        {
        case SettingType::Boolean:
            if (*osSetting.value.boolValue != osSetting.defaults.boolValue)
            {
                JsonSetNestedValue(root, osSetting.json_path, *osSetting.value.boolValue);
            }
            break;
        case SettingType::IntValue:
            if (*osSetting.value.intValue != osSetting.defaults.intValue)
            {
                JsonSetNestedValue(root, osSetting.json_path, *osSetting.value.intValue);
            }
            break;
        case SettingType::IntValueRanged:
            if ((osSetting.rangedWidth == RangedWidth::U8 && *osSetting.value.u8Value != static_cast<u8>(osSetting.defaults.intValue)) ||
                (osSetting.rangedWidth == RangedWidth::U16 && *osSetting.value.u16Value != static_cast<u16>(osSetting.defaults.intValue)))
            {
                JsonSetNestedValue(root, osSetting.json_path,
                                   osSetting.rangedWidth == RangedWidth::U8 ? static_cast<int32_t>(*osSetting.value.u8Value)
                                                                            : static_cast<int32_t>(*osSetting.value.u16Value));
            }
            break;
        case SettingType::S32:
            if (*osSetting.value.s32Value != osSetting.defaults.s32Value)
            {
                JsonSetNestedValue(root, osSetting.json_path, *osSetting.value.s32Value);
            }
            break;
        case SettingType::S64Ranged:
            if (*osSetting.value.s64Value != osSetting.defaults.s64Value)
            {
                JsonSetNestedValue(root, osSetting.json_path, static_cast<int32_t>(*osSetting.value.s64Value));
            }
            break;
        case SettingType::U32:
            if (*osSetting.value.u32Value != osSetting.defaults.u32Value)
            {
                JsonSetNestedValue(root, osSetting.json_path, *osSetting.value.u32Value);
            }
            break;
        case SettingType::Float:
            if (*osSetting.value.floatValue != osSetting.defaults.floatValue)
            {
                JsonSetNestedValue(root, osSetting.json_path, *osSetting.value.floatValue);
            }
            break;
        case SettingType::String:
            if (*osSetting.value.stringValue != osSetting.defaults.stringValue)
            {
                JsonSetNestedValue(root, osSetting.json_path, *osSetting.value.stringValue);
            }
            break;
        case SettingType::ControllerType:
            if (*osSetting.value.controllerType != osSetting.defaults.controllerType)
            {
                JsonSetNestedValue(root, osSetting.json_path, ControllerTypeToString(*osSetting.value.controllerType));
            }
            break;
        case SettingType::AudioEngine:
            if (*osSetting.value.audioEngine != osSetting.defaults.audioEngine)
            {
                JsonSetNestedValue(root, osSetting.json_path, AudioEngineToString(*osSetting.value.audioEngine));
            }
            break;
        case SettingType::AudioMode:
            if (*osSetting.value.audioMode != osSetting.defaults.audioMode)
            {
                JsonSetNestedValue(root, osSetting.json_path, AudioModeToString(*osSetting.value.audioMode));
            }
            break;
        case SettingType::Language:
            if (*osSetting.value.language != osSetting.defaults.language)
            {
                JsonSetNestedValue(root, osSetting.json_path, LanguageToString(*osSetting.value.language));
            }
            break;
        case SettingType::DockedMode:
            if (*osSetting.value.dockedMode != osSetting.defaults.dockedMode)
            {
                JsonSetNestedValue(root, osSetting.json_path, DockedModeToString(*osSetting.value.dockedMode));
            }
            break;
        case SettingType::MemoryLayout:
            if (*osSetting.value.memoryLayout != osSetting.defaults.memoryLayout)
            {
                JsonSetNestedValue(root, osSetting.json_path, MemoryLayoutToString(*osSetting.value.memoryLayout));
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
OsSetting::OsSetting(const char * id, const char * path, bool * val, bool defaultValue) :
    identifier(id),
    json_path(path),
    settingType(SettingType::Boolean)
{
    defaults.boolValue = defaultValue;
    value.boolValue = val;
}

OsSetting::OsSetting(const char * id, const char * path, float * val, float defaultValue) :
    identifier(id),
    json_path(path),
    settingType(SettingType::Float)
{
    defaults.floatValue = defaultValue;
    value.floatValue = val;
}

OsSetting::OsSetting(const char * id, const char * path, std::string * val, const char * defaultValue) :
    identifier(id),
    json_path(path),
    settingType(SettingType::String)
{
    defaults.stringValue = defaultValue;
    value.stringValue = val;
}

OsSetting::OsSetting(const char * id, const char * path, ControllerType * val, ControllerType defaultValue) :
    identifier(id),
    json_path(path),
    settingType(SettingType::ControllerType)
{
    defaults.controllerType = defaultValue;
    value.controllerType = val;
}

OsSetting::OsSetting(const char * id, const char * path, AudioCore::Sink::AudioEngine * val, AudioCore::Sink::AudioEngine defaultValue) :
    identifier(id),
    json_path(path),
    settingType(SettingType::AudioEngine)
{
    defaults.audioEngine = defaultValue;
    value.audioEngine = val;
}

OsSetting::OsSetting(const char * id, const char * path, AudioMode * val, AudioMode defaultValue) :
    identifier(id),
    json_path(path),
    settingType(SettingType::AudioMode)
{
    defaults.audioMode = defaultValue;
    value.audioMode = val;
}

OsSetting::OsSetting(const char * id, const char * path, Language * val, Language defaultValue) :
    identifier(id),
    json_path(path),
    settingType(SettingType::Language)
{
    defaults.language = defaultValue;
    value.language = val;
}

OsSetting::OsSetting(const char * id, const char * path, DockedMode * val, DockedMode defaultValue) :
    identifier(id),
    json_path(path),
    settingType(SettingType::DockedMode)
{
    defaults.dockedMode = defaultValue;
    value.dockedMode = val;
}

OsSetting::OsSetting(const char * id, const char * path, MemoryLayout * val, MemoryLayout defaultValue) :
    identifier(id),
    json_path(path),
    settingType(SettingType::MemoryLayout)
{
    defaults.memoryLayout = defaultValue;
    value.memoryLayout = val;
}

OsSetting::OsSetting(const char * id, const char * path, u8 * val, int32_t defaultValue, int32_t minValue, int32_t maxValue) :
    identifier(id),
    json_path(path),
    settingType(SettingType::IntValueRanged),
    rangedWidth(RangedWidth::U8),
    minValue(minValue),
    maxValue(maxValue)
{
    defaults.intValue = defaultValue;
    value.u8Value = val;
}

OsSetting::OsSetting(const char * id, const char * path, u16 * val, int32_t defaultValue, int32_t minValue, int32_t maxValue) :
    identifier(id),
    json_path(path),
    settingType(SettingType::IntValueRanged),
    rangedWidth(RangedWidth::U16),
    minValue(minValue),
    maxValue(maxValue)
{
    defaults.intValue = defaultValue;
    value.u16Value = val;
}

OsSetting::OsSetting(const char * id, const char * path, s32 * val, s32 defaultValue) :
    identifier(id),
    json_path(path),
    settingType(SettingType::S32)
{
    defaults.s32Value = defaultValue;
    value.s32Value = val;
}

OsSetting::OsSetting(const char * id, const char * path, u32 * val, u32 defaultValue) :
    identifier(id),
    json_path(path),
    settingType(SettingType::U32)
{
    defaults.u32Value = defaultValue;
    value.u32Value = val;
}

OsSetting::OsSetting(const char * id, const char * path, s64 * val, s64 defaultValue, s64 minValue, s64 maxValue) :
    identifier(id),
    json_path(path),
    settingType(SettingType::S64Ranged),
    minValueS64(minValue),
    maxValueS64(maxValue)
{
    defaults.s64Value = defaultValue;
    value.s64Value = val;
}
} // namespace