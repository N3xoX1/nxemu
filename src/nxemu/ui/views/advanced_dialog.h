#pragma once
#include "dialog.h"
#include "settings_page.h"
#include "settings_context.h"
#include <SDL.h>
#include <string>
#include <vector>

class EmulatorSession;
class UiWindow;

// The page of the settings few need: what tunes the graphics, the system and
// NXEmu itself, in tabs. A change is kept as it is made.
class AdvancedDialog final : public Dialog
{
public:
    AdvancedDialog(UiWindow & window, EmulatorSession & session, SettingsContext & settings, const std::filesystem::path & assetDir, OpenSettingsPage openPage);

    // Takes the key when a shortcut is being chosen: true when it did.
    bool HandleKey(const SDL_KeyboardEvent & event);

private:
    void OnOpen() override;
    void OnUpdate() override;
    void OnCommand(const std::string & command) override;
    void OnCloseRequested() override;

    void FillLists();
    void ShowTab();
    void ShowValues();
    void ApplyChange(const std::string & id);
    void ApplyLogFilter();
    void ShowHotkeys();
    void RestoreDefaults();

    EmulatorSession & m_session;
    SettingsContext & m_settings;
    const OpenSettingsPage m_openPage; // shows another page of the settings in place of this one
    std::string m_tab = "graphics";    // the one shown, kept from one opening to the next
    std::vector<std::string> m_changes; // ids of the controls changed since the last frame
    bool m_logFilterLeft = false;      // the log filter was left or validated
    bool m_running = false;            // a game was running at the last frame
    int m_hotkeyAsked = -1;            // index of the shortcut waiting for its key, or -1
    std::string m_hotkeyNote;          // why the last key was not taken
    bool m_unsaved = false;            // a change is not in the settings file yet
};
