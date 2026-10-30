#include "settings.h"
#include <common/file.h>
#include <common/json.h>
#include <common/path.h>

std::unique_ptr<SettingsStore> SettingsStore::s_instance;

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

    std::lock_guard<std::mutex> lock(m_mutex);
    m_configPath = configPath;

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
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_configPath.c_str();
}

void SettingsStore::SetChanged(const char * setting, bool changed)
{
    if (setting == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_settingsChanged[setting] = changed;
}

JsonValue SettingsStore::GetSettings(const char * section) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const JsonValue * value = m_details.Find(section);
    if (value != nullptr)
    {
        return *value;
    }
    return JsonValue();
}

void SettingsStore::SetSettings(const char * section, JsonValue & json)
{
    std::lock_guard<std::mutex> lock(m_mutex);
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
    std::lock_guard<std::mutex> lock(m_mutex);
    return LookupDefaultString(setting);
}

bool SettingsStore::GetDefaultBool(const char * setting) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return LookupDefaultBool(setting);
}

int32_t SettingsStore::GetDefaultInt(const char* setting) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return LookupDefaultInt(setting);
}

float SettingsStore::GetDefaultFloat(const char * setting) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return LookupDefaultFloat(setting);
}

const char * SettingsStore::GetString(const char * setting) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    SettingsMapString::const_iterator itr = m_settingsString.find(setting);
    if (itr == m_settingsString.end())
    {
        return LookupDefaultString(setting);
    }
    return itr->second.c_str();
}

bool SettingsStore::GetBool(const char * setting) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    SettingsMapBool::const_iterator itr = m_settingsBool.find(setting);
    if (itr == m_settingsBool.end())
    {
        return LookupDefaultBool(setting);
    }
    return itr->second;
}

bool SettingsStore::GetChanged(const char * setting) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    SettingsMapBool::const_iterator itr = m_settingsChanged.find(setting);
    if (itr == m_settingsChanged.end())
    {
        return false;
    }
    return itr->second;
}

int32_t SettingsStore::GetInt(const char* setting) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    SettingsMapInt::const_iterator itr = m_settingsInt.find(setting);
    if (itr == m_settingsInt.end())
    {
        return LookupDefaultInt(setting);
    }
    return itr->second;
}

float SettingsStore::GetFloat(const char * setting) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    SettingsMapFloat::const_iterator itr = m_settingsFloat.find(setting);
    if (itr == m_settingsFloat.end())
    {
        return LookupDefaultFloat(setting);
    }
    return itr->second;
}

void SettingsStore::SetDefaultString(const char * setting, const char * value)
{
    if (setting == nullptr || value == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_settingsDefaultString[setting] = value;
}

void SettingsStore::SetDefaultBool(const char * setting, bool value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_settingsDefaultBool[setting] = value;
}

void SettingsStore::SetDefaultInt(const char * setting, int32_t value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_settingsDefaultInt[setting] = value;
}

void SettingsStore::SetDefaultFloat(const char * setting, float value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_settingsDefaultFloat[setting] = value;
}

void SettingsStore::SetString(const char * setting, const char * value)
{
    if (setting == nullptr || value == nullptr)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        SettingsMapString::const_iterator it = m_settingsString.find(setting);
        if (it != m_settingsString.end() && it->second.compare(value) == 0)
        {
            return;
        }
        m_settingsString[setting] = value;
    }
    NotifyChange(setting);
}

void SettingsStore::SetBool(const char * setting, bool value)
{
    if (setting == nullptr)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        SettingsMapBool::const_iterator it = m_settingsBool.find(setting);
        if (it != m_settingsBool.end() && it->second == value)
        {
            return;
        }
        m_settingsBool[setting] = value;
    }
    NotifyChange(setting);
}

void SettingsStore::SetInt(const char * setting, int32_t value)
{
    if (setting == nullptr)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        SettingsMapInt::const_iterator it = m_settingsInt.find(setting);
        if (it != m_settingsInt.end() && it->second == value)
        {
            return;
        }
        m_settingsInt[setting] = value;
    }
    NotifyChange(setting);
}

void SettingsStore::SetFloat(const char * setting, float value)
{
    if (setting == nullptr)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        SettingsMapFloat::const_iterator it = m_settingsFloat.find(setting);
        if (it != m_settingsFloat.end() && it->second == value)
        {
            return;
        }
        m_settingsFloat[setting] = value;
    }
    NotifyChange(setting);
}

void SettingsStore::Save(void)
{
    std::string jsonStr;
    std::string configPath;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        jsonStr = JsonStyledWriter().write(m_details);
        configPath = m_configPath;
    }
    Path(configPath).DirectoryCreate();
    File configFile;
    if (!configFile.Open(configPath.c_str(), IFile::modeWrite | IFile::modeCreate))
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
    std::lock_guard<std::mutex> lock(m_mutex);
    m_notification[setting].emplace_back(info);
}

void SettingsStore::UnregisterCallback(const std::string & setting, SettingChangeCallback callback, void * userData)
{
    std::lock_guard<std::mutex> lock(m_mutex);
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
    NotificationCallbacks callbacks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        NotificationMap::const_iterator itr = m_notification.find(setting);
        if (itr == m_notification.end())
        {
            return;
        }
        callbacks = itr->second;
    }
    for (NotificationCallbacks::const_iterator callItr = callbacks.begin(); callItr != callbacks.end(); callItr++)
    {
        callItr->callback(setting, callItr->userData);
    }
}

const char * SettingsStore::LookupDefaultString(const char * setting) const
{
    SettingsMapString::const_iterator itr = m_settingsDefaultString.find(setting);
    if (itr == m_settingsDefaultString.end())
    {
        return "";
    }
    return itr->second.c_str();
}

bool SettingsStore::LookupDefaultBool(const char * setting) const
{
    SettingsMapBool::const_iterator itr = m_settingsDefaultBool.find(setting);
    if (itr == m_settingsDefaultBool.end())
    {
        return false;
    }
    return itr->second;
}

int32_t SettingsStore::LookupDefaultInt(const char * setting) const
{
    SettingsMapInt::const_iterator itr = m_settingsDefaultInt.find(setting);
    if (itr == m_settingsDefaultInt.end())
    {
        return 0;
    }
    return itr->second;
}

float SettingsStore::LookupDefaultFloat(const char * setting) const
{
    SettingsMapFloat::const_iterator itr = m_settingsDefaultFloat.find(setting);
    if (itr == m_settingsDefaultFloat.end())
    {
        return 0.0f;
    }
    return itr->second;
}
