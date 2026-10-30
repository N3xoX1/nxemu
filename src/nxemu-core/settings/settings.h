#pragma once
#include <nxemu-module-spec/base.h>
#include <common/json.h>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

class SettingsStore
{
    struct CallbackInfo
    {
        SettingChangeCallback callback;
        void * userData;
        unsigned inFlight = 0; // Guarded by m_mutex.
        bool removed = false;
    };

    typedef std::unordered_map<std::string, std::string> SettingsMapString;
    typedef std::unordered_map<std::string, bool> SettingsMapBool;
    typedef std::unordered_map<std::string, int32_t> SettingsMapInt;
    typedef std::unordered_map<std::string, float> SettingsMapFloat;
    typedef std::vector<std::shared_ptr<CallbackInfo>> NotificationCallbacks;
    typedef std::unordered_map<std::string, NotificationCallbacks> NotificationMap;

public:
    SettingsStore();

    bool Initialize(const char * configPath, const char * baseDirectory);
    const char * GetConfigFile(void) const;

    JsonValue GetSettings(const char * section) const;
    void SetSettings(const char * section, JsonValue & json);

    const char * GetDefaultString(const char * setting) const;
    bool GetDefaultBool(const char * setting) const;
    int GetDefaultInt(const char * setting) const;
    float GetDefaultFloat(const char * setting) const;
    const char * GetString(const char * setting) const;
    bool GetBool(const char * setting) const;
    bool GetChanged(const char * setting) const;
    int32_t GetInt(const char * setting) const;
    float GetFloat(const char * setting) const;
    void SetDefaultString(const char * setting, const char * value);
    void SetDefaultBool(const char * setting, bool value);
    void SetDefaultInt(const char * setting, const int32_t value);
    void SetDefaultFloat(const char * setting, const float value);
    void SetString(const char * setting, const char * value);
    void SetBool(const char * setting, bool value);
    void SetInt(const char * setting, int32_t value);
    void SetFloat(const char * setting, float value);
    void SetChanged(const char * setting, bool changed);

    void Save(void);

    void RegisterCallback(const std::string & setting, SettingChangeCallback callback, void * userData);
    // Waits for calls running on other threads, so the caller can then free userData
    // or unload the module that owns the callback. A callback may remove itself.
    void UnregisterCallback(const std::string & setting, SettingChangeCallback callback, void * userData);

    static SettingsStore& GetInstance();
    static void CleanUp();

private:
    void NotifyChange(const char * setting);
    const char * LookupDefaultString(const char * setting) const;
    bool LookupDefaultBool(const char * setting) const;
    int32_t LookupDefaultInt(const char * setting) const;
    float LookupDefaultFloat(const char * setting) const;

    mutable std::mutex m_mutex;
    std::condition_variable m_callbackIdle;
    SettingsMapString m_settingsString;
    SettingsMapBool m_settingsBool;
    SettingsMapInt m_settingsInt;
    SettingsMapFloat m_settingsFloat;
    SettingsMapString m_settingsDefaultString;
    SettingsMapBool m_settingsDefaultBool;
    SettingsMapInt m_settingsDefaultInt;
    SettingsMapFloat m_settingsDefaultFloat;
    SettingsMapBool m_settingsChanged;
    NotificationMap m_notification;

    static std::unique_ptr<SettingsStore> s_instance;
    std::string m_configPath;
    JsonValue m_details;
};
