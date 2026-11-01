#pragma once
#include "dialog.h"
#include "hotkeys.h"
#include "settings_page.h"
#include "settings_context.h"
#include <SDL.h>
#include <string>
#include <vector>

class EmulatorSession;
class UiWindow;

class AdvancedDialog final : public Dialog
{
public:
    AdvancedDialog(UiWindow & window, EmulatorSession & session, SettingsContext & settings, const std::filesystem::path & assetDir, OpenSettingsPage openPage);

    void ShowPage(SettingsPage page);
    void RefreshLanguage();
    bool HandleKey(const SDL_KeyboardEvent & event);
    bool HandleGamepad(const SDL_Event & event, const Hotkeys::GamepadShortcut & chord);

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
    const OpenSettingsPage m_openPage;
    std::string m_tab = "graphics";
    std::string m_debugTab = "debug-graphics";
    bool m_debug = false;
    std::vector<std::string> m_changes;
    bool m_logFilterLeft = false;
    bool m_running = false;
    int m_hotkeyAsked = -1;
    bool m_hotkeyGamepad = false;
    SDL_JoystickID m_hotkeyDevice = -1;
    std::string m_hotkeyNote;
    bool m_unsaved = false;
};
