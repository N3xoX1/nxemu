#pragma once

#include "module_base.h"
#include <string>

class ModuleSettings :
    public IModuleSettings
{
public:
    //IModuleSettings
    const char * GetString(const char * setting) const override;
    bool GetBool(const char * setting) const override;
    int32_t GetInt(const char * setting) const override;
    float GetFloat(const char * setting) const override;
    void SetString(const char * setting, const char * value) override;
    void SetBool(const char * setting, bool value) override;
    void SetInt(const char * setting, int32_t value) override;
    void SetFloat(const char * setting, float value) override;

    void SetDefaultBool(const char * setting, bool value) override;
    void SetDefaultInt(const char * setting, int32_t value) override;
    void SetDefaultFloat(const char * setting, float value) override;
    void SetDefaultString(const char * setting, const char * value) override;

    const char * GetSectionSettings(const char * section) const override;
    void SetSectionSettings(const char * section, const char * json) override;

    void RegisterCallback(const char * setting, SettingChangeCallback callback, void * userData) override;
    void UnregisterCallback(const char * setting, SettingChangeCallback callback, void * userData) override;

private:
    mutable std::string m_sectionSetting;
};