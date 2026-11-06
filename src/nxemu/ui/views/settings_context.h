#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace Rml { class ElementDocument; }
class EmulatorSession;

const char * GameSettingId(const char * key);

using SettingChangeable = std::function<bool(const char * setting)>;

class RunningGameSettings
{
public:
    int32_t GetInt(const char * key) const;
    bool GetBool(const char * key) const;
    void SetInt(const char * key, int32_t value);
    void SetBool(const char * key, bool value);
};

class SettingsContext
{
public:
    explicit SettingsContext(EmulatorSession & session);

    void Global();
    bool Game(uint64_t programId, std::string title);
    bool CanConfigure(uint64_t programId) const;
    bool IsGame() const { return m_programId != 0; }
    std::string Title() const;
    int32_t GetInt(const char * key) const;
    bool GetBool(const char * key) const;
    const char * GetString(const char * key) const;
    int32_t GetDefaultInt(const char * key) const;
    bool GetDefaultBool(const char * key) const;
    const char * GetDefaultString(const char * key) const;
    void SetInt(const char * key, int32_t value);
    void SetBool(const char * key, bool value);
    void SetString(const char * key, const char * value);
    bool InheritCommand(const std::string & command, const SettingChangeable & changeable);
    void InheritAll(const SettingChangeable & changeable);
    void Decorate(Rml::ElementDocument * document, const SettingChangeable & changeable) const;

private:
    bool Editable() const;

    EmulatorSession & m_session;
    uint64_t m_programId = 0;
    std::string m_title;
    bool m_titleIdSet = false;
};
