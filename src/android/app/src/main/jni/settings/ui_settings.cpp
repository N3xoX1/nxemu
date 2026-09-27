#include "ui_identifiers.h"
#include "ui_settings.h"
#include <common/json_util.h>
#include <cstring>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/notification.h>

namespace
{
    enum class SettingType
    {
        String,
        int32,
        Bool,
        StringList,
    };

    std::string SerializeStringList(const Stringlist & list)
    {
        JsonValue jsonArray(JsonValueType::Array);
        for (const std::string & item : list)
        {
            jsonArray.Append(JsonValue(item));
        }
        return JsonStyledWriter().write(jsonArray);
    }

    class UISetting
    {
    public:
        UISetting(const char * id, const char * key, std::string * value, const char * defaultValue);
        UISetting(const char * id, const char * key, bool * value, bool defaultValue);
        UISetting(const char * id, const char * key, int32_t * value, int32_t defaultValue);
        UISetting(const char * id, const char * key, ThemeMode * value, ThemeMode defaultValue);
        UISetting(const char * id, const char * key, Stringlist * value);

        const char * identifier;
        const char * json_key;
        SettingType settingType;
        union
        {
            std::string * string;
            bool * boolean;
            int32_t * int32;
            Stringlist * string_list;
        } setting;
        union
        {
            const char * default_string;
            const int32_t default_int32;
            const bool default_bool;
        };
    };

    static UISetting settings[] = {
        {NXUISetting::GameDirectories, "GameDirectories", &uiSettings.gameDirectories},
        {NXUISetting::ShowInputOverlay, "ShowInputOverlay", &uiSettings.showInputOverlay, true},
        {NXUISetting::OverlayScale, "OverlayScale", &uiSettings.overlayScale, 50},
        {NXUISetting::OverlayOpacity, "OverlayOpacity", &uiSettings.overlayOpacity, 100},
        {NXUISetting::OverlayControlData, "OverlayControlData", &uiSettings.overlayControlData, ""},
        {NXUISetting::ThemeMode, "ThemeMode", &uiSettings.themeMode, ThemeMode::FollowSystem},
        {NXUISetting::LockDrawer, "LockDrawer", &uiSettings.lockDrawer, false},
    };

    void UISettingChanged(const char * setting, void * /*userData*/);

} // namespace

UISettings uiSettings = {};

void SetupUISetting()
{
    SettingsStore & settingsStore = SettingsStore::GetInstance();

    uiSettings = {};
    for (const UISetting & setting : settings)
    {
        switch (setting.settingType)
        {
        case SettingType::String:
            *(setting.setting.string) = setting.default_string;
            break;
        case SettingType::Bool:
            *(setting.setting.boolean) = setting.default_bool;
            break;
        case SettingType::int32:
            *(setting.setting.int32) = setting.default_int32;
            break;
        case SettingType::StringList:
            *(setting.setting.string_list) = {};
            break;
        default:
            g_notify->BreakPoint(__FILE__, __LINE__);
        }
    }

    JsonValue section = SettingsStore::GetInstance().GetSettings("UI");
    for (const UISetting & setting : settings)
    {
        JsonValue value = JsonGetNestedValue(section, setting.json_key);
        switch (setting.settingType)
        {
        case SettingType::String:
            if (value.isString())
            {
                *(setting.setting.string) = value.asString();
            }
            break;
        case SettingType::int32:
            if (value.isInt())
            {
                *(setting.setting.int32) = (int32_t)value.asInt64();
            }
            break;
        case SettingType::Bool:
            if (value.isBool())
            {
                *(setting.setting.boolean) = value.asBool();
            }
            break;
        case SettingType::StringList:
            if (value.isArray())
            {
                for (uint32_t i = 0; i < value.size(); i++)
                {
                    if (!value[i].isString())
                    {
                        continue;
                    }
                    setting.setting.string_list->push_back(value[i].asString());
                }
            }
            break;
        default:
            g_notify->BreakPoint(__FILE__, __LINE__);
        }

        if (setting.identifier != nullptr)
        {
            switch (setting.settingType)
            {
            case SettingType::int32:
                settingsStore.SetDefaultInt(setting.identifier, setting.default_int32);
                settingsStore.SetInt(setting.identifier, setting.setting.int32 != nullptr ? *setting.setting.int32 : setting.default_int32);
                break;
            case SettingType::StringList:
                settingsStore.SetDefaultString(setting.identifier, "");
                settingsStore.SetString(setting.identifier, setting.setting.string_list != nullptr ? SerializeStringList(*setting.setting.string_list).c_str() : "");
                break;
            case SettingType::String:
                settingsStore.SetDefaultString(setting.identifier, setting.default_string);
                settingsStore.SetString(setting.identifier, setting.setting.string != nullptr ? setting.setting.string->c_str() : setting.default_string);
                break;
            case SettingType::Bool:
                settingsStore.SetDefaultBool(setting.identifier, setting.default_bool);
                settingsStore.SetBool(setting.identifier, setting.setting.boolean != nullptr ? *setting.setting.boolean : setting.default_bool);
                break;
            default:
                g_notify->BreakPoint(__FILE__, __LINE__);
            }

            settingsStore.RegisterCallback(setting.identifier, UISettingChanged, nullptr);
        }
    }
}

void SaveUISetting()
{
    JsonValue json;
    for (const UISetting & setting : settings)
    {
        switch (setting.settingType)
        {
        case SettingType::StringList:
            if (!setting.setting.string_list->empty())
            {
                JsonValue jsonList(JsonValueType::Array);
                for (const std::string & item : *(setting.setting.string_list))
                {
                    jsonList.Append(JsonValue(item));
                }
                JsonSetNestedValue(json, setting.json_key, std::move(jsonList));
            }
            break;
        case SettingType::String:
            if (strcmp(setting.setting.string->c_str(), setting.default_string) != 0)
            {
                JsonSetNestedValue(json, setting.json_key, JsonValue(*setting.setting.string));
            }
            break;
        case SettingType::Bool:
            if (*setting.setting.boolean != setting.default_bool)
            {
                JsonSetNestedValue(json, setting.json_key, JsonValue(*setting.setting.boolean));
            }
            break;
        case SettingType::int32:
            if (*setting.setting.int32 != setting.default_int32)
            {
                JsonSetNestedValue(json, setting.json_key, JsonValue(*setting.setting.int32));
            }
            break;
        default:
            g_notify->BreakPoint(__FILE__, __LINE__);
        }
    }

    SettingsStore & settingstore = SettingsStore::GetInstance();
    settingstore.SetSettings("UI", json);
    settingstore.Save();
}

namespace
{
    UISetting::UISetting(const char * id, const char * key, std::string * value, const char * defaultValue) :
        identifier(id),
        json_key(key),
        settingType(SettingType::String),
        default_string(defaultValue)
    {
        setting.string = value;
    }

    UISetting::UISetting(const char * id, const char * key, int32_t * value, int32_t defaultValue) :
        identifier(id),
        json_key(key),
        settingType(SettingType::int32),
        default_int32(defaultValue)
    {
        setting.int32 = value;
    }

    UISetting::UISetting(const char * id, const char * key, ThemeMode * value, ThemeMode defaultValue) :
        UISetting(id, key, (int32_t *)value, (int32_t)defaultValue)
    {
    }

    UISetting::UISetting(const char * id, const char * key, bool * value, bool defaultValue) :
        identifier(id),
        json_key(key),
        settingType(SettingType::Bool),
        default_bool(defaultValue)
    {
        setting.boolean = value;
    }

    UISetting::UISetting(const char * id, const char * key, Stringlist * value) :
        identifier(id),
        json_key(key),
        settingType(SettingType::StringList),
        default_string(nullptr)
    {
        setting.string_list = value;
    }

    void UISettingChanged(const char * setting, void * /*userData*/)
    {
        SettingsStore & settingsStore = SettingsStore::GetInstance();

        for (const UISetting & uiSetting : settings)
        {
            if (uiSetting.identifier == nullptr || strcmp(uiSetting.identifier, setting) != 0)
            {
                continue;
            }
            switch (uiSetting.settingType)
            {
            case SettingType::int32:
                if (uiSetting.setting.int32 != nullptr)
                {
                    *uiSetting.setting.int32 = settingsStore.GetInt(setting);
                }
                break;
            case SettingType::StringList:
                if (uiSetting.setting.string_list != nullptr)
                {
                    uiSetting.setting.string_list->clear();
                    std::string json = settingsStore.GetString(setting);
                    JsonValue root;
                    if (!json.empty())
                    {
                        JsonReader reader;
                        if (!reader.Parse(json.data(), json.data() + json.size(), root))
                        {
                            return;
                        }
                        if (root.isArray())
                        {
                            for (uint32_t i = 0, n = root.size(); i < n; i++)
                            {
                                uiSetting.setting.string_list->push_back(root[i].asString());
                            }
                        }
                    }
                }
                break;
            case SettingType::String:
                if (uiSetting.setting.string != nullptr)
                {
                    *uiSetting.setting.string = settingsStore.GetString(setting);
                }
                break;
            case SettingType::Bool:
                if (uiSetting.setting.boolean != nullptr)
                {
                    *uiSetting.setting.boolean = settingsStore.GetBool(setting);
                }
                break;
            default:
                g_notify->BreakPoint(__FILE__, __LINE__);
            }
            break;
        }
    }
}