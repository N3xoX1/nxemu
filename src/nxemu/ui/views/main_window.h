#pragma once
#include "about_dialog.h"
#include "advanced_dialog.h"
#include "controller_dialog.h"
#include "directories_dialog.h"
#include "firmware_dialog.h"
#include "general_dialog.h"
#include "game_details_panel.h"
#include "hotkeys.h"
#include "loading_screen.h"
#include "menu_bar.h"
#include "popup_menu.h"
#include "profiles_dialog.h"
#include "settings_context.h"
#include "performance_overlay.h"
#include "settings_page.h"
#include "tooltip.h"
#include "user_profiles.h"
#include "volume_control.h"
#include <SDL.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <nxemu/ui/emulation/emulator_session.h>
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/ui/library/game_library.h>
#include <nxemu/ui/host/discord_presence.h>
#include <nxemu/ui/library/play_time.h>
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

    bool Create(const std::function<bool()> & pump = {});
    void OpenGame(const std::string & path);
    void RequestQuit();
    void RevealMouseCursor();
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
        Restart,
        ToggleEmulationControls,
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
    struct HotkeyAction
    {
        HotkeyId id;
        Action action;
    };
    static const HotkeyAction kHotkeyActions[];

    std::string ShortcutLabel(Action action) const;
    void BindEvents();
    void RebuildMenu();
    void AddGames(std::vector<FoundGame> found);
    void RescanGames();
    void FinishRescan();
    void ReleaseIcons(const std::vector<GameEntry> & games);
    void UpdateScanIndicator();
    void RenderGames();
    void RenderVisibleGames();
    std::string IconRml(const std::string & icon);
    void LoadPendingIcons();
    void ReleaseUnusedIcons(size_t inUse);
    std::string CardRml(size_t index);
    std::string ListRowRml(size_t index, size_t number);
    void ApplyLibraryOptions();
    void LayoutList();
    void SaveIconSize();
    void FitWindowToCards();
    void ShowSelectMenu(Rml::Element * origin);
    void ShowGameMenu(Rml::Element * origin);
    void SetMenuRange(int id, int value);
    void SetDockedMode(bool docked);
    void RefreshProfiles();
    void UpdateStatusWidgets();
    void ApplyIconSize();
    void SizeCard(Rml::Element * card) const;
    float CardStep() const;
    void SelectGame(Rml::Element * card);
    const GameEntry * SelectedGame() const;
    void RequestSelectedDetails();
    void RenderDetails();
    void ToggleAddOn(size_t index);
    void SyncPhase();
    void SyncInterfaceLanguage();
    void UpdateDiscord();
    static void DiscordSettingChanged(const char * setting, void * userData);
    void ShowPanel(Panel panel);
    void SetUiHidden(bool hidden);
    void LoadFile();
    void RefreshFirmwareVersion();
    bool DialogOpen() const;
    void AnnounceGame(const std::string & path);
    void UpdateLoadingScreen();
    void LayoutGameArea();
    void StopGame();
    bool ShowSettingsPage(SettingsPage page);
    bool EnsureUtilityGuest(bool preserveProfiles = false);
    void SetTitle(const std::string & game);
    void ShowVersion();
    void SyncWindowState();
    void ResetWindowSize(int width, int height);
    void MeasureGame();
    void UpdateStatusText();
    void UpdateMouseCursor();
    void SetText(const char * id, const std::string & text);

    bool ConfirmStop(const std::string & title);

    UiWindow & m_window;
    EmulatorSession & m_session;
    GameLibrary & m_library;
    const std::filesystem::path m_assetDir;
    std::string m_interfaceLanguage;
    Rml::ElementDocument * m_document = nullptr;
    std::unique_ptr<MenuBar> m_menuBar;
    std::unique_ptr<PopupMenu> m_gameMenu;
    std::unique_ptr<LoadingScreen> m_loadingScreen;
    std::unique_ptr<Tooltip> m_tooltip;
    std::unique_ptr<PerformanceOverlay> m_performance;
    std::unique_ptr<GameDetailsPanel> m_detailsPanel;
    GameDetails m_details;
    bool m_detailsDirty = false;
    bool m_detailsReload = false;
    bool m_longestMeasured = false;
    std::unique_ptr<VolumeControl> m_volume;
    std::vector<UserProfile> m_profiles;
    uint64_t m_lastStatusUpdate = 0;
    SettingsContext m_settingsContext;
    std::unique_ptr<AboutDialog> m_aboutDialog;
    std::unique_ptr<FirmwareDialog> m_firmwareDialog;
    std::string m_firmwareVersion;
    std::string m_firmwareOfferedFor;
    std::unique_ptr<GeneralDialog> m_generalDialog;
    std::unique_ptr<AdvancedDialog> m_advancedDialog;
    std::unique_ptr<ControllerDialog> m_controllerDialog;
    std::unique_ptr<DirectoriesDialog> m_directoriesDialog;
    std::unique_ptr<ProfilesDialog> m_profilesDialog;
    std::vector<Action> m_pendingActions;
    std::vector<GameEntry> m_games;
    std::string m_gameSearch;
    bool m_gamesDirty = false;
    bool m_gamesScanning = false;
    bool m_rescanning = false;
    std::vector<GameEntry> m_nextGames;
    uint64_t m_scanIndicatorTime = 0;
    uint64_t m_scanReadingSince = 0;
    float m_scanIndicatorShown = 0.0f;
    std::vector<size_t> m_shownGames;
    bool m_shownDirty = false;
    size_t m_gameColumns = 0;
    float m_gameRowHeight = 0.0f;
    size_t m_firstGameRow = 0;
    size_t m_lastGameRow = 0;
    std::unordered_map<std::string, uint64_t> m_loadedIcons;
    uint64_t m_iconUse = 0;
    bool m_iconsPending = false;
    int m_iconSize = 0;
    int m_pendingIconSize = 0;
    uint64_t m_iconSizeChanged = 0;
    float m_listScrollBar = 0.0f;
    bool m_fitWindow = true;
    Rml::Element * m_selectedGame = nullptr;
    std::string m_selectedPath;
    uint64_t m_selectedProgramId = 0;
    Rml::Element * m_pendingGameMenu = nullptr;
    std::optional<Rml::Vector2f> m_gameMenuPoint;
    std::string m_menuGame;
    Rml::Element * m_pendingSelectMenu = nullptr;
    std::string m_pendingGame;
    bool m_launchReady = false;
    bool m_gameShown = false;
    bool m_rescanGames = false;
    bool m_utilityGuest = false;
    bool m_keepProfileDialogDuringSetup = false;
    PixelRect m_gameArea{};
    PlayTime m_playTime;
    DiscordPresence m_discord;
    uint64_t m_playingProgram = 0;
    EmulatorSession::Phase m_phase = EmulatorSession::Phase::Idle;
    Panel m_panel = Panel::RomBrowser;
    bool m_menuDirty = true;
    unsigned m_hotkeyRevision = 0;
    uint64_t m_lastMeasure = 0;
    std::string m_speedText;
    int m_hiddenFrame = 0;
    uint64_t m_mouseMovedAt = 0;
    bool m_mouseHidden = false;
    enum class Hidden
    {
        Nothing,
        Interface,
        Fullscreen,
    };
    Hidden m_hiddenBy = Hidden::Nothing;
    bool m_hintDue = false;
    uint64_t m_hintUntil = 0;
    uint64_t m_versionCopiedUntil = 0;
    bool m_hideUi = false;
    bool m_applyStartOptions = false;
    bool m_quit = false;
};
