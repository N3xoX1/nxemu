#pragma once
#include <nxemu-core/settings/game_settings.h>
#include <string>

namespace Rml { class ElementDocument; }

// Core setting labels and choices use shorter names than the UI controls.
// Translate them with the same vocabulary as the settings pages.
std::string GameSettingText(const std::string & text);

// The same controls edit either global settings or one game's user layer.
// Per-game changes are persisted atomically and take effect on next launch.
class SettingsContext
{
public:
    void Global();
    void Game(uint64_t programId, std::string title);
    bool TakeChanges() { bool changed = m_changed; m_changed = false; return changed; }
    bool IsGame() const { return m_programId != 0; }
    int32_t GetInt(const char * key) const;
    bool GetBool(const char * key) const;
    const char * GetString(const char * key) const;
    int32_t GetDefaultInt(const char * key) const;
    bool GetDefaultBool(const char * key) const;
    const char * GetDefaultString(const char * key) const;
    void SetInt(const char * key, int32_t value);
    void SetBool(const char * key, bool value);
    void SetString(const char * key, const char * value);
    bool InheritCommand(const std::string & command);
    void InheritAll();
    void Decorate(Rml::ElementDocument * document) const;

private:
    bool m_changed = false;
    uint64_t m_programId = 0;
    std::string m_title;
    std::string m_error;
};
