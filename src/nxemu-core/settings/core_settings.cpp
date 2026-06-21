#include "core_settings.h"
#include "identifiers.h"
#include "settings.h"
#include <common/json.h>
#include <common/path.h>
#include <yuzu_common/yuzu_assert.h>

namespace
{
    enum class SettingType
    {
        Boolean,
        Interger,
        Path,
        String,
    };

    class CoreSetting
    {
    public:
        CoreSetting(const char * id, bool defaultValue);
        CoreSetting(const char * id, int defaultValue);
        CoreSetting(const char * id, Path * settingPath);
        CoreSetting(const char * id, const char * section, const char * key, bool * settingValue, bool defaultValue);
        CoreSetting(const char * id, const char * section, const char * key, Path * settingPath, std::string * settingPathValue, const char * defaultValue);
        CoreSetting(const char * id, const char * section, const char * key, std::string * settingStr, const char * defaultValue);

        const char * identifier;
        const char * json_section;
        const char * json_key;
        SettingType settingType;
        union
        {
            bool boolValue;
            int intValue;
            const char * strValue;
        } defaults;
        union
        {
            bool * boolValue;
            int * intValue;
            Path * path;
            std::string * strValue;            
        } value;
        union
        {
            std::string * strValue;
        } clearValue;
    };

    static CoreSetting settings[] = {
        { NXCoreSetting::AppBaseDirectory, &coreSettings.baseDir },
        { NXCoreSetting::AppDirectory, &coreSettings.appDir },
        { NXCoreSetting::ModuleDirectory, "", "ModuleDirectory-x64", &coreSettings.moduleDir, &coreSettings.moduleDirValue, "./modules" },
#ifdef _WIN32
#ifdef _DEBUG
        { NXCoreSetting::ModuleLoader, "modules", "loader", &coreSettings.moduleLoader, "loader\\nxemu-loader_d.dll" },
        { NXCoreSetting::ModuleCpu, "modules", "cpu", &coreSettings.moduleCpu, "cpu\\nxemu-cpu_d.dll" },
        { NXCoreSetting::ModuleVideo, "modules", "video", &coreSettings.moduleVideo, "video\\nxemu-video_d.dll" },
        { NXCoreSetting::ModuleOs, "modules", "os", &coreSettings.moduleOs, "operating_system\\nxemu-os_d.dll" },
#else
        { NXCoreSetting::ModuleLoader, "modules", "loader", &coreSettings.moduleLoader, "loader\\nxemu-loader.dll" },
        { NXCoreSetting::ModuleCpu, "modules", "cpu", &coreSettings.moduleCpu, "cpu\\nxemu-cpu.dll" },
        { NXCoreSetting::ModuleVideo, "modules", "video", &coreSettings.moduleVideo, "video\\nxemu-video.dll" },
        { NXCoreSetting::ModuleOs, "modules", "os", &coreSettings.moduleOs, "operating_system\\nxemu-os.dll" },
#endif
#else
        { NXCoreSetting::ModuleLoader, "modules", "loader", &coreSettings.moduleLoader, "libnxemu-loader.so" },
        { NXCoreSetting::ModuleCpu, "modules", "cpu", &coreSettings.moduleCpu, "libnxemu-cpu.so" },
        { NXCoreSetting::ModuleVideo, "modules", "video", &coreSettings.moduleVideo, "libnxemu-video.so" },
        { NXCoreSetting::ModuleOs, "modules", "os", &coreSettings.moduleOs, "libnxemu-os.so" },
#endif
        { NXCoreSetting::ShowLogConsole, "", "ShowLogConsole", &coreSettings.ShowLogConsole, false },
        { NXCoreSetting::LogFilter, "", "LogFilter", &coreSettings.LogFilter, "*:Info" },
        { NXCoreSetting::EmulationRunning, false },
        { NXCoreSetting::EmulationState, (int)EmulationState::Stopped },
        { NXCoreSetting::DisplayedFrames, false },
        { NXCoreSetting::ShuttingDown, false},
        { NXCoreSetting::DiskCacheLoadStage, -1 },
        { NXCoreSetting::DiskCacheLoadCurrent, 0 },
        { NXCoreSetting::DiskCacheLoadTotal, 0 },
        { NXCoreSetting::DiskCacheLoadTick, 0 },
        { NXCoreSetting::GpuHookLibDir, "", nullptr, &coreSettings.gpuHookLibDir, "" },
        { NXCoreSetting::GpuCustomDriverDir, "", nullptr, &coreSettings.gpuCustomDriverDir, "" },
        { NXCoreSetting::GpuCustomDriverName, "", nullptr, &coreSettings.gpuCustomDriverName, "" },
        { NXCoreSetting::GpuFileRedirectDir, "", nullptr, &coreSettings.gpuFileRedirectDir, "" },
    };

    void CoreSettingChanged(const char* setting, void* /*userData*/);

} // namespace

CoreSettings coreSettings = {};

void SetupCoreSetting()
{
    SettingsStore & settingsStore = SettingsStore::GetInstance();
    JsonValue jsonSettings = settingsStore.GetSettings("Core");

    for (CoreSetting & setting : settings)
    {
        switch (setting.settingType)
        {
        case SettingType::Boolean:
            if (setting.value.boolValue != nullptr)
            {
                *setting.value.boolValue = setting.defaults.boolValue;
            }
            break;
        case SettingType::Interger:
            if (setting.value.intValue != nullptr)
            {
                *setting.value.intValue = setting.defaults.intValue;
            }
            break;
        case SettingType::Path:
            if (setting.value.path != nullptr)
            {
                *setting.value.path = Path(setting.defaults.strValue, "").DirectoryNormalize(Path(Path::MODULE_DIRECTORY));
            }
            if (setting.clearValue.strValue != nullptr)
            {
                *setting.clearValue.strValue = setting.defaults.strValue;
            }
            break;
        case SettingType::String:
            if (setting.value.strValue != nullptr)
            {
                *setting.value.strValue = setting.defaults.strValue;
            }
            break;
        default:
            UNIMPLEMENTED();
        }

        if (setting.json_key != nullptr)
        {
            JsonValue value = jsonSettings;
            if (setting.json_section != nullptr && setting.json_section[0] != '\0')
            {
                JsonValue section = value[setting.json_section];
                value = section.isObject() ? section : JsonValue();
            }
            value = value[setting.json_key];
            switch (setting.settingType)
            {
            case SettingType::Boolean:
                if (value.isBool() && setting.value.boolValue != nullptr)
                {
                    *setting.value.boolValue = value.asBool();
                }
                break;
            case SettingType::Path:
                if (value.isString() && setting.value.path != nullptr && setting.clearValue.strValue != nullptr)
                {
                    *setting.clearValue.strValue = value.asString();
                    *setting.value.path = Path(*setting.clearValue.strValue, "").DirectoryNormalize(Path(Path::MODULE_DIRECTORY));
                }
                break;
            case SettingType::String:
                if (value.isString() && setting.value.strValue != nullptr)
                {
                    *setting.value.strValue = value.asString();
                }
                break;
            default:
                UNIMPLEMENTED();
            }
        }

        switch (setting.settingType)
        {
        case SettingType::Boolean:
            settingsStore.SetDefaultBool(setting.identifier, setting.defaults.boolValue);
            settingsStore.SetBool(setting.identifier, setting.value.boolValue != nullptr ? *setting.value.boolValue : setting.defaults.boolValue);
            break;
        case SettingType::Interger:
            settingsStore.SetDefaultInt(setting.identifier, setting.defaults.intValue);
            settingsStore.SetInt(setting.identifier, setting.value.intValue != nullptr ? *setting.value.intValue : setting.defaults.intValue);
            break;
        case SettingType::Path:
            settingsStore.SetDefaultString(setting.identifier, setting.defaults.strValue);
            settingsStore.SetString(setting.identifier, setting.clearValue.strValue != nullptr ? setting.clearValue.strValue->c_str() : setting.defaults.strValue);
            break;
        case SettingType::String:
            settingsStore.SetDefaultString(setting.identifier, setting.defaults.strValue);
            settingsStore.SetString(setting.identifier, setting.value.strValue != nullptr ? setting.value.strValue->c_str() : setting.defaults.strValue);
            break;
        default:
            UNIMPLEMENTED();
        }

        settingsStore.RegisterCallback(setting.identifier, CoreSettingChanged, nullptr);
    }
}

void SaveCoreSetting(void)
{
    typedef std::map<std::string, JsonValue> SectionMap;
    SectionMap sections;

    for (const CoreSetting & coreSetting : settings)
    {
        if (coreSetting.json_key == nullptr)
        {
            continue;
        }
        switch (coreSetting.settingType)
        {
        case SettingType::Boolean:
            if (*coreSetting.value.boolValue != coreSetting.defaults.boolValue)
            {
                sections[coreSetting.json_section][coreSetting.json_key] = *coreSetting.value.boolValue;
            }
            break;
        case SettingType::Path:
            if (*coreSetting.clearValue.strValue != coreSetting.defaults.strValue)
            {
                sections[coreSetting.json_section][coreSetting.json_key] = *coreSetting.clearValue.strValue;
            }
            break;
        case SettingType::String:
            if (*coreSetting.value.strValue != coreSetting.defaults.strValue)
            {
                sections[coreSetting.json_section][coreSetting.json_key] = *coreSetting.value.strValue;
            }
            break;
        default:
            UNIMPLEMENTED();
        }
    }
    JsonValue json;
    for (SectionMap::const_iterator it = sections.begin(); it != sections.end(); ++it)
    {
        if (it->second.size() == 0)
        {
            continue;
        }

        if (it->first.empty())
        {
            if (it->second.isObject())
            {
                JsonMembers names = it->second.GetMemberNames();
                for (const std::string & name : names)
                {
                    json[name] = it->second[name];
                }
            }
        }
        else
        {
            json[it->first] = it->second;
        }
    }

    SettingsStore & settingsStore = SettingsStore::GetInstance();
    settingsStore.SetSettings("Core", json);
    settingsStore.Save();
}

namespace
{
CoreSetting::CoreSetting(const char * id, bool defaultValue) :
    identifier(id),
    json_section(nullptr),
    json_key(nullptr),
    settingType(SettingType::Boolean)
{
    defaults.boolValue = defaultValue;
    value.boolValue = nullptr;
    clearValue.strValue = nullptr;
}

CoreSetting::CoreSetting(const char * id, int defaultValue) :
    identifier(id),
    json_section(nullptr),
    json_key(nullptr),
    settingType(SettingType::Interger)
{
    defaults.intValue = defaultValue;
    value.intValue = nullptr;
    clearValue.strValue = nullptr;
}

CoreSetting::CoreSetting(const char * id, Path * settingPath) :
    identifier(id),
    json_section(nullptr),
    json_key(nullptr),
    settingType(SettingType::Path)
{
    defaults.strValue = "";
    value.path = settingPath;
    clearValue.strValue = nullptr;
}

CoreSetting::CoreSetting(const char * id, const char * section, const char * key, bool * settingValue, bool defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::Boolean)
{
    defaults.boolValue = defaultValue;
    value.boolValue = settingValue;
    clearValue.strValue = nullptr;
}

CoreSetting::CoreSetting(const char * id, const char * section, const char * key, Path * settingPath, std::string * settingPathValue, const char * defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::Path)
{
    defaults.strValue = defaultValue;
    value.path = settingPath;
    clearValue.strValue = settingPathValue;
}

CoreSetting::CoreSetting(const char * id, const char * section, const char * key, std::string * settingStr, const char * defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::String)
{
    defaults.strValue = defaultValue;
    value.strValue = settingStr;
    clearValue.strValue = nullptr;
}

void CoreSettingChanged(const char * setting, void* /*userData*/)
{
    SettingsStore & settingsStore = SettingsStore::GetInstance();

    for (const CoreSetting & coreSetting : settings)
    {
        if (strcmp(coreSetting.identifier, setting) != 0)
        {
            continue;
        }
        switch (coreSetting.settingType)
        {
        case SettingType::Boolean:
            if (coreSetting.value.boolValue != nullptr)
            {
                *coreSetting.value.boolValue = settingsStore.GetBool(setting);
            }
            break;
        case SettingType::Interger:
            if (coreSetting.value.intValue != nullptr)
            {
                *coreSetting.value.intValue = settingsStore.GetInt(setting);
            }
            break;
        case SettingType::String:
            if (coreSetting.value.strValue != nullptr)
            {
                *coreSetting.value.strValue = settingsStore.GetString(setting);
            }
            break;
        case SettingType::Path:
            if (coreSetting.value.path != nullptr)
            {
                *coreSetting.value.path = Path(settingsStore.GetString(setting), "").DirectoryNormalize(Path(Path::MODULE_DIRECTORY));
            }
            if (coreSetting.clearValue.strValue != nullptr)
            {
                *coreSetting.clearValue.strValue = settingsStore.GetString(setting);
            }
            break;
        default:
            UNIMPLEMENTED();
        }
        break;
    }
}

} // namespace