#pragma once
#include "dialog.h"
#include "settings_page.h"
#include "settings_context.h"
#include <functional>
#include <nxemu/ui/host/ui_renderer.h>
#include <string>
#include <vector>

class EmulatorSession;
class UiWindow;

// The page of the settings everyone needs: the few that decide how games
// look, sound and run. A change is kept as it is made.
class GeneralDialog final : public Dialog
{
public:
    struct Host
    {
        // A setting the window shows elsewhere changed.
        std::function<void()> changed;
        // Shows another page of the settings in place of this one.
        OpenSettingsPage openPage;
    };

    GeneralDialog(UiWindow & window, EmulatorSession & session, SettingsContext & settings, const std::filesystem::path & assetDir, Host host);
    ~GeneralDialog();
    void RefreshLanguage();

private:
    void OnOpen() override;
    void OnUpdate() override;
    void OnCommand(const std::string & command) override;
    void OnCloseRequested() override;

    void FillLists();
    void ShowValues();
    void ShowDevice();
    void ShowRunning();
    void ApplyChange(const std::string & id);
    void Changed();
    // Whether the user chose the region, and the one it is otherwise.
    bool RegionChosen() const;
    int LanguageRegion() const;

    UiWindow & m_window;
    EmulatorSession & m_session;
    SettingsContext & m_settings;
    const Host m_host;
    std::vector<UiRenderer::GraphicsDevice> m_devices; // in the order of the list
    size_t m_profileCount = 0;
    std::vector<std::string> m_changes;                // ids of the controls changed since the last frame
    bool m_unsaved = false;                            // a change is not in the settings file yet
};
