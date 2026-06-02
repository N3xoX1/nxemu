#include "ui_settings.h"
#include "ui_identifiers.h"
#include <common/json.h>
#include <common/json_util.h>
#include <common/path.h>
#include <nxemu-core/notification.h>
#include <nxemu-core/settings/settings.h>
#include <sciter_handler.h>

namespace
{
    Path GetDefaultLanguageDir();

    const HotkeyMap g_defaultHotkeys = {
        {Hotkey::LoadFile, {.key = (uint32_t)SCITER_KEY_O, .ctrl = true}},
        {Hotkey::Exit, {.key = (uint32_t)SCITER_KEY_Q, .ctrl = true}},
        {Hotkey::Fullscreen, {.key = (uint32_t)SCITER_KEY_F11}},
        {Hotkey::ExitFullscreen, {.key = (uint32_t)SCITER_KEY_ESCAPE}},
        {Hotkey::HideUi, {.key = (uint32_t)SCITER_KEY_F4}},
        {Hotkey::PauseContinue, {.key = (uint32_t)SCITER_KEY_F2}},
        {Hotkey::ToggleDockedMode, {.key = (uint32_t)SCITER_KEY_F10}},
        {Hotkey::StopEmulation, {.key = (uint32_t)SCITER_KEY_F5}},
        {Hotkey::Controllers, {.key = (uint32_t)SCITER_KEY_C, .ctrl = true}},
        {Hotkey::Configure, {.key = (uint32_t)SCITER_KEY_T, .ctrl = true}},
    };

    void merge_default_hotkeys(HotkeyMap & map)
    {
        for (const HotkeyMap::value_type & def : g_defaultHotkeys)
        {
            if (map.find(def.first) == map.end())
            {
                map.insert(def);
            }
        }
    }

    enum class SettingType
    {
        Path,
        String,
        int32,
        Bool,
        StringList,
        Hotkeys,
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
        UISetting(const char * id, const char * key, Path * path, std::string * value, Path defaultPath, const char * defaultValue);
        UISetting(const char * id, const char * key, std::string * value, const char * defaultValue);
        UISetting(const char * id, const char * key, bool * value, bool defaultValue);
        UISetting(const char * id, const char * key, int32_t * value, uint32_t defaultValue);
        UISetting(const char * id, const char * key, Stringlist * value);
        UISetting(const char * id, const char * key, HotkeyMap * value, const HotkeyMap * defaultHotkeyMap);

        const char * identifier;
        const char * json_key;
        SettingType settingType;
        union
        {
            Path * path;
            std::string * string;
            bool * boolean;
            int32_t * int32;
            Stringlist * string_list;
            HotkeyMap * hotkey_map;
        } setting;
        std::string * settingValue;
        Path default_path;
        union
        {
            const char * default_string;
            const uint32_t default_int32;
            const bool default_bool;
            const HotkeyMap * default_hotKeys;
        };
    };

    static UISetting settings[] = {
        {NXUISetting::RecentFiles, "RecentFiles", &uiSettings.recentFiles},
        {NXUISetting::GameDirectories, "GameDirectories", &uiSettings.gameDirectories},
        {nullptr, "Language\\Directory", &uiSettings.languageDir, &uiSettings.languageDirValue, GetDefaultLanguageDir(), "./lang"},
        {nullptr, "Language\\Base", &uiSettings.languageBase, "english"},
        {nullptr, "Language\\Current", &uiSettings.languageCurrent, "english"},
        {nullptr, "SciterConsole", &uiSettings.sciterConsole, false},
        {NXUISetting::MyGameIconSize, "GameBrowser\\MyGameIconSize", &uiSettings.gameBrowserMyGameIconSize, 2},
        {nullptr, "PerformVulkanCheck", &uiSettings.performVulkanCheck, true},
        {nullptr, nullptr, &uiSettings.hasBrokenVulkan, false},
        {nullptr, nullptr, &uiSettings.enableAllControllers, false},
        {nullptr, "StartGamesInFullscreen", &uiSettings.startGamesInFullscreen, false},
        {NXUISetting::ConfirmBeforeStopping, "ConfirmBeforeStopping", &uiSettings.confirmBeforeStopping, true},
        {NXUISetting::Hotkeys, "Hotkeys", &uiSettings.hotkeys, &g_defaultHotkeys},
    };

    void UISettingChanged(const char * setting, void * /*userData*/);

} // namespace

UISettings uiSettings = {};

void SetupUISetting(void)
{
    SettingsStore & settingsStore = SettingsStore::GetInstance();

    uiSettings = {};
    for (const UISetting & setting : settings)
    {
        switch (setting.settingType)
        {
        case SettingType::Path:
            *(setting.settingValue) = setting.default_string;
            *(setting.setting.path) = setting.default_path;
            break;
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
        case SettingType::Hotkeys:
            *setting.setting.hotkey_map = setting.default_hotKeys != nullptr ? *setting.default_hotKeys : HotkeyMap{};
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
        case SettingType::Path:
            if (value.isString())
            {
                std::string dirValue = value.asString();
                if (!dirValue.empty())
                {
                    *(setting.settingValue) = dirValue;
                    *(setting.setting.path) = Path(*(setting.settingValue), "").DirectoryNormalize(Path(Path::MODULE_DIRECTORY));
                }
            }
            break;
        case SettingType::String:
            if (value.isString())
            {
                *(setting.setting.string) = value.asString();
            }
            break;
        case SettingType::int32:
            if (value.isInt())
            {
                *(setting.setting.int32) = (uint32_t)value.asUInt64();
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
        case SettingType::Hotkeys:
            if (value.isObject() && setting.setting.hotkey_map != nullptr)
            {
                HotkeyMap & map = *setting.setting.hotkey_map;
                for (const std::string & name : value.GetMemberNames())
                {
                    const JsonValue & v = value[name];
                    if (!v.isObject())
                    {
                        continue;
                    }

                    HotkeyMap::iterator itr = map.find(name);
                    if (itr == map.end())
                    {
                        continue;
                    }
                    MenuBarAccelerator & accel = itr->second;
                    if (v.isMember("key") && v["key"].isInt())
                    {
                        accel.key = (uint32_t)v["key"].asInt64();
                    }
                    if (v.isMember("ctrl") && v["ctrl"].isBool())
                    {
                        accel.ctrl = v["ctrl"].asBool();
                    }
                    if (v.isMember("alt") && v["alt"].isBool())
                    {
                        accel.alt = v["alt"].asBool();
                    }
                    if (v.isMember("shift") && v["shift"].isBool())
                    {
                        accel.shift = v["shift"].asBool();
                    }
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
                settingsStore.SetDefaultString(setting.identifier, setting.default_string);
                settingsStore.SetString(setting.identifier, setting.setting.string_list != nullptr ? SerializeStringList(*setting.setting.string_list).c_str() : setting.default_string);
                break;
            case SettingType::String:
                settingsStore.SetDefaultString(setting.identifier, setting.default_string);
                settingsStore.SetString(setting.identifier, setting.setting.string != nullptr ? setting.setting.string->c_str() : setting.default_string);
                break;
            case SettingType::Hotkeys:
                settingsStore.SetDefaultString(setting.identifier, setting.default_hotKeys != nullptr ? SerializeUIHotkeysMap(*setting.default_hotKeys).c_str() : "");
                settingsStore.SetString(setting.identifier, SerializeUIHotkeysMap(*setting.setting.hotkey_map).c_str());
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

void SaveUISetting(void)
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
        case SettingType::Path:
            if (strcmp(setting.settingValue->c_str(), setting.default_string) != 0)
            {
                JsonSetNestedValue(json, setting.json_key, JsonValue(*setting.settingValue));
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
            if (*setting.setting.int32 != static_cast<int32_t>(setting.default_int32))
            {
                JsonSetNestedValue(json, setting.json_key, JsonValue(*setting.setting.int32));
            }
            break;
        case SettingType::Hotkeys:
            if (setting.default_hotKeys != nullptr)
            {
                JsonValue diff = HotkeyMapToJsonObjectDiff(*setting.setting.hotkey_map, *setting.default_hotKeys);
                if (!diff.GetMemberNames().empty())
                {
                    JsonSetNestedValue(json, setting.json_key, std::move(diff));
                }
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
    Path GetDefaultLanguageDir()
    {
        Path dir(Path::MODULE_DIRECTORY);
        dir.AppendDirectory("lang");
        return dir;
    }

    UISetting::UISetting(const char * id, const char * key, Path * path, std::string * value, Path defaultPath, const char * defaultValue) :
        identifier(id),
        json_key(key),
        settingType(SettingType::Path),
        settingValue(value),
        default_path(defaultPath),
        default_string(defaultValue)
    {
        setting.path = path;
    }

    UISetting::UISetting(const char * id, const char * key, std::string * value, const char * defaultValue) :
        identifier(id),
        json_key(key),
        settingType(SettingType::String),
        settingValue(nullptr),
        default_string(defaultValue)
    {
        setting.string = value;
    }

    UISetting::UISetting(const char * id, const char * key, int32_t * value, uint32_t defaultValue) :
        identifier(id),
        json_key(key),
        settingType(SettingType::int32),
        settingValue(nullptr),
        default_int32(defaultValue)
    {
        setting.int32 = value;
    }

    UISetting::UISetting(const char * id, const char * key, bool * value, bool defaultValue) :
        identifier(id),
        json_key(key),
        settingType(SettingType::Bool),
        settingValue(nullptr),
        default_bool(defaultValue)
    {
        setting.boolean = value;
    }

    UISetting::UISetting(const char * id, const char * key, Stringlist * value) :
        identifier(id),
        json_key(key),
        settingType(SettingType::StringList),
        settingValue(nullptr),
        default_string(nullptr)
    {
        setting.string_list = value;
    }

    UISetting::UISetting(const char * id, const char * key, HotkeyMap * value, const HotkeyMap * defaultHotkeyMap) :
        identifier(id),
        json_key(key),
        settingType(SettingType::Hotkeys),
        settingValue(nullptr),
        default_hotKeys(defaultHotkeyMap)
    {
        setting.hotkey_map = value;
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
            case SettingType::Hotkeys:
                if (uiSetting.setting.hotkey_map != nullptr)
                {
                    const std::string json = settingsStore.GetString(setting);
                    JsonReader reader;
                    JsonValue root;
                    if (json.empty())
                    {
                        uiSetting.setting.hotkey_map->clear();
                    }
                    else if (reader.Parse(json.data(), json.data() + json.size(), root) && root.isObject())
                    {
                        DeserializeHotkeyMapFromJsonObject(root, *uiSetting.setting.hotkey_map);
                    }
                    else
                    {
                        uiSetting.setting.hotkey_map->clear();
                    }
                    merge_default_hotkeys(*uiSetting.setting.hotkey_map);
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