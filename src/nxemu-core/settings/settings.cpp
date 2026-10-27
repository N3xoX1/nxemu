#include "settings.h"
#include "identifiers.h"
#include <cstring>
#include <algorithm>
#include <common/file.h>
#include <common/json.h>
#include <common/path.h>

std::unique_ptr<SettingsStore> SettingsStore::s_instance;

namespace {
// Track the actual registration, not just whether this thread is in any
// callback: removing a different subscriber must still drain its invocations.
struct CallbackInvocation
{
    const void * entry;
    CallbackInvocation * previous;
};
thread_local CallbackInvocation * g_callbackInvocation = nullptr;
}


SettingsStore::SettingsStore()
{
}

std::atomic<bool> * SettingsStore::RuntimeBool(const char * setting) const
{
    if (setting == nullptr || std::strncmp(setting, "nxcore:", 7) != 0) return nullptr;
    if (std::strcmp(setting, NXCoreSetting::EmulationRunning) == 0) return &m_emulationRunning;
    if (std::strcmp(setting, NXCoreSetting::DisplayedFrames) == 0) return &m_displayedFrames;
    return nullptr;
}

std::atomic<int32_t> * SettingsStore::RuntimeInt(const char * setting) const
{
    if (setting == nullptr || std::strncmp(setting, "nxcore:", 7) != 0) return nullptr;
    if (std::strcmp(setting, NXCoreSetting::EmulationState) == 0) return &m_emulationState;
    if (std::strcmp(setting, NXCoreSetting::DiskCacheLoadStage) == 0) return &m_diskCacheStage;
    if (std::strcmp(setting, NXCoreSetting::DiskCacheLoadCurrent) == 0) return &m_diskCacheCurrent;
    if (std::strcmp(setting, NXCoreSetting::DiskCacheLoadTotal) == 0) return &m_diskCacheTotal;
    if (std::strcmp(setting, NXCoreSetting::DiskCacheLoadTick) == 0) return &m_diskCacheTick;
    return nullptr;
}

bool SettingsStore::TryGetDiskCacheProgress(DiskCacheProgress & out) const
{
    // Writer marks the tick odd before changing fields, then even to publish.
    // seq_cst prevents a mixed-stage snapshot even on weakly ordered ARM CPUs.
    for (unsigned attempt = 0; attempt < 4; ++attempt)
    {
        const int32_t before = m_diskCacheTick.load(std::memory_order_seq_cst);
        if (static_cast<uint32_t>(before) & 1u) continue;
        DiskCacheProgress candidate;
        candidate.stage = m_diskCacheStage.load(std::memory_order_seq_cst);
        candidate.current = m_diskCacheCurrent.load(std::memory_order_seq_cst);
        candidate.total = m_diskCacheTotal.load(std::memory_order_seq_cst);
        const int32_t after = m_diskCacheTick.load(std::memory_order_seq_cst);
        if (before == after && !(static_cast<uint32_t>(after) & 1u))
        {
            candidate.tick = after;
            out = candidate;
            return true;
        }
    }
    return false;
}

bool SettingsStore::Initialize(const char * configPath, const char * baseDirectory)
{
    if (baseDirectory == nullptr || baseDirectory[0] == '\0')
    {
        return false;
    }

    const Path normalization_base(baseDirectory);

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
    if (auto * runtime = RuntimeBool(setting)) return runtime->load(std::memory_order_seq_cst);
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
    if (auto * runtime = RuntimeInt(setting)) return runtime->load(std::memory_order_seq_cst);
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
    if (auto * runtime = RuntimeBool(setting))
    {
        if (runtime->exchange(value, std::memory_order_seq_cst) != value)
            NotifyChange(setting);
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
    if (auto * runtime = RuntimeInt(setting))
    {
        if (runtime->exchange(value, std::memory_order_seq_cst) != value)
        {
            // An odd tick is a write-in-progress marker, not a new frame.
            if (runtime != &m_diskCacheTick || !(static_cast<uint32_t>(value) & 1u))
                NotifyChange(setting);
        }
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
        return;

    auto info = std::make_shared<CallbackInfo>(CallbackInfo{callback, userData});
    std::lock_guard<std::mutex> lock(m_notificationMutex);
    m_notification[setting].emplace_back(std::move(info));
    m_hasCallbacks.store(true, std::memory_order_release);
}

void SettingsStore::UnregisterCallback(const std::string & setting, SettingChangeCallback callback, void * userData)
{
    std::unique_lock<std::mutex> lock(m_notificationMutex);
    auto itr = m_notification.find(setting);
    if (itr == m_notification.end())
        return;

    NotificationCallbacks retired;
    for (const auto & entry : itr->second)
    {
        if (entry->callback == callback && entry->userData == userData)
        {
            entry->removed = true;
            retired.push_back(entry);
        }
    }
    // Keep retired records discoverable while draining. A concurrent second
    // unregistration must not return early and destroy the same userData.
    for (const auto & entry : retired)
    {
        unsigned localInvocations = 0;
        for (auto * frame = g_callbackInvocation; frame; frame = frame->previous)
        {
            localInvocations += frame->entry == entry.get();
        }
        // Concurrent invocations may both unregister themselves. Neither can
        // join the other in that case; subscriber ownership must outlive all
        // self-removing callbacks. External unregistration always drains.
        if (localInvocations == 0)
            m_notificationIdle.wait(lock, [&] { return entry->inFlight == 0; });
    }
    // Waiting releases the lock, so look up the container again afterwards.
    itr = m_notification.find(setting);
    if (itr != m_notification.end())
    {
        std::erase_if(itr->second, [](const auto & entry) {
            return entry->removed && entry->inFlight == 0;
        });
        if (itr->second.empty())
            m_notification.erase(itr);
    }
    m_hasCallbacks.store(!m_notification.empty(), std::memory_order_release);
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
    if (setting == nullptr || !m_hasCallbacks.load(std::memory_order_acquire))
        return;
    NotificationCallbacks callbacks;
    {
        std::lock_guard<std::mutex> lock(m_notificationMutex);
        const auto itr = m_notification.find(setting);
        if (itr == m_notification.end())
            return;
        callbacks = itr->second;
    }

    // The snapshot owns registration records, not subscriber objects. Check
    // removal before each call and count active invocations while under the
    // registry lock. No user callback executes under that lock.
    for (const auto & entry : callbacks)
    {
        {
            std::lock_guard<std::mutex> lock(m_notificationMutex);
            if (entry->removed)
                continue;
            ++entry->inFlight;
        }
        CallbackInvocation invocation{entry.get(), g_callbackInvocation};
        g_callbackInvocation = &invocation;
        const auto finish = [&] {
            g_callbackInvocation = invocation.previous;
            bool removed;
            {
                std::lock_guard<std::mutex> lock(m_notificationMutex);
                --entry->inFlight;
                removed = entry->removed;
                if (removed && entry->inFlight == 0)
                {
                    const auto itr = m_notification.find(setting);
                    if (itr != m_notification.end())
                    {
                        std::erase(itr->second, entry);
                        if (itr->second.empty())
                            m_notification.erase(itr);
                    }
                    m_hasCallbacks.store(!m_notification.empty(), std::memory_order_release);
                }
            }
            if (removed)
                m_notificationIdle.notify_all();
        };
        try
        {
            entry->callback(setting, entry->userData);
        }
        catch (...)
        {
            finish();
            throw;
        }
        finish();
    }
}
