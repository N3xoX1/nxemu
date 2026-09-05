#include "ui_identifiers.h"
#include "ui_settings.h"
#include <common/json_util.h>
#include <cstring>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/notification.h>
#include <android/log.h>

namespace
{
    enum class SettingType
    {
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
        UISetting(const char * id, const char * key, Stringlist * value);

        const char * identifier;
        const char * json_key;
        SettingType settingType;
        union
        {
            Stringlist * string_list;
        } setting;
    };

    static UISetting settings[] = {
        {NXUISetting::GameDirectories, "GameDirectories", &uiSettings.gameDirectories},
    };

    void UISettingChanged(const char * setting, void * /*userData*/);

} // namespace

UISettings uiSettings = {};

void SetupUISetting()
{
    __android_log_print(ANDROID_LOG_INFO, "NxEmu", "SetupUISetting");
    SettingsStore & settingsStore = SettingsStore::GetInstance();

    uiSettings = {};
    for (const UISetting & setting : settings)
    {
        switch (setting.settingType)
        {
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
            case SettingType::StringList:
                settingsStore.SetDefaultString(setting.identifier, "");
                settingsStore.SetString(setting.identifier, setting.setting.string_list != nullptr ? SerializeStringList(*setting.setting.string_list).c_str() : "");
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
            __android_log_print(ANDROID_LOG_INFO, "NxEmu", "SaveUISetting::stringList");
            if (!setting.setting.string_list->empty())
            {
                JsonValue jsonList(JsonValueType::Array);
                for (const std::string & item : *(setting.setting.string_list))
                {
                    __android_log_print(ANDROID_LOG_INFO, "NxEmu", "item: %s", item.c_str());
                    jsonList.Append(JsonValue(item));
                }
                JsonSetNestedValue(json, setting.json_key, std::move(jsonList));
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
    UISetting::UISetting(const char * id, const char * key, Stringlist * value) :
        identifier(id),
        json_key(key),
        settingType(SettingType::StringList)
    {
        setting.string_list = value;
    }

    void UISettingChanged(const char * setting, void * /*userData*/)
    {
        __android_log_print(ANDROID_LOG_INFO, "NxEmu", "UISettingChanged: setting = %s", setting);
        SettingsStore & settingsStore = SettingsStore::GetInstance();

        for (const UISetting & uiSetting : settings)
        {
            if (uiSetting.identifier == nullptr || strcmp(uiSetting.identifier, setting) != 0)
            {
                continue;
            }
            switch (uiSetting.settingType)
            {
            case SettingType::StringList:
                if (uiSetting.setting.string_list != nullptr)
                {
                    uiSetting.setting.string_list->clear();
                    std::string json = settingsStore.GetString(setting);
                    __android_log_print(ANDROID_LOG_INFO, "NxEmu", "UISettingChanged: StringList json = %s", json.c_str());
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
            default:
                g_notify->BreakPoint(__FILE__, __LINE__);
            }
            break;
        }
    }
}