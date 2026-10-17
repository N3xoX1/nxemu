#pragma once
#include "menu_bar.h"
#include "popup_menu.h"
#include <SDL.h>
#include <filesystem>
#include <memory>
#include <nxemu/ui/emulation/emulator_session.h>
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/ui/library/game_library.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rml
{
class Element;
class ElementDocument;
} // namespace Rml

class MainWindow final : private EmulatorSession::Listener
{
public:
    MainWindow(UiWindow & window, EmulatorSession & session, GameLibrary & library, std::filesystem::path assetDir);
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
    void AddGames(std::vector<FoundGame> found);
    void RenderGames();
    void RenderVisibleGames();
    std::string IconRml(const std::string & icon);
    void LoadPendingIcons();
    void ReleaseUnusedIcons(size_t inUse);
    std::string CardRml(size_t index);
    void ApplyIconSize();
    void SizeCard(Rml::Element * card) const;
    float CardStep() const;
    void SelectGame(Rml::Element * card);
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
    GameLibrary & m_library;
    const std::filesystem::path m_assetDir;
    Rml::ElementDocument * m_document = nullptr;
    std::unique_ptr<MenuBar> m_menuBar;
    std::unique_ptr<PopupMenu> m_gameMenu;
    std::vector<Action> m_pendingActions;
    std::vector<GameEntry> m_games;
    std::string m_gameSearch;
    bool m_gamesDirty = false;
    bool m_gamesScanning = false;
    std::vector<size_t> m_shownGames;
    bool m_shownDirty = false;
    size_t m_gameColumns = 0;
    float m_gameRowHeight = 0.0f;
    size_t m_firstGameRow = 0;
    size_t m_lastGameRow = 0;
    std::unordered_map<std::string, uint64_t> m_loadedIcons;
    uint64_t m_iconUse = 0;
    bool m_iconsPending = false;
    int m_iconSize;
    Rml::Element * m_selectedGame = nullptr;
    std::string m_selectedPath;
    uint64_t m_selectedProgramId = 0;
    Panel m_panel = Panel::RomBrowser;
    bool m_menuDirty = true;
    int m_hiddenFrame = 0;
    uint64_t m_versionCopiedUntil = 0;
    bool m_quit = false;
};
