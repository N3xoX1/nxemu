#pragma once
#include "dialog.h"
#include <functional>
#include <nxemu/ui/library/firmware_installer.h>

class UiWindow;

class FirmwareDialog final : public Dialog
{
public:
    struct Host
    {
        std::function<void()> installStarting;
        std::function<void()> installEnded;
    };

    FirmwareDialog(UiWindow & window, ISystemloader & loader, const std::filesystem::path & assetDir, Host host);

    void SetLoader(ISystemloader & loader) { m_loader = &loader; }
    // Synchronous lifetime boundary before SystemModules::ShutDown().
    void UnbindLoader();

    bool Installing() const { return m_installer.Busy(); }
    void CancelInstall() { m_installer.Cancel(); }
    bool OfferSource(const std::string & utf8Path);
    void OfferFromGame(const std::string & utf8Path, const std::string & version);

private:
    void OnOpen() override;
    void OnUpdate() override;
    void OnCommand(const std::string & command) override;
    void OnCloseRequested() override;

    void Install(const std::string & utf8Path);
    void ShowStep(const char * step, bool failed);
    void ShowInstalledVersion();
    void ShowProgress(const FirmwareInstaller::State & state);
    void ShowResult(const FirmwareInstaller::State & state);

    UiWindow & m_window;
    ISystemloader * m_loader;
    const Host m_host;
    FirmwareInstaller m_installer;
    std::string m_nextGame, m_nextGameVersion;
    std::string m_game, m_gameVersion;
    FirmwareInstaller::Phase m_shownPhase = FirmwareInstaller::Phase::Idle;
    uint64_t m_stepShown = 0;
};
