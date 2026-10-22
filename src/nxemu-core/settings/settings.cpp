#include "settings.h"
#include <common/file.h>
#include <common/json.h>
#include <common/path.h>

std::unique_ptr<SettingsStore> SettingsStore::s_instance;
thread_local bool SettingsStore::s_globalReads = false;

SettingsStore::GlobalReadScope::GlobalReadScope() : m_previous(s_globalReads)
{
    s_globalReads = true;
}

SettingsStore::GlobalReadScope::~GlobalReadScope()
{
    s_globalReads = m_previous;
}

void SettingsStore::SetGameOverrides(JsonValue overrides)
{
    m_gameOverrides = std::move(overrides);
}

void SettingsStore::SetControlOverride(const char * setting, JsonValue value)
{
    if (setting == nullptr || (!value.isInt() && !value.isBool())) return;
    m_controlOverrides[setting] = std::move(value);
    NotifyChange(setting);
}

void SettingsStore::ClearControlOverride(const char * setting)
{
    if (!HasControlOverride(setting)) return;
    m_controlOverrides.removeMember(setting);
    NotifyChange(setting);
}

bool SettingsStore::HasControlOverride(const char * setting) const
{
    return setting != nullptr && m_controlOverrides.Find(setting) != nullptr;
}

SettingsStore::SettingsStore()
{
}

bool SettingsStore::Initialize(const char * configPath, const char * baseDirectory)
{
    if (baseDirectory == nullptr || baseDirectory[0] == '\0')
    {
        return false;
    }

    const Path normalization_base(baseDirectory);

    m_configPath = configPath;
    m_controlOverrides = JsonValue();

    m_details = JsonValue();
    for (size_t i = 0; i < 100; i++)
    {
        File configFile;
        if (!configFile.Open(m_configPath.c_str(), IFile::modeRead))
        {
            break;
        }
        uint32_t fileLen = (uint32_t)configFile.GetLength();
        if (fileLen <= 0)
        {
            break;
        }
        std::unique_ptr<char[]> data = std::make_unique<char[]>(fileLen);
        uint32_t Size = configFile.Read(data.get(), fileLen);
        if (Size != fileLen)
        {
            break;
        }

        JsonReader reader;
        JsonValue root;
        if (!reader.Parse(data.get(), data.get() + Size, root))
        {
            return false;
        }
        const JsonValue * value = root.Find("ConfigFile");
        if (value == nullptr)
        {
            m_details = std::move(root);
            break;
        }
        else
        {
            Path newConfigfile(value != nullptr ? value->asString() : "");
            newConfigfile.DirectoryNormalize(normalization_base);
            m_configPath = (const char *)newConfigfile;
        }
    }
    return true;
}

const char * SettingsStore::GetConfigFile(void) const
{
    return m_configPath.c_str();
}

void SettingsStore::SetChanged(const char * setting, bool changed)
{
    if (setting == nullptr)
    {
        return;
    }
    m_settingsChanged[setting] = changed;
}

JsonValue SettingsStore::GetSettings(const char * section) const
{
    const JsonValue * value = m_details.Find(section);
    if (value != nullptr)
    {
        return *value;
    }
    return JsonValue();
}

void SettingsStore::SetSettings(const char * section, JsonValue & json)
{
    if (json.isNull())
    {
        m_details.removeMember(section);
    }
    else
    {
        m_details[section] = json;
    }
}

const char * SettingsStore::GetDefaultString(const char * setting) const
{
    SettingsMapString::const_iterator itr = m_settingsDefaultString.find(setting);
    if (itr == m_settingsDefaultString.end())
    {
        return "";
    }
    return itr->second.c_str();
}

bool SettingsStore::GetDefaultBool(const char * setting) const
{
    SettingsMapBool::const_iterator itr = m_settingsDefaultBool.find(setting);
    if (itr == m_settingsDefaultBool.end())
    {
        return false;
    }
    return itr->second;
}

int32_t SettingsStore::GetDefaultInt(const char* setting) const
{
    SettingsMapInt::const_iterator itr = m_settingsDefaultInt.find(setting);
    if (itr == m_settingsDefaultInt.end())
    {
        return false;
    }
    return itr->second;
}

float SettingsStore::GetDefaultFloat(const char * setting) const
{
    SettingsMapFloat::const_iterator itr = m_settingsDefaultFloat.find(setting);
    if (itr == m_settingsDefaultFloat.end())
    {
        return false;
    }
    return itr->second;
}

const char * SettingsStore::GetString(const char * setting) const
{
    SettingsMapString::const_iterator itr = m_settingsString.find(setting);
    if (itr == m_settingsString.end())
    {
        return GetDefaultString(setting);
    }
    return itr->second.c_str();
}

bool SettingsStore::GetBool(const char * setting) const
{
    const JsonValue * control = s_globalReads ? nullptr : m_controlOverrides.Find(setting);
    if (control != nullptr && control->isBool()) return control->asBool();
    return GetBoolWithoutControl(setting);
}

bool SettingsStore::GetBoolWithoutControl(const char * setting) const
{
    const JsonValue * value = s_globalReads ? nullptr : m_gameOverrides.Find(setting);
    if (value != nullptr && value->isBool())
    {
        return value->asBool();
    }
    SettingsMapBool::const_iterator itr = m_settingsBool.find(setting);
    if (itr == m_settingsBool.end())
    {
        return GetDefaultBool(setting);
    }
    return itr->second;
}

bool SettingsStore::GetChanged(const char * setting) const
{
    SettingsMapBool::const_iterator itr = m_settingsChanged.find(setting);
    if (itr == m_settingsChanged.end())
    {
        return false;
    }
    return itr->second;
}

int32_t SettingsStore::GetInt(const char* setting) const
{
    const JsonValue * control = s_globalReads ? nullptr : m_controlOverrides.Find(setting);
    if (control != nullptr && control->isInt()) return static_cast<int32_t>(control->asInt64());
    return GetIntWithoutControl(setting);
}

int32_t SettingsStore::GetIntWithoutControl(const char * setting) const
{
    const JsonValue * value = s_globalReads ? nullptr : m_gameOverrides.Find(setting);
    if (value != nullptr && value->isInt())
    {
        return static_cast<int32_t>(value->asInt64());
    }
    SettingsMapInt::const_iterator itr = m_settingsInt.find(setting);
    if (itr == m_settingsInt.end())
    {
        return GetDefaultInt(setting);
    }
    return itr->second;
}

float SettingsStore::GetFloat(const char * setting) const
{
    SettingsMapFloat::const_iterator itr = m_settingsFloat.find(setting);
    if (itr == m_settingsFloat.end())
    {
        return GetDefaultFloat(setting);
    }
    return itr->second;
}

void SettingsStore::SetDefaultString(const char * setting, const char * value)
{
    if (setting == nullptr || value == nullptr)
    {
        return;
    }
    m_settingsDefaultString[setting] = value;
}

void SettingsStore::SetDefaultBool(const char * setting, bool value)
{
    m_settingsDefaultBool[setting] = value;
}

void SettingsStore::SetDefaultInt(const char * setting, int32_t value)
{
    m_settingsDefaultInt[setting] = value;
}

void SettingsStore::SetDefaultFloat(const char * setting, float value)
{
    m_settingsDefaultFloat[setting] = value;
}

void SettingsStore::SetString(const char * setting, const char * value)
{
    if (setting == nullptr || value == nullptr)
    {
        return;
    }

    SettingsMapString::const_iterator it = m_settingsString.find(setting);
    if (it != m_settingsString.end() && it->second.compare(value) == 0)
    {
        return;
    }
    m_settingsString[setting] = value != nullptr ? value : "";
    NotifyChange(setting);
}

void SettingsStore::SetBool(const char * setting, bool value)
{
    if (setting == nullptr)
    {
        return;
    }

    SettingsMapBool::const_iterator it = m_settingsBool.find(setting);
    if (it != m_settingsBool.end() && it->second == value)
    {
        return;
    }
    m_settingsBool[setting] = value;
    NotifyChange(setting);
}

void SettingsStore::SetInt(const char * setting, int32_t value)
{
    if (setting == nullptr)
    {
        return;
    }

    SettingsMapInt::const_iterator it = m_settingsInt.find(setting);
    if (it != m_settingsInt.end() && it->second == value)
    {
        return;
    }
    m_settingsInt[setting] = value;
    NotifyChange(setting);
}

void SettingsStore::SetFloat(const char * setting, float value)
{
    if (setting == nullptr)
    {
        return;
    }

    SettingsMapFloat::const_iterator it = m_settingsFloat.find(setting);
    if (it != m_settingsFloat.end() && it->second == value)
    {
        return;
    }
    m_settingsFloat[setting] = value;
    NotifyChange(setting);
}

void SettingsStore::Save(void)
{
    std::string jsonStr = JsonStyledWriter().write(m_details);
    Path(m_configPath).DirectoryCreate();
    File configFile;
    if (!configFile.Open(m_configPath.c_str(), IFile::modeWrite | IFile::modeCreate))
    {
        return;
    }
    configFile.Write(jsonStr.c_str(), (uint32_t)jsonStr.length());
}

void SettingsStore::RegisterCallback(const std::string & setting, SettingChangeCallback callback, void * userData)
{
    if (callback == nullptr)
    {
        return;
    }

    CallbackInfo info = { callback, userData };
    m_notification[setting].emplace_back(info);
}

void SettingsStore::UnregisterCallback(const std::string & setting, SettingChangeCallback callback, void * userData)
{
    NotificationMap::iterator itr = m_notification.find(setting);
    if (itr == m_notification.end())
    {
        return;
    }
    NotificationCallbacks & callbacks = itr->second;
    for (NotificationCallbacks::iterator callItr = callbacks.begin(); callItr != callbacks.end();)
    {
        if (callItr->callback == callback && callItr->userData == userData)
        {
            callItr = callbacks.erase(callItr);
        }
        else
        {
            ++callItr;
        }
    }
}

SettingsStore & SettingsStore::GetInstance()
{
    if (s_instance == nullptr)
    {
        s_instance = std::make_unique<SettingsStore>();
    }
    return *s_instance;
}

void SettingsStore::CleanUp()
{
    s_instance.reset();
}

void SettingsStore::NotifyChange(const char * setting)
{
    if (setting == nullptr)
    {
        return;
    }
    NotificationMap::const_iterator itr = m_notification.find(setting);
    if (itr != m_notification.end())
    {
        const NotificationCallbacks & callbacks = itr->second;
        for (NotificationCallbacks::const_iterator callItr = callbacks.begin(); callItr != callbacks.end(); callItr++)
        {
            callItr->callback(setting, callItr->userData);
        }
    }
}
