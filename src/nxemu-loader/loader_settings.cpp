#include "loader_settings.h"
#include "loader_settings_identifiers.h"
#include <common/json_util.h>
#include <nxemu-module-spec/base.h>
#include <yuzu_common/yuzu_assert.h>

extern IModuleSettings * g_settings;

LoaderSettings loaderSettings = {};

namespace
{
    using TitleIdStringListMap = std::map<uint64_t, std::vector<std::string>>;

    enum class SettingType
    {
        BooleanValue,
        IntegerValue,
        TitleIdStringListMapValue,
    };

    class LoaderSetting
    {
    public:
        LoaderSetting(const char * id, const char * path, bool * val, bool defaultValue);
        LoaderSetting(const char * id, const char * path, int * val, int defaultValue);
        LoaderSetting(const char * path, TitleIdStringListMap * val);

        const char * identifier;
        const char * json_path;
        SettingType settingType;
        union
        {
            bool * boolValue;
            int * intValue;
            TitleIdStringListMap * titleIdStringListMap;
        } setting;
        union
        {
            bool boolValue;
            int intValue;
        } defaultValue;
    };

    static LoaderSetting settings[] = {
        {NXLoaderSetting::CheckForUpdatedFirmware, "CheckForUpdatedFirmware", &loaderSettings.checkForUpdatedFirmware, true},
        {NXLoaderSetting::FirmwareInstallCurrent, nullptr, nullptr, 0},
        {NXLoaderSetting::FirmwareInstallTotal, nullptr, nullptr, 0},
        {"DisabledAddOns", &loaderSettings.disabled_addons},
    };

    std::string FormatTitleId(uint64_t program_id)
    {
        char buffer[17];
        std::snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(program_id));
        return buffer;
    }

    bool ParseTitleId(const std::string & key, uint64_t & program_id)
    {
        if (key.size() != 16)
        {
            return false;
        }

        char * end = nullptr;
        const unsigned long long value = std::strtoull(key.c_str(), &end, 16);
        if (end == key.c_str() || (end != nullptr && *end != '\0'))
        {
            return false;
        }

        program_id = static_cast<uint64_t>(value);
        return true;
    }

    void LoadTitleIdStringListMap(TitleIdStringListMap & out, const JsonValue & value)
    {
        out.clear();
        if (!value.isObject())
        {
            return;
        }

        const JsonMembers members = value.GetMemberNames();
        for (const std::string & key : members)
        {
            uint64_t program_id = 0;
            if (!ParseTitleId(key, program_id) || !value[key].isArray())
            {
                continue;
            }

            std::vector<std::string> names;
            const JsonValue & array = value[key];
            names.reserve(array.size());
            for (uint32_t i = 0, n = array.size(); i < n; ++i)
            {
                if (array[i].isString())
                {
                    names.push_back(array[i].asString());
                }
            }
            out[program_id] = std::move(names);
        }
    }

    JsonValue SaveTitleIdStringListMap(const TitleIdStringListMap & map)
    {
        JsonValue object(JsonValueType::Object);
        for (TitleIdStringListMap::const_iterator entry = map.begin(); entry != map.end(); ++entry)
        {
            if (entry->second.empty())
            {
                continue;
            }

            JsonValue names(JsonValueType::Array);
            for (const std::string & name : entry->second)
            {
                names.Append(JsonValue(name));
            }
            object[FormatTitleId(entry->first)] = names;
        }
        return object;
    }
};

void LoaderSettingChanged(const char * setting, void * /*userData*/)
{
    for (const LoaderSetting & loaderSetting : settings)
    {
        if (loaderSetting.identifier == nullptr)
        {
            continue;
        }

        if (strcmp(loaderSetting.identifier, setting) != 0)
        {
            continue;
        }
        switch (loaderSetting.settingType)
        {
        case SettingType::BooleanValue:
            *loaderSetting.setting.boolValue = g_settings->GetBool(setting);
            break;
        case SettingType::IntegerValue:
            if (loaderSetting.setting.intValue != nullptr)
            {
                *loaderSetting.setting.intValue = g_settings->GetInt(setting);
            }
            break;
        default:
            UNIMPLEMENTED();
        }
    }
}

void SetupLoaderSetting(void)
{
    for (const LoaderSetting & loaderSetting : settings)
    {
        switch (loaderSetting.settingType)
        {
        case SettingType::BooleanValue:
            *loaderSetting.setting.boolValue = loaderSetting.defaultValue.boolValue;
            break;
        case SettingType::IntegerValue:
            if (loaderSetting.setting.intValue != nullptr)
            {
                *loaderSetting.setting.intValue = loaderSetting.defaultValue.intValue;
            }
            break;
        case SettingType::TitleIdStringListMapValue:
            loaderSetting.setting.titleIdStringListMap->clear();
            break;
        default:
            UNIMPLEMENTED();
        }
    }

    JsonValue root;
    JsonReader reader;
    const std::string json = g_settings->GetSectionSettings("nxemu-loader");

    if (!json.empty() && reader.Parse(json.data(), json.data() + json.size(), root) && root.isObject())
    {
        for (const LoaderSetting & loaderSetting : settings)
        {
            if (loaderSetting.json_path == nullptr)
            {
                continue;
            }

            const JsonValue value = JsonGetNestedValue(root, loaderSetting.json_path);
            switch (loaderSetting.settingType)
            {
            case SettingType::BooleanValue:
                if (value.isBool())
                {
                    *loaderSetting.setting.boolValue = value.asBool();
                }
                break;
            case SettingType::IntegerValue:
                if (value.isInt() && loaderSetting.setting.intValue != nullptr)
                {
                    *loaderSetting.setting.intValue = static_cast<int>(value.asInt64());
                }
                break;
            case SettingType::TitleIdStringListMapValue:
                LoadTitleIdStringListMap(*loaderSetting.setting.titleIdStringListMap, value);
                break;
            default:
                UNIMPLEMENTED();
            }
        }
    }

    for (const LoaderSetting & loaderSetting : settings)
    {
        if (loaderSetting.identifier == nullptr)
        {
            continue;
        }

        switch (loaderSetting.settingType)
        {
        case SettingType::BooleanValue:
            g_settings->SetDefaultBool(loaderSetting.identifier, loaderSetting.defaultValue.boolValue);
            g_settings->SetBool(loaderSetting.identifier, *loaderSetting.setting.boolValue);
            break;
        case SettingType::IntegerValue:
            g_settings->SetDefaultInt(loaderSetting.identifier, loaderSetting.defaultValue.intValue);
            g_settings->SetInt(loaderSetting.identifier, loaderSetting.setting.intValue != nullptr ? *loaderSetting.setting.intValue : loaderSetting.defaultValue.intValue);
            break;
        default:
            UNIMPLEMENTED();
        }
    }

    CleanupLoaderSetting();
    for (const LoaderSetting & loaderSetting : settings)
    {
        if (loaderSetting.identifier == nullptr)
        {
            continue;
        }
        g_settings->RegisterCallback(loaderSetting.identifier, LoaderSettingChanged, nullptr);
    }
}

void CleanupLoaderSetting(void)
{
    for (const LoaderSetting & loaderSetting : settings)
    {
        if (loaderSetting.identifier == nullptr)
        {
            continue;
        }
        g_settings->UnregisterCallback(loaderSetting.identifier, LoaderSettingChanged, nullptr);
    }
}

void SaveLoaderSettings(void)
{
    JsonValue root;

    for (const LoaderSetting & loaderSetting : settings)
    {
        switch (loaderSetting.settingType)
        {
        case SettingType::BooleanValue:
            if (loaderSetting.json_path != nullptr &&
                *loaderSetting.setting.boolValue != loaderSetting.defaultValue.boolValue)
            {
                JsonSetNestedValue(root, loaderSetting.json_path, *loaderSetting.setting.boolValue != 0);
            }
            break;
        case SettingType::IntegerValue:
            if (loaderSetting.json_path != nullptr && loaderSetting.setting.intValue != nullptr &&
                *loaderSetting.setting.intValue != loaderSetting.defaultValue.intValue)
            {
                JsonSetNestedValue(root, loaderSetting.json_path, *loaderSetting.setting.intValue);
            }
            break;
        case SettingType::TitleIdStringListMapValue:
        {
            JsonValue value = SaveTitleIdStringListMap(*loaderSetting.setting.titleIdStringListMap);
            if (!value.GetMemberNames().empty())
            {
                JsonSetNestedValue(root, loaderSetting.json_path, value);
            }
            break;
        }
        default:
            UNIMPLEMENTED();
        }
    }
    g_settings->SetSectionSettings("nxemu-loader", root.isNull() ? "" : JsonStyledWriter().write(root));
}

LoaderSetting::LoaderSetting(const char * id, const char * path, bool * val, bool defaultValue_) :
    identifier(id),
    json_path(path),
    settingType(SettingType::BooleanValue)
{
    setting.boolValue = val;
    defaultValue.boolValue = defaultValue_;
}

LoaderSetting::LoaderSetting(const char * id, const char * path, int * val, int defaultValue_) :
    identifier(id),
    json_path(path),
    settingType(SettingType::IntegerValue)
{
    setting.intValue = val;
    defaultValue.intValue = defaultValue_;
}

LoaderSetting::LoaderSetting(const char * path, TitleIdStringListMap * val) :
    identifier(nullptr),
    json_path(path),
    settingType(SettingType::TitleIdStringListMapValue)
{
    setting.titleIdStringListMap = val;
}
