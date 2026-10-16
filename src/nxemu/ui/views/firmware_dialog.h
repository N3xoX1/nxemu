#pragma once
#include "dialog.h"
#include <functional>
#include <nxemu/ui/library/firmware_installer.h>

class UiWindow;

// Guides the user through installing the console's firmware: choosing the
// package, following the installation, and what came of it. Opened for a game
// that carries a firmware, it offers that one in place of the choice.
class FirmwareDialog final : public Dialog
{
public:
    struct Host
    {
        // Called before an installation starts and after it ends: nothing
        // else may use the loader in between.
        std::function<void()> installStarting;
        std::function<void()> installEnded;
    };

    FirmwareDialog(UiWindow & window, ISystemloader & loader, const std::filesystem::path & assetDir, Host host);

    // Rebind after the session replaces its emulator modules.
    void SetLoader(ISystemloader & loader) { m_loader = &loader; }

    bool Installing() const { return m_installer.Busy(); }
    // Stops an installation in progress; it ends shortly after.
    void CancelInstall() { m_installer.Cancel(); }
    // Installs from a file or a directory dropped on the window. Returns
    // false when the dialog is not waiting for a source.
    bool OfferSource(const std::string & utf8Path);
    // Opens on the offer to install the firmware a game card image carries,
    // of the version given, before the game starts.
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
    std::string m_nextGame, m_nextGameVersion; // what the dialog is being opened to offer
    std::string m_game, m_gameVersion;         // what it was opened to offer: the game waits for it to close
    FirmwareInstaller::Phase m_shownPhase = FirmwareInstaller::Phase::Idle;
    uint64_t m_stepShown = 0; // when the step on screen appeared, in SDL ticks
};
