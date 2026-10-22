#pragma once
#include <nxemu-module-spec/base.h>
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
    };

    typedef std::unordered_map<std::string, std::string> SettingsMapString;
    typedef std::unordered_map<std::string, bool> SettingsMapBool;
    typedef std::unordered_map<std::string, int32_t> SettingsMapInt;
    typedef std::unordered_map<std::string, float> SettingsMapFloat;
    typedef std::vector<CallbackInfo> NotificationCallbacks;
    typedef std::unordered_map<std::string, NotificationCallbacks> NotificationMap;

public:
    SettingsStore();

    // Effective reads normally include the immutable snapshot of a running
    // game's overrides. Editors and module serializers read the global layer.
    // The scope is thread local: saving never changes what emulation reads.
    class GlobalReadScope
    {
    public:
        GlobalReadScope();
        ~GlobalReadScope();
        GlobalReadScope(const GlobalReadScope &) = delete;
        GlobalReadScope & operator=(const GlobalReadScope &) = delete;
    private:
        bool m_previous;
    };
    void SetGameOverrides(JsonValue overrides); // only while modules are stopped
    // Explicit frontend controls take priority over the game's launch snapshot.
    // GlobalReadScope still exposes the underlying global settings for saving.
    void SetControlOverride(const char * setting, JsonValue value);
    void ClearControlOverride(const char * setting);
    bool HasControlOverride(const char * setting) const;
    // What a setting would be without its control override.
    int32_t GetIntWithoutControl(const char * setting) const;
    bool GetBoolWithoutControl(const char * setting) const;

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
    void UnregisterCallback(const std::string & setting, SettingChangeCallback callback, void * userData);

    static SettingsStore& GetInstance();
    static void CleanUp();

private:
    void NotifyChange(const char * setting);

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
    JsonValue m_gameOverrides;
    JsonValue m_controlOverrides;
    static thread_local bool s_globalReads;
};
