#pragma once
#include "settings_context.h"
#include "about_dialog.h"
#include "advanced_dialog.h"
#include "controller_dialog.h"
#include "directories_dialog.h"
#include "firmware_dialog.h"
#include "game_details_panel.h"
#include "general_dialog.h"
#include "loading_screen.h"
#include "menu_bar.h"
#include "popup_menu.h"
#include "profiles_dialog.h"
#include "settings_page.h"
#include "hotkeys.h"
#include "tooltip.h"
#include "user_profiles.h"
#include "performance_overlay.h"
#include "volume_control.h"
#include <SDL.h>
#include <filesystem>
#include <memory>
#include <nxemu/ui/emulation/emulator_session.h>
#include <nxemu/ui/host/discord_presence.h>
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/ui/library/game_library.h>
#include <nxemu/ui/library/play_time.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rml
{
class Element;
class ElementDocument;
} // namespace Rml

// The main document: title bar, menu bar, game browser, loading and pause screens, status
// bar, and where in the window the game is shown.
class MainWindow final : private EmulatorSession::Listener
{
public:
    MainWindow(UiWindow & window, EmulatorSession & session, GameLibrary & library, std::filesystem::path assetDir);
    ~MainWindow();

    bool Create();
    void OpenGame(const std::string & path);
    void RequestQuit();
    bool QuitRequested() const { return m_quit; }

    void HandleEvent(const SDL_Event & event);
    // Advances the interface by one frame; the window is ready to render afterwards.
    void Update();
    // True while what the window shows only changes when an event arrives:
    // it need not be drawn again until then.
    bool Still() const;
    // Where the game shows through the interface, or null while there is none
    // to show.
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

    struct OverrideAction
    {
        uint64_t programId;
        GameOverride setting;
    };

    // EmulatorSession::Listener
    void OnModulesReleasing() override;
    void OnModulesReady() override;
    void OnGameStarted(const std::string & path) override;
    void OnSessionError(const std::string & message) override;

    void Post(Action action);
    void RunPendingWork();
    void Run(Action action);
    // What each shortcut starts.
    struct HotkeyAction
    {
        HotkeyId id;
        Action action;
    };
    static const HotkeyAction kHotkeyActions[];

    std::string ShortcutLabel(Action action) const;

    void BindEvents();
    void RebuildMenu();
    void ShowGames(std::vector<GameEntry> games);
    void RenderGames();
    void ApplyLibraryOptions();
    void ApplyIconSize();
    void SizeCard(Rml::Element * card) const;
    void RenderVisibleGames();
    std::string CardRml(size_t index);
    std::string ListRowRml(size_t index, size_t number);
    std::string IconRml(const std::string & icon);
    void LoadPendingIcons();
    void ReleaseUnusedIcons(size_t inUse);
    void LayoutList();
    void SaveIconSize();
    void SelectGame(Rml::Element * card);
    const GameEntry * SelectedGame() const;
    void RequestSelectedDetails();
    void RenderDetails();
    void ToggleAddOn(size_t index);
    void UpdateScanIndicator();
    void ShowGameMenu(Rml::Element * origin);
    void SyncPhase();
    void SyncInterfaceLanguage();
    void ShowPanel(Panel panel);
    void SetUiHidden(bool hidden);
    void SetTitle(const std::string & game);
    void ShowVersion();
    void SyncWindowState();
    void LayoutGameArea();
    bool AttachVideo();
    void ResetWindowSize(int width, int height);
    float CardStep() const;
    void FitWindowToCards();
    void AnnounceGame(const std::string & path);
    void UpdateLoadingScreen();
    bool ShowSettingsPage(SettingsPage page);
    void SetDockedMode(bool docked);
    void ShowSelectMenu(Rml::Element * origin);
    void SetMenuRange(int id, int value);
    // Reads the profiles again: they or the active one changed.
    void RefreshProfiles();
    void UpdateStatusWidgets();
    void MeasureGame();
    void UpdateStatusText();
    void RefreshFirmwareVersion();
    bool DialogOpen() const;
    void UpdateMouseCursor();
    void SetText(const char * id, const std::string & text);

    void LoadFile();
    void StopGame();
    bool ConfirmStop(const std::string & title);

    UiWindow & m_window;
    EmulatorSession & m_session;
    GameLibrary & m_library;
    const std::filesystem::path m_assetDir;
    Rml::ElementDocument * m_document = nullptr;
    std::string m_interfaceLanguage;
    std::unique_ptr<MenuBar> m_menuBar;
    std::unique_ptr<PopupMenu> m_gameMenu;
    std::unique_ptr<AboutDialog> m_aboutDialog;
    std::unique_ptr<FirmwareDialog> m_firmwareDialog;
    std::unique_ptr<ControllerDialog> m_controllerDialog;
    std::unique_ptr<GeneralDialog> m_generalDialog;
    std::unique_ptr<AdvancedDialog> m_advancedDialog;
    std::unique_ptr<Tooltip> m_tooltip;
    std::unique_ptr<VolumeControl> m_volume;
    std::unique_ptr<PerformanceOverlay> m_performance;
    bool m_showPerformance = false; // the user wants it over games
    uint64_t m_lastMeasure = 0;     // when the game was last measured
    bool m_longestMeasured = false; // the longest picture was being measured since then
    std::string m_speedText;        // what the status bar says of that measure
    std::unique_ptr<DirectoriesDialog> m_directoriesDialog;
    std::unique_ptr<ProfilesDialog> m_profilesDialog;
    std::vector<UserProfile> m_profiles; // as the status bar shows them
    std::unique_ptr<LoadingScreen> m_loadingScreen;
    std::unique_ptr<GameDetailsPanel> m_detailsPanel;
    PlayTime m_playTime;
    DiscordPresence m_discord;
    unsigned m_hotkeyRevision = 0;  // of the shortcuts the menus show
    std::vector<GameEntry> m_games;
    std::string m_gameSearch;       // what the search field holds
    bool m_fitWindow = true;        // the window still has the size it was created with
    bool m_gamesDirty = false;      // the cards no longer match the search or the order
    std::vector<size_t> m_shownGames; // indices of the games the search keeps, in the chosen order
    bool m_shownDirty = false;        // the elements no longer show m_shownGames
    size_t m_gameColumns = 0;         // games per row of the elements made
    float m_gameRowHeight = 0.0f;     // height of a row of games, in px; 0 until one is measured
    size_t m_firstGameRow = 0;        // the rows that have elements: from this one,
    size_t m_lastGameRow = 0;         // up to this one, excluded
    std::unordered_map<std::string, uint64_t> m_loadedIcons; // the pictures that are textures, and when each was last shown
    uint64_t m_iconUse = 0;           // counts the showings of pictures
    bool m_iconsPending = false;      // some pictures in the document are still to be loaded
    float m_listScrollBar = 0.0f;   // width the list's scroll bar takes from its lines, in px
    int m_iconSize = 0;             // side of the cards' pictures, in dp
    int m_pendingIconSize = 0;      // what the slider was moved to, 0 when it was not
    uint64_t m_iconSizeChanged = 0; // when the size last changed, 0 once it is saved
    Rml::Element * m_selectedGame = nullptr;
    std::string m_selectedPath;     // the selected game, which the search may be hiding
    SettingsContext m_settingsContext;
    GameDetails m_details;          // what its file told, once m_details.path is m_selectedPath
    bool m_detailsAfterStop = false;
    bool m_detailsDirty = false;    // the details panel no longer shows the selected game as it is
    uint64_t m_playingProgram = 0;  // program id of the game being run, to count its play time
    Rml::Element * m_pendingGameMenu = nullptr; // a game or its actions button, to open its menu
    std::optional<Rml::Vector2f> m_gameMenuPoint; // where the right button asked for that menu, which opens there
    Rml::Element * m_pendingSelectMenu = nullptr; // a setting chosen from a menu, to open it
    std::string m_menuGame;                     // path of the game the open menu acts on
    std::vector<Action> m_pendingActions;
    std::vector<OverrideAction> m_pendingOverrides;
    std::string m_pendingGame;
    bool m_launchReady = false;
    std::string m_firmwareOfferedFor; // the pending game whose firmware was offered
    bool m_gameShown = false; // the game being run has presented a frame
    EmulatorSession::Phase m_phase = EmulatorSession::Phase::Idle;
    Panel m_panel = Panel::RomBrowser;
    PixelRect m_gameArea;
    int m_hiddenFrame = 0;
    // What hides the interface, whose way back is said for a moment.
    enum class Hidden { Nothing, Interface, Fullscreen };
    Hidden m_hiddenBy = Hidden::Nothing;
    bool m_hintDue = false;   // the way back is still to be said
    uint64_t m_hintUntil = 0; // when it stops being said; 0 when it is not
    uint64_t m_mouseMovedAt = 0; // when the mouse last moved or clicked
    bool m_mouseHidden = false;
    uint64_t m_versionCopiedUntil = 0; // Copy notification expiry; 0 when hidden.
    bool m_rescanGames = false;
    // Read when it may have changed, not every time the status bar is drawn:
    // reading it opens files of the system storage.
    std::string m_firmwareVersion;
    bool m_menuDirty = true;
    bool m_hideUi = false;
    bool m_applyStartOptions = false;
    bool m_quit = false;
    uint64_t m_lastStatusUpdate = 0;
    uint64_t m_scanIndicatorTime = 0;
    uint64_t m_scanReadingSince = 0;
    float m_scanIndicatorShown = 0.0f; // fraction of the bar that is filled
};
