#pragma once
#include "menu_bar.h"
#include "popup_menu.h"
#include <SDL.h>
#include <filesystem>
#include <memory>
#include <nxemu/ui/emulation/emulator_session.h>
#include <nxemu/ui/host/ui_window.h>
#include <string>
#include <vector>

namespace Rml
{
class Element;
class ElementDocument;
} // namespace Rml

class MainWindow final : private EmulatorSession::Listener
{
public:
    MainWindow(UiWindow & window, EmulatorSession & session, std::filesystem::path assetDir);
    ~MainWindow();

    bool Create();
    void RequestQuit();
    bool QuitRequested() const { return m_quit; }

    void HandleEvent(const SDL_Event & event);
    void Update();
    bool Still() const;
    const PixelRect * GameArea() const;

private:
    MainWindow(const MainWindow &) = delete;
    MainWindow & operator=(const MainWindow &) = delete;

    enum class Action : int
    {
        LoadFile = 1,
        Exit,
        OpenAppDirectory,
        OpenLogDirectory,
        PauseOrContinue,
        Stop,
        ToggleSpeedLimit,
        ToggleStartGamesInFullscreen,
        ToggleStartGamesWithUiHidden,
        ToggleFullscreen,
        ExitFullscreen,
        ToggleHideUi,
        TogglePerformance,
        // One for each PerformanceItem, in their order.
        TogglePerformanceHistory,
        TogglePerformanceRange,
        TogglePerformanceFrameTime,
        TogglePerformanceLongest,
        TogglePerformanceSpeed,
        TogglePerformanceProcessor,
        TogglePerformanceMemory,
        ToggleDockedMode,
        ResetWindowSize720p,
        ResetWindowSize900p,
        ResetWindowSize1080p,
        OpenSettings,
        ConfigureSelectedGame,
        OpenProfiles,
        OpenGameDirectories,
        OpenDiscord,
        OpenReportIssues,
        OpenAbout,
        InstallFirmware,
        ConfigureControllers,
        StartMenuGame,
        OpenMenuGameDirectory,
        CopyMenuGamePath,
        ConfigureMenuGame,
        OpenMenuGameSaves,
        OpenMenuGameCache,
        ClearMenuGameCache,
        ResetVolumeOverrides,
        SetDockedMode,
        SetHandheldMode,
        ToggleGameDetails,
        ToggleCardInformation,
        ShowGridView,
        ShowListView,
        TogglePlayTime,
        ToggleCardTitle,
        PlaySelectedGame,
        OpenSelectedSaveDirectory,
        OpenSelectedCacheDirectory,
        OpenSelectedModDirectory,
        CopyVersion,
        ProfileFirst = 880,         // one per profile, to play as it
        ResolutionScaleFirst = 900, // one per ResolutionSetup value
        GameSortFirst = 920,        // one per way of sorting the games
        ScalingFilterFirst = 940,   // one per ScalingFilter value
        AddOnFirst = 960,           // one per add-on of the selected game, to switch it
        RecentFileFirst = 1000,
    };

    enum class Panel
    {
        RomBrowser,
        Loading,
        Pause,
        Renderer,
    };

    // EmulatorSession::Listener
    void OnModulesReleasing() override;
    void OnModulesReady() override;
    void OnGameStarted(const std::string & path) override;
    void OnSessionError(const std::string & message) override;

    void Post(Action action);
    void RunPendingWork();
    void Run(Action action);
    std::string ShortcutLabel(Action action) const;
    void BindEvents();
    void RebuildMenu();
    void ShowPanel(Panel panel);
    void SetTitle(const std::string & game);
    void ShowVersion();
    void SyncWindowState();
    bool AttachVideo();
    void ResetWindowSize(int width, int height);
    void UpdateStatusText();
    void SetText(const char * id, const std::string & text);

    UiWindow & m_window;
    EmulatorSession & m_session;
    const std::filesystem::path m_assetDir;
    Rml::ElementDocument * m_document = nullptr;
    std::unique_ptr<MenuBar> m_menuBar;
    std::unique_ptr<PopupMenu> m_gameMenu;
    std::vector<Action> m_pendingActions;
    Panel m_panel = Panel::RomBrowser;
    bool m_menuDirty = true;
    int m_hiddenFrame = 0;
    uint64_t m_versionCopiedUntil = 0;
    bool m_quit = false;
};
