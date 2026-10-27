#pragma once
#include <nxemu-module-spec/base.h>
#include <atomic>
#include <condition_variable>
#include <vector>
#include <cstdint>
#include <mutex>
#include <common/json.h>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

class SettingsStore
{
    struct CallbackInfo
    {
        SettingChangeCallback callback;
        void * userData;
        unsigned inFlight = 0; // guarded by m_notificationMutex
        bool removed = false;
    };

    typedef std::unordered_map<std::string, std::string> SettingsMapString;
    typedef std::unordered_map<std::string, bool> SettingsMapBool;
    typedef std::unordered_map<std::string, int32_t> SettingsMapInt;
    typedef std::unordered_map<std::string, float> SettingsMapFloat;
    typedef std::vector<std::shared_ptr<CallbackInfo>> NotificationCallbacks;
    typedef std::unordered_map<std::string, NotificationCallbacks> NotificationMap;

public:
    struct DiskCacheProgress
    {
        int32_t stage = -1;
        int32_t current = 0;
        int32_t total = 0;
        int32_t tick = 0;
    };

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
    // Non-blocking coherent snapshot; returns false during a concurrent publication.
    bool TryGetDiskCacheProgress(DiskCacheProgress & out) const;
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
    // External removal drains active calls. Self-removal cancels future calls;
    // userData must outlive any other concurrent invocation. Subscribers must
    // not synchronously unregister each other in a cross-thread cycle.
    void UnregisterCallback(const std::string & setting, SettingChangeCallback callback, void * userData);

    static SettingsStore& GetInstance();
    static void CleanUp();

private:
    void NotifyChange(const char * setting);
    std::atomic<bool> * RuntimeBool(const char * setting) const;
    std::atomic<int32_t> * RuntimeInt(const char * setting) const;

    // Only transient, cross-thread status values are atomic. Persistent settings,
    // JSON data and borrowed GetString() values keep their existing ownership.
    mutable std::atomic<bool> m_emulationRunning{false};
    mutable std::atomic<bool> m_displayedFrames{false};
    mutable std::atomic<int32_t> m_emulationState{0};
    mutable std::atomic<int32_t> m_diskCacheStage{-1};
    mutable std::atomic<int32_t> m_diskCacheCurrent{0};
    mutable std::atomic<int32_t> m_diskCacheTotal{0};
    // Even = published, odd = writer updating the three fields.
    mutable std::atomic<int32_t> m_diskCacheTick{0};
    mutable std::mutex m_notificationMutex;
    std::condition_variable m_notificationIdle;
    std::atomic<bool> m_hasCallbacks{false};

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
