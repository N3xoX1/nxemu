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

class GeneralDialog final : public Dialog
{
public:
    struct Host
    {
        std::function<void()> changed;
        OpenSettingsPage openPage;
    };

    GeneralDialog(UiWindow & window, EmulatorSession & session, SettingsContext & settings, const std::filesystem::path & assetDir, Host host);
    ~GeneralDialog();

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

    UiWindow & m_window;
    EmulatorSession & m_session;
    SettingsContext & m_settings;
    const Host m_host;
    std::vector<UiRenderer::GraphicsDevice> m_devices;
    size_t m_profileCount = 0;
    std::vector<std::string> m_changes;
    bool m_unsaved = false;
};
