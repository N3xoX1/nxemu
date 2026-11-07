#include "core_settings.h"
#include "identifiers.h"
#include "settings.h"
#include <common/json.h>
#include <common/path.h>
#include <cstring>
#include <iterator>
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
        CoreSetting(const char * id, bool defaultValue, bool debugSetting = false);
        CoreSetting(const char * id, int defaultValue, bool debugSetting = false);
        CoreSetting(const char * id, Path * settingPath, bool debugSetting = false);
        CoreSetting(const char * id, const char * section, const char * key, bool * settingValue, bool defaultValue, bool debugSetting = false);
        CoreSetting(const char * id, const char * section, const char * key, Path * settingPath, std::string * settingPathValue, const char * defaultValue, bool debugSetting = false);
        CoreSetting(const char * id, const char * section, const char * key, std::string * settingStr, const char * defaultValue, bool debugSetting = false);
        CoreSetting(const char * id, const char * section, const char * key, int32_t * settingValue, int32_t defaultValue, const char * const * names, int32_t nameCount, bool debugSetting = false);

        const char * identifier;
        const char * json_section;
        const char * json_key;
        SettingType settingType;
        bool debugSetting;
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
        const char * const * names = nullptr;
        int32_t nameCount = 0;
    };

    constexpr const char * LanguageNames[] = {
        "Japanese", "EnglishAmerican", "French", "German", "Italian", "Spanish", "Chinese", "Korean", "Dutch", "Portuguese", "Russian",
        "Taiwanese", "EnglishBritish", "FrenchCanadian", "SpanishLatin", "ChineseSimplified", "ChineseTraditional", "PortugueseBrazilian",
    };
    constexpr int32_t LanguageEnglishAmerican = 1;

#if defined(_M_ARM64) || defined(ARCHITECTURE_arm64) || defined(__aarch64__)
    constexpr const char * ModuleDirectorySettingKey = "ModuleDirectory-arm64";
#else
    constexpr const char * ModuleDirectorySettingKey = "ModuleDirectory-x64";
#endif

    static CoreSetting settings[] = {
        { NXCoreSetting::AppBaseDirectory, &coreSettings.baseDir },
        { NXCoreSetting::AppDirectory, &coreSettings.appDir },
        { NXCoreSetting::ModuleDirectory, "", ModuleDirectorySettingKey, &coreSettings.moduleDir, &coreSettings.moduleDirValue, "./modules" },
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
#elif defined(__APPLE__)
        {NXCoreSetting::ModuleLoader, "modules", "loader", &coreSettings.moduleLoader, "loader/libnxemu-loader.dylib"},
        {NXCoreSetting::ModuleCpu, "modules", "cpu", &coreSettings.moduleCpu, "cpu/libnxemu-cpu.dylib"},
        {NXCoreSetting::ModuleVideo, "modules", "video", &coreSettings.moduleVideo, "video/libnxemu-video.dylib"},
        {NXCoreSetting::ModuleOs, "modules", "os", &coreSettings.moduleOs, "operating_system/libnxemu-os.dylib"},
#elif defined(ANDROID)
        {NXCoreSetting::ModuleLoader, "modules", "loader", &coreSettings.moduleLoader, "libnxemu-loader.so"},
        {NXCoreSetting::ModuleCpu, "modules", "cpu", &coreSettings.moduleCpu, "libnxemu-cpu.so"},
        {NXCoreSetting::ModuleVideo, "modules", "video", &coreSettings.moduleVideo, "libnxemu-video.so"},
        {NXCoreSetting::ModuleOs, "modules", "os", &coreSettings.moduleOs, "libnxemu-os.so"},
#elif !defined(NDEBUG)
        {NXCoreSetting::ModuleLoader, "modules", "loader", &coreSettings.moduleLoader, "loader/libnxemu-loader_d.so"},
        {NXCoreSetting::ModuleCpu, "modules", "cpu", &coreSettings.moduleCpu, "cpu/libnxemu-cpu_d.so"},
        {NXCoreSetting::ModuleVideo, "modules", "video", &coreSettings.moduleVideo, "video/libnxemu-video_d.so"},
        {NXCoreSetting::ModuleOs, "modules", "os", &coreSettings.moduleOs, "operating_system/libnxemu-os_d.so"},
#else
        {NXCoreSetting::ModuleLoader, "modules", "loader", &coreSettings.moduleLoader, "loader/libnxemu-loader.so"},
        {NXCoreSetting::ModuleCpu, "modules", "cpu", &coreSettings.moduleCpu, "cpu/libnxemu-cpu.so"},
        {NXCoreSetting::ModuleVideo, "modules", "video", &coreSettings.moduleVideo, "video/libnxemu-video.so"},
        {NXCoreSetting::ModuleOs, "modules", "os", &coreSettings.moduleOs, "operating_system/libnxemu-os.so"},
#endif
        { NXCoreSetting::DebugSettingsEnabled, "", "DebugSettingsEnabled", &coreSettings.debugSettingsEnabled, false },
        { NXCoreSetting::ShowLogConsole, "", "ShowLogConsole", &coreSettings.ShowLogConsole, false, true },
        { NXCoreSetting::LogFilter, "", "LogFilter", &coreSettings.LogFilter, "*:Info", true },
        { NXCoreSetting::EmulationRunning, false },
        { NXCoreSetting::EmulationState, (int)EmulationState::Stopped },
        { NXCoreSetting::GameLoadResult, 0 },
        { NXCoreSetting::Has39BitAddressSpace, false },
        { NXCoreSetting::Has36BitAddressSpace, false },
        { NXCoreSetting::Is64BitProgram, false },
        { NXCoreSetting::DisplayedFrames, false },
        { NXCoreSetting::GpuInitializationState, (int)GpuInitializationState::Preparing },
        { NXCoreSetting::ShuttingDown, false},
        { NXCoreSetting::DiskCacheLoadStage, -1 },
        { NXCoreSetting::DiskCacheLoadCurrent, 0 },
        { NXCoreSetting::DiskCacheLoadTotal, 0 },
        { NXCoreSetting::DiskCacheLoadTick, 0 },
        { NXCoreSetting::GpuHookLibDir, "", nullptr, &coreSettings.gpuHookLibDir, "" },
        { NXCoreSetting::GpuCustomDriverDir, "", nullptr, &coreSettings.gpuCustomDriverDir, "" },
        { NXCoreSetting::GpuCustomDriverName, "", nullptr, &coreSettings.gpuCustomDriverName, "" },
        { NXCoreSetting::GpuFileRedirectDir, "", nullptr, &coreSettings.gpuFileRedirectDir, "" },
        { NXCoreSetting::SystemLanguageIndex, "", "ConsoleLanguage", &coreSettings.languageIndex, LanguageEnglishAmerican, LanguageNames, static_cast<int32_t>(std::size(LanguageNames)) },
    };

    void ApplyDefault(const CoreSetting & setting)
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
    }

    void CoreSettingChanged(const char* setting, void* /*userData*/);

} // namespace

CoreSettings coreSettings = {};

void SetupCoreSetting()
{
    SettingsStore & settingsStore = SettingsStore::GetInstance();
    JsonValue jsonSettings = settingsStore.GetSettings("Core");

    for (CoreSetting & setting : settings)
    {
        ApplyDefault(setting);

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
            case SettingType::Interger:
                if (setting.value.intValue == nullptr)
                {
                    break;
                }
                if (value.isString() && setting.names != nullptr)
                {
                    const std::string name = value.asString();
                    for (int32_t index = 0; index < setting.nameCount; index++)
                    {
                        if (name == setting.names[index])
                        {
                            *setting.value.intValue = index;
                        }
                    }
                }
                else if (value.isInt())
                {
                    *setting.value.intValue = static_cast<int32_t>(value.asInt64());
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

        settingsStore.SetDebugSetting(setting.identifier, setting.debugSetting);
        settingsStore.RegisterCallback(setting.identifier, CoreSettingChanged, nullptr);
    }
    settingsStore.SetDebugSettingsEnabled(coreSettings.debugSettingsEnabled);
    for (const CoreSetting & setting : settings)
    {
        if (setting.debugSetting && !settingsStore.DebugSettingsEnabled())
        {
            ApplyDefault(setting);
        }
    }
}

void SaveCoreSetting(void)
{
    typedef std::map<std::string, JsonValue> SectionMap;
    SectionMap sections;
    SettingsStore & settingsStore = SettingsStore::GetInstance();
    const JsonValue saved = settingsStore.GetSettings("Core");

    for (const CoreSetting & coreSetting : settings)
    {
        if (coreSetting.json_key == nullptr)
        {
            continue;
        }
        if (coreSetting.debugSetting && !settingsStore.DebugSettingsEnabled())
        {
            const bool topLevel = coreSetting.json_section == nullptr || coreSetting.json_section[0] == '\0';
            const JsonValue & section = topLevel || !saved.isObject() ? saved : saved[coreSetting.json_section];
            if (section.isObject() && section.isMember(coreSetting.json_key))
            {
                sections[topLevel ? "" : coreSetting.json_section][coreSetting.json_key] = section[coreSetting.json_key];
            }
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
        case SettingType::Interger:
            if (*coreSetting.value.intValue != coreSetting.defaults.intValue)
            {
                const int32_t value = *coreSetting.value.intValue;
                if (coreSetting.names != nullptr && value >= 0 && value < coreSetting.nameCount)
                {
                    sections[coreSetting.json_section][coreSetting.json_key] = coreSetting.names[value];
                }
                else
                {
                    sections[coreSetting.json_section][coreSetting.json_key] = value;
                }
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

    settingsStore.SetSettings("Core", json);
    settingsStore.Save();
}

namespace
{
CoreSetting::CoreSetting(const char * id, bool defaultValue, bool debugSetting) :
    identifier(id),
    json_section(nullptr),
    json_key(nullptr),
    settingType(SettingType::Boolean),
    debugSetting(debugSetting)
{
    defaults.boolValue = defaultValue;
    value.boolValue = nullptr;
    clearValue.strValue = nullptr;
}

CoreSetting::CoreSetting(const char * id, int defaultValue, bool debugSetting) :
    identifier(id),
    json_section(nullptr),
    json_key(nullptr),
    settingType(SettingType::Interger),
    debugSetting(debugSetting)
{
    defaults.intValue = defaultValue;
    value.intValue = nullptr;
    clearValue.strValue = nullptr;
}

CoreSetting::CoreSetting(const char * id, Path * settingPath, bool debugSetting) :
    identifier(id),
    json_section(nullptr),
    json_key(nullptr),
    settingType(SettingType::Path),
    debugSetting(debugSetting)
{
    defaults.strValue = "";
    value.path = settingPath;
    clearValue.strValue = nullptr;
}

CoreSetting::CoreSetting(const char * id, const char * section, const char * key, bool * settingValue, bool defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::Boolean),
    debugSetting(debugSetting)
{
    defaults.boolValue = defaultValue;
    value.boolValue = settingValue;
    clearValue.strValue = nullptr;
}

CoreSetting::CoreSetting(const char * id, const char * section, const char * key, Path * settingPath, std::string * settingPathValue, const char * defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::Path),
    debugSetting(debugSetting)
{
    defaults.strValue = defaultValue;
    value.path = settingPath;
    clearValue.strValue = settingPathValue;
}

CoreSetting::CoreSetting(const char * id, const char * section, const char * key, std::string * settingStr, const char * defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::String),
    debugSetting(debugSetting)
{
    defaults.strValue = defaultValue;
    value.strValue = settingStr;
    clearValue.strValue = nullptr;
}

CoreSetting::CoreSetting(const char * id, const char * section, const char * key, int32_t * settingValue, int32_t defaultValue, const char * const * names_, int32_t nameCount_, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::Interger),
    debugSetting(debugSetting),
    names(names_),
    nameCount(nameCount_)
{
    defaults.intValue = defaultValue;
    value.intValue = settingValue;
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
    if (strcmp(setting, NXCoreSetting::DebugSettingsEnabled) == 0)
    {
        settingsStore.SetDebugSettingsEnabled(coreSettings.debugSettingsEnabled);
        settingsStore.NotifyDebugSettings();
    }
}

} // namespace