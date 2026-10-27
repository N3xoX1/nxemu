#include "main_window.h"
#include <nxemu/ui/i18n/translation.h>
#include "resolution_scales.h"
#include "rml_events.h"
#include "scaling_filters.h"
#include <RmlUi/Core.h>
#include <algorithm>
#include <cmath>
#include <common/path.h>
#include <common/std_string.h>
#include <nxemu-core/notification.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/version.h>
#include <nxemu-module-spec/cpu.h>
#include <nxemu-module-spec/operating_system.h>
#include <nxemu-cpu/cpu_settings_identifiers.h>
#include <nxemu-module-spec/video.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu-os/os_types.h>
#include <nxemu-video/video_settings_identifiers.h>
#include <nxemu/settings/ui_identifiers.h>
#include <nxemu/settings/ui_settings.h>
#include <nxemu/ui/host/shell.h>
#include <nxemu/ui/host/ui_renderer.h>
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/ui/library/game_cache.h>
#include <nxemu/ui/library/firmware_avatars.h>
#include <nxemu/ui/library/game_details.h>
#include <nxemu/ui/library/interface_language.h>
#include <nxemu/ui/platform/file_dialogs.h>
#include <tuple>
#include <yuzu_common/fs/filesystem_interfaces.h>
#include <yuzu_common/fs/path_util.h>

namespace
{
constexpr const char * kDiscordUrl = "https://discord.gg/hEa4hNyFWU";
constexpr const char * kReportUrl = "https://report.nxemu.com/";
constexpr float kNarrowWidth = 800.0f;
constexpr float kCompactWidth = 560.0f;
constexpr uint64_t kVersionCopiedMs = 1500;
constexpr uint64_t kStatusIntervalMs = 1000;
constexpr int kGameIconMin = 96;
constexpr int kGameIconMax = 240;
constexpr int kGameIconSteps[] = {108, 146, 176, 216};
constexpr int kGameIconStepCount = static_cast<int>(sizeof(kGameIconSteps) / sizeof(kGameIconSteps[0]));
constexpr int kCardGap = 16; // between cards, and 10 between the smallest: see the style sheet
constexpr int kSmallCardGap = 10;
constexpr int kCardBorder = 2;           // .rom-card's, on each side
constexpr size_t kUnmeasuredRows = 3;    // rows of games shown before the height of one is known
constexpr size_t kIconsPerFrame = 2;     // pictures decoded in one frame, each taking a millisecond or two
constexpr size_t kMaxLoadedIcons = 256;  // pictures kept as textures, unless more are on screen at once
constexpr size_t kMinMarginRows = 2;     // rows kept ready beyond those in sight, each way
constexpr int kCoverTitleDelayMs = 1200; // a cover names its game to a mouse that stays
constexpr int kScrollbarWidth = 12;
constexpr uint64_t kIconSizeSaveDelayMs = 400;
constexpr int kFilterSharpnessRange = 1;
constexpr float kScanStepShare = 0.85f;
constexpr float kScanEaseSeconds = 0.7f;
constexpr uint64_t kScanIndicatorDelayMs = 250;
constexpr int kPerformanceSizeRange = 2;
constexpr int kPerformanceRateRange = 3;
constexpr int kPerformanceLengthRange = 4;
constexpr uint64_t kGameHintMs = 4000;
constexpr uint64_t kMouseHideMs = 2500;

const char * const kGameSorts[] = {TrMark("Name"), TrMark("Publisher"), TrMark("Recent"), TrMark("Play time"), TrMark("Size")};
constexpr int kGameSortCount = static_cast<int>(sizeof(kGameSorts) / sizeof(kGameSorts[0]));
constexpr int kSortByName = 0;
constexpr int kSortByPublisher = 1;
constexpr int kSortByRecentlyPlayed = 2;
constexpr int kSortByPlayTime = 3;
constexpr int kSortBySize = 4;
constexpr int kOfferedSorts[] = {kSortByName, kSortByPublisher, kSortByRecentlyPlayed, kSortByPlayTime, kSortBySize};

struct ListColumn
{
    const char * hiddenClass;
    float minWidth;
};
constexpr ListColumn kOptionalListColumns[] = {
    {"hide-size", 930.0f}, {"hide-last", 820.0f}, {"hide-publisher", 700.0f}, {"hide-format", 560.0f},
    {"hide-number", 480.0f}, {"hide-version", 410.0f}, {"hide-play-label", 400.0f},
};

int GameIconSize()
{
    return std::clamp(static_cast<int>(uiSettings.gameIconSize), kGameIconMin, kGameIconMax);
}


bool IsDocked()
{
    return SettingsStore::GetInstance().GetInt(NXOsSetting::DockedMode) == static_cast<int32_t>(DockedMode::Docked);
}

int ResolutionScale()
{
    const int scale = SettingsStore::GetInstance().GetInt(NXVideoSetting::ResolutionSetup);
    return scale >= 0 && scale < kResolutionScaleCount ? scale : static_cast<int>(ResolutionSetup::Res1X);
}

int GameSort()
{
    const int sort = uiSettings.gameSort;
    return std::find(std::begin(kOfferedSorts), std::end(kOfferedSorts), sort) != std::end(kOfferedSorts) ? sort : kSortByName;
}

std::string FileFormat(const std::string & utf8Path)
{
    std::string extension = Common::FS::PathToUTF8String(std::filesystem::path{Common::FS::ToU8String(utf8Path)}.extension());
    extension.erase(0, extension.find_first_not_of('.'));
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    return extension;
}

std::string FileSize(uint64_t bytes)
{
    if (bytes == 0)
    {
        return {};
    }
    constexpr double kMegabyte = 1024.0 * 1024.0;
    constexpr double kGigabyte = kMegabyte * 1024.0;
    return bytes >= kGigabyte ? Tr("%1 GB", {stdstr_f("%.2f", bytes / kGigabyte)}) : Tr("%1 MB", {stdstr_f("%.0f", bytes / kMegabyte)});
}

int GameIconStep(int size)
{
    int step = 1;
    while (step < kGameIconStepCount && size >= (kGameIconSteps[step - 1] + kGameIconSteps[step]) / 2)
    {
        step++;
    }
    return step;
}

std::string Folded(const std::string & text)
{
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return result;
}

std::string FileName(const std::string & utf8Path)
{
    return Common::FS::PathToUTF8String(std::filesystem::path{Common::FS::ToU8String(utf8Path)}.filename());
}
} // namespace

const MainWindow::HotkeyAction MainWindow::kHotkeyActions[] = {
    {HotkeyId::LoadFile, Action::LoadFile},
    {HotkeyId::Settings, Action::OpenSettings},
    {HotkeyId::Controllers, Action::ConfigureControllers},
    {HotkeyId::Exit, Action::Exit},
    {HotkeyId::Fullscreen, Action::ToggleFullscreen},
    {HotkeyId::ExitFullscreen, Action::ExitFullscreen},
    {HotkeyId::HideUi, Action::ToggleHideUi},
    {HotkeyId::Performance, Action::TogglePerformance},
    {HotkeyId::PauseOrContinue, Action::PauseOrContinue},
    {HotkeyId::Stop, Action::Stop},
    {HotkeyId::DockedMode, Action::ToggleDockedMode},
    {HotkeyId::SpeedLimit, Action::ToggleSpeedLimit},
};

MainWindow::MainWindow(UiWindow & window, EmulatorSession & session, GameLibrary & library, std::filesystem::path assetDir) :
    m_window(window),
    m_session(session),
    m_library(library),
    m_assetDir(std::move(assetDir)),
    m_playTime(Common::FS::GetYuzuPath(Common::FS::YuzuPath::PlayTimeDir) / "play_time.json"),
    m_hotkeyRevision(Hotkeys::Revision())
{
    m_session.SetListener(this);
}

MainWindow::~MainWindow()
{
    SettingsStore::GetInstance().UnregisterCallback(NXUISetting::EnableDiscordPresence, DiscordSettingChanged, this);
    m_session.SetListener(nullptr);
    // Join every worker borrowing the loader before session destruction.
    m_library.StopScan();
    m_library.StopDetails();
    if (m_firmwareDialog)
    {
        m_firmwareDialog->UnbindLoader();
    }
    SaveIconSize();
    // UiWindow's binding is owned by Run(), not by this view.  It is
    // detached after all RmlUI views have been destroyed.
}

bool MainWindow::Create(const std::function<bool()> & pump)
{
    m_interfaceLanguage = InterfaceLanguage::Code();
    m_document = Translation::LoadDocument(*m_window.Context(), m_assetDir / "main_window.rml");
    if (m_document == nullptr)
    {
        return false;
    }
    if (pump && !pump()) return false;
    m_menuBar = std::make_unique<MenuBar>(
        m_document->GetElementById("menu-bar"), [this](int id) { Post(static_cast<Action>(id)); }, [this](int id, int value) { SetMenuRange(id, value); });
    m_gameMenu = std::make_unique<PopupMenu>(
        m_document->GetElementById("popup-menu"), [this](int id) { Post(static_cast<Action>(id)); }, [this](int id, int value) { SetMenuRange(id, value); });
    m_loadingScreen = std::make_unique<LoadingScreen>(m_document->GetElementById("loading-panel"), m_window.Renderer());
    const OpenSettingsPage openPage = [this](SettingsPage page) { return ShowSettingsPage(page); };
    m_aboutDialog = std::make_unique<AboutDialog>(m_window.Context(), m_assetDir);
    m_firmwareDialog = std::make_unique<FirmwareDialog>(m_window, m_session.Loader(), m_assetDir,
                                                        FirmwareDialog::Host{
                                                            [this] {
                                                                const bool scanning = m_library.Scanning();
                                                                m_library.StopScan();
                                                                m_library.StopDetails();
                                                                if (scanning)
                                                                {
                                                                    m_rescanGames = true;
                                                                }
                                                            },
                                                            [this] {
                                                                FirmwareAvatars::Forget();
                                                                RefreshFirmwareVersion();
                                                            },
                                                        });
    m_tooltip = std::make_unique<Tooltip>(m_window.Context(), m_assetDir);
    if (pump && !pump()) return false;
    m_performance = std::make_unique<PerformanceOverlay>(m_document->GetElementById("performance"));
    m_detailsPanel = std::make_unique<GameDetailsPanel>(m_document->GetElementById("games-details"), [this](size_t index) {
        Post(static_cast<Action>(static_cast<int>(Action::AddOnFirst) + static_cast<int>(index)));
    });
    m_generalDialog = std::make_unique<GeneralDialog>(m_window, m_session, m_settingsContext, m_assetDir, GeneralDialog::Host{[this] { UpdateStatusWidgets(); }, openPage});
    m_advancedDialog = std::make_unique<AdvancedDialog>(m_window, m_session, m_settingsContext, m_assetDir, openPage,
                                                            [this] { return EnsureUtilityGuest(); });
    m_controllerDialog = std::make_unique<ControllerDialog>(m_window, m_session, m_assetDir);
    if (pump && !pump()) return false;
    m_directoriesDialog = std::make_unique<DirectoriesDialog>(m_window, m_library, m_assetDir,
                                                        DirectoriesDialog::Host{
                                                            [this] { return !m_session.IsRunning() && m_pendingGame.empty(); },
                                                            [this] { RescanGames(); },
                                                            openPage,
                                                        });
    m_profilesDialog = std::make_unique<ProfilesDialog>(m_window, m_session, m_assetDir,
                                                        ProfilesDialog::Host{
                                                            [this] { return !m_session.IsRunning() && m_pendingGame.empty(); },
                                                            [this] { return EnsureUtilityGuest(true); },
                                                            [this] { RefreshProfiles(); },
                                                            openPage,
                                                        });
    BindEvents();
    if (pump && !pump()) return false;

    m_document->SetClass(m_window.DrawsWindowControls() ? "window-controls" : "system-controls", true);
    if (m_window.SystemTitleBarHeight() > 0)
    {
        if (Rml::Element * titleBar = m_document->GetElementById("title-bar"))
        {
            titleBar->SetProperty(Rml::PropertyId::Height, Rml::Property(static_cast<float>(m_window.SystemTitleBarHeight() + 6), Rml::Unit::DP));
        }
    }
    SetTitle({});
    ShowPanel(Panel::RomBrowser);
    UpdateStatusText();
    m_iconSize = GameIconSize();
    RefreshFirmwareVersion();
    RefreshProfiles();
    SettingsStore::GetInstance().RegisterCallback(NXUISetting::EnableDiscordPresence, DiscordSettingChanged, this);
    UpdateDiscord();
    ApplyLibraryOptions();
    ApplyIconSize();
    m_library.StartScan(m_session.Loader());
    RenderGames();
    m_document->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
    return true;
}

void MainWindow::OpenGame(const std::string & path)
{
    m_pendingGame = path;
    m_launchReady = false;
    m_menuDirty = true;
}

void MainWindow::RequestQuit()
{
    m_firmwareDialog->CancelInstall();
    if (!m_session.IsRunning() || ConfirmStop(Tr("NXEmu - Exit")))
    {
        m_quit = true;
    }
}

void MainWindow::HandleEvent(const SDL_Event & event)
{
    const Hotkeys::GamepadShortcut chord = Hotkeys::TrackGamepad(event);
    if (m_advancedDialog->HandleGamepad(event, chord))
    {
        return;
    }
    if (event.type == SDL_JOYBUTTONDOWN && chord.buttons != 0 && !DialogOpen() && m_session.IsRunning() &&
        (SDL_GetWindowFlags(m_window.Handle()) & SDL_WINDOW_INPUT_FOCUS) != 0)
    {
        for (const HotkeyAction & hotkey : kHotkeyActions)
        {
            if (Hotkeys::GetGamepad(hotkey.id) == chord && (hotkey.action != Action::ExitFullscreen || m_window.IsFullscreen()))
            {
                Post(hotkey.action);
                return;
            }
        }
    }
    if (event.type == SDL_MOUSEMOTION || event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEWHEEL)
    {
        m_mouseMovedAt = SDL_GetTicks64();
        UpdateMouseCursor();
    }
    if (event.type == SDL_DROPFILE)
    {
        if (event.drop.file != nullptr)
        {
            if (!m_firmwareDialog->OfferSource(event.drop.file) && !DialogOpen())
            {
                OpenGame(event.drop.file);
            }
            SDL_free(event.drop.file);
        }
        return;
    }
    if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)
    {
        m_controllerDialog->HandleKey(event.key);
        if (m_advancedDialog->HandleKey(event.key))
        {
            return;
        }
    }
    if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE &&
        (m_menuBar->IsOpen() || m_gameMenu->IsOpen()))
    {
        m_menuBar->Close();
        m_gameMenu->Close();
        m_pendingGameMenu = nullptr;
        m_pendingSelectMenu = nullptr;
        return;
    }
    if ((event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) && !DialogOpen())
    {
        for (const HotkeyAction & hotkey : kHotkeyActions)
        {
            if (!Hotkeys::Get(hotkey.id).Matches(event.key.keysym))
            {
                continue;
            }
            if (hotkey.action == Action::ExitFullscreen && !m_window.IsFullscreen())
            {
                continue;
            }
            if (event.type == SDL_KEYDOWN && event.key.repeat == 0)
            {
                Post(hotkey.action);
            }
            return;
        }
        m_session.HandleKey(event.key);
    }
    if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_HIT_TEST)
    {
        m_menuBar->Close();
        m_gameMenu->Close();
    }
    if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
    {
        m_gameMenu->Close();
    }
    m_window.HandleEvent(event);
}

void MainWindow::Update()
{
    RunPendingWork();
    m_session.Update(!m_firmwareDialog->Installing());
    if (!m_session.FrontendReady())
    {
        m_quit = true;
        return;
    }
    if (m_detailsReload && !m_session.IsRunning() && !m_session.ReleasePending())
    {
        m_detailsReload = false;
        RequestSelectedDetails();
    }
    m_aboutDialog->Update();
    m_firmwareDialog->Update();
    m_controllerDialog->Update();
    m_generalDialog->Update();
    m_advancedDialog->Update();
    m_directoriesDialog->Update();
    m_profilesDialog->Update();
    if (m_utilityGuest && !m_profilesDialog->IsVisible() && !m_advancedDialog->IsVisible() && !m_controllerDialog->IsVisible())
    {
        m_utilityGuest = false;
        m_session.ReleaseIdleGuest();
    }
    SyncInterfaceLanguage();
    std::vector<FoundGame> found;
    if (m_library.TakeFound(found))
    {
        AddGames(std::move(found));
    }
    if (m_library.Scanning() != m_gamesScanning)
    {
        m_gamesScanning = !m_gamesScanning;
        m_gamesDirty = true;
        if (!m_gamesScanning && m_rescanning)
        {
            FinishRescan();
        }
    }
    UpdateScanIndicator();
    if (m_gamesDirty)
    {
        RenderGames();
    }
    GameDetails details;
    if (m_library.TakeDetails(details) && details.path == m_selectedPath)
    {
        m_details = std::move(details);
        m_detailsDirty = true;
    }
    if (m_detailsDirty)
    {
        RenderDetails();
    }
    RenderVisibleGames();
    LoadPendingIcons();
    LayoutList();
    if (m_fitWindow)
    {
        m_fitWindow = false;
        FitWindowToCards();
    }
    if (m_pendingIconSize != 0 && m_pendingIconSize != m_iconSize)
    {
        m_iconSize = m_pendingIconSize;
        m_iconSizeChanged = SDL_GetTicks64();
        ApplyIconSize();
    }
    m_pendingIconSize = 0;
    if (m_iconSizeChanged != 0 && SDL_GetTicks64() - m_iconSizeChanged >= kIconSizeSaveDelayMs)
    {
        SaveIconSize();
    }
    SyncPhase();
    m_playTime.SetPlaying(m_phase == EmulatorSession::Phase::Running ? m_playingProgram : 0);
    if (m_rescanGames && m_phase == EmulatorSession::Phase::Idle && m_pendingGame.empty() && !m_firmwareDialog->Installing())
    {
        m_rescanGames = false;
        RescanGames();
    }
    SyncWindowState();
    UpdateMouseCursor();
    if (const uint64_t now = SDL_GetTicks64(); now - m_lastMeasure >= (m_performance->Shown() ? m_performance->Interval() : kStatusIntervalMs))
    {
        m_lastMeasure = now;
        MeasureGame();
    }
    if (const uint64_t now = SDL_GetTicks64(); now - m_lastStatusUpdate >= kStatusIntervalMs)
    {
        m_lastStatusUpdate = now;
        UpdateStatusWidgets();
    }
    if (m_hotkeyRevision != Hotkeys::Revision())
    {
        m_hotkeyRevision = Hotkeys::Revision();
        m_menuDirty = true;
    }
    if (m_menuDirty && !m_menuBar->IsOpen())
    {
        m_menuDirty = false;
        RebuildMenu();
    }
    const Rml::Context * context = m_document->GetContext();
    const float width = static_cast<float>(context->GetDimensions().x) / context->GetDensityIndependentPixelRatio();
    m_document->SetClass("narrow", width < kNarrowWidth);
    m_document->SetClass("compact", width < kCompactWidth);
    const float density = context->GetDensityIndependentPixelRatio();
    const float menuWidth = m_document->GetElementById("menu-bar")->GetOffsetWidth() / density;
    const float windowSpace = m_window.DrawsWindowControls() ? 150.0f : 82.0f;
    const float titleSpace = width - menuWidth - windowSpace;
    const bool gameInTitle = m_document->IsClassSet("has-game") || m_document->IsClassSet("game-controls-visible");
    m_document->SetClass("title-hide-settings", titleSpace < (gameInTitle ? 720.0f : 230.0f));
    m_document->SetClass("title-hide-text", titleSpace < 520.0f);
    m_document->SetClass("title-hide-secondary", titleSpace < 400.0f);
    m_document->SetClass("title-minimal", titleSpace < 260.0f);
    m_tooltip->Update(m_menuBar->IsOpen() || (m_gameMenu->IsOpen() && !m_gameMenu->Hovered()));
    m_window.Context()->Update();
    LayoutGameArea();
}

bool MainWindow::Still() const
{
    return m_phase != EmulatorSession::Phase::Loading && m_pendingGame.empty() && !m_firmwareDialog->Installing() && !m_controllerDialog->Listening() && !m_profilesDialog->Busy() &&
           m_window.Context()->GetNextUpdateDelay() > 0;
}

bool MainWindow::DialogOpen() const
{
    return m_aboutDialog->IsVisible() || m_firmwareDialog->IsVisible() || m_controllerDialog->IsVisible() || m_generalDialog->IsVisible() || m_advancedDialog->IsVisible() || m_directoriesDialog->IsVisible() ||
           m_profilesDialog->IsVisible();
}

void MainWindow::OnModulesReleasing()
{
    if (m_firmwareDialog)
    {
        m_firmwareDialog->UnbindLoader();
    }
    const bool scanning = m_library.Scanning();
    // A finished-but-joinable scan still borrows the loader DLL.
    m_library.StopScan();
    if (scanning)
    {
        m_rescanGames = true;
    }
    m_library.StopDetails();
    m_details = {};
    m_detailsDirty = true;
    m_detailsReload = true;
    m_controllerDialog->Abandon();
    if (m_keepProfileDialogDuringSetup)
    {
        // Lazy profile edits must retain the dialog, but drain its loader worker.
        m_profilesDialog->DrainLoaderWork();
    }
    else
    {
        m_profilesDialog->Abandon();
    }
    // The interface Vulkan device persists across guest sessions (A2k).
}

void MainWindow::OnModulesReady()
{
    m_firmwareDialog->SetLoader(m_session.Loader());
}

void MainWindow::OnGameStarted(const std::string & path)
{
    const auto game = std::find_if(m_games.begin(), m_games.end(), [&path](const GameEntry & entry) { return entry.path == path; });
    const bool hasIcon = game != m_games.end() && !game->icon.empty();
    m_document->SetClass("has-game-icon", hasIcon);
    if (Rml::Element * icon = m_document->GetElementById("title-game-icon"))
    {
        if (hasIcon)
        {
            icon->SetAttribute("src", game->icon);
        }
        else
        {
            icon->RemoveAttribute("src");
        }
    }
    SetTitle(game != m_games.end() ? game->title : SettingsStore::GetInstance().GetString(NXCoreSetting::GameName));
    m_library.AddRecentFile(path);
    m_gamesDirty = true;
    m_applyStartOptions = true;
    m_menuDirty = true;
}

void MainWindow::OnSessionError(const std::string & message)
{
    g_notify->DisplayError(message.c_str(), "NXEmu");
}

void MainWindow::Post(Action action)
{
    m_pendingActions.push_back(action);
}

void MainWindow::RunPendingWork()
{
    std::vector<Action> actions;
    actions.swap(m_pendingActions);
    for (const Action action : actions)
    {
        Run(action);
    }
    if (!actions.empty() && m_gameMenu->IsOpen() && m_gameMenu->Origin() != nullptr && m_pendingSelectMenu == nullptr &&
        m_gameMenu->Origin()->IsClassSet("select"))
    {
        m_pendingSelectMenu = m_gameMenu->Origin();
    }
    if (m_pendingGameMenu != nullptr)
    {
        Rml::Element * origin = m_pendingGameMenu;
        m_pendingGameMenu = nullptr;
        ShowGameMenu(origin);
    }
    if (m_pendingSelectMenu != nullptr)
    {
        Rml::Element * origin = m_pendingSelectMenu;
        m_pendingSelectMenu = nullptr;
        ShowSelectMenu(origin);
    }
    if (!m_pendingGame.empty() && !m_firmwareDialog->IsVisible() && !m_firmwareDialog->Installing())
    {
        if (!m_launchReady && m_firmwareOfferedFor != m_pendingGame && !m_session.IsRunning())
        {
            m_firmwareOfferedFor = m_pendingGame;
            char version[64]{};
            if (m_session.Loader().GetGameCardFirmwareUpdate(m_pendingGame.c_str(), version, sizeof(version)) != 0)
            {
                m_firmwareDialog->OfferFromGame(m_pendingGame, version);
                return;
            }
        }
        if (!m_launchReady)
        {
            m_launchReady = true;
            AnnounceGame(m_pendingGame);
            if (!m_session.IsRunning())
            {
                return;
            }
        }
        m_launchReady = false;
        std::string path;
        path.swap(m_pendingGame);
        m_firmwareOfferedFor.clear();
        m_session.LoadGame(path);
    }
}

void MainWindow::Run(Action action)
{
    switch (action)
    {
    case Action::LoadFile: LoadFile(); break;
    case Action::OpenAbout: m_aboutDialog->Show(); break;
    case Action::InstallFirmware:
        if (m_session.FrontendReady() && !m_session.IsRunning() && m_pendingGame.empty())
        {
            m_firmwareDialog->Show();
        }
        break;
    case Action::Exit: RequestQuit(); break;
    case Action::PauseOrContinue: m_session.TogglePause(); break;
    case Action::Stop: StopGame(); break;
    case Action::Restart:
        if (m_session.IsRunning())
        {
            const std::string path = SettingsStore::GetInstance().GetString(NXCoreSetting::GameFile);
            if (!path.empty() && ConfirmStop(Tr("Restart")))
            {
                m_session.Stop();
                OpenGame(path);
            }
        }
        break;
    case Action::ToggleEmulationControls:
        uiSettings.showEmulationControls = !uiSettings.showEmulationControls;
        SaveUISetting();
        m_menuDirty = true;
        SyncPhase();
        break;
    case Action::ToggleStartGamesInFullscreen:
        SettingsStore::GetInstance().SetBool(NXUISetting::StartGamesInFullscreen, !uiSettings.startGamesInFullscreen);
        SaveUISetting();
        m_menuDirty = true;
        break;
    case Action::ToggleStartGamesWithUiHidden:
        SettingsStore::GetInstance().SetBool(NXUISetting::StartGamesWithUiHidden, !uiSettings.startGamesWithUiHidden);
        SaveUISetting();
        m_menuDirty = true;
        break;
    case Action::TogglePerformance:
        uiSettings.showPerformance = !uiSettings.showPerformance;
        SaveUISetting();
        m_menuDirty = true;
        break;
    case Action::TogglePerformanceHistory:
    case Action::TogglePerformanceRange:
    case Action::TogglePerformanceFrameTime:
    case Action::TogglePerformanceLongest:
    case Action::TogglePerformanceSpeed:
    case Action::TogglePerformanceProcessor:
    case Action::TogglePerformanceMemory:
        m_performance->Toggle(static_cast<PerformanceItem>(static_cast<int>(action) - static_cast<int>(Action::TogglePerformanceHistory)));
        m_menuDirty = true;
        break;
    case Action::ToggleHideUi:
        if (m_session.IsRunning())
        {
            m_hideUi = !m_hideUi;
            m_menuDirty = true;
        }
        break;
    case Action::StartMenuGame: OpenGame(m_menuGame); break;
    case Action::OpenMenuGameDirectory:
        OpenDirectory(Common::FS::PathToUTF8String(std::filesystem::path{Common::FS::ToU8String(m_menuGame)}.parent_path()));
        break;
    case Action::CopyMenuGamePath: SDL_SetClipboardText(m_menuGame.c_str()); break;
    case Action::OpenMenuGameSaves:
    case Action::OpenMenuGameCache:
    case Action::ClearMenuGameCache:
    {
        const auto game = std::find_if(m_games.begin(), m_games.end(), [this](const GameEntry & entry) { return entry.path == m_menuGame; });
        if (game == m_games.end() || game->programId == 0)
        {
            break;
        }
        const std::filesystem::path cacheRoot = Common::FS::GetYuzuPath(Common::FS::YuzuPath::ShaderDir);
        if (action == Action::OpenMenuGameSaves)
        {
            const std::string directory = GameDetailsReader::SaveDirectory(game->programId);
            if (!directory.empty())
            {
                OpenDirectory(directory);
            }
        }
        else if (action == Action::OpenMenuGameCache)
        {
            OpenDirectory(Common::FS::PathToUTF8String(GameCache::Directory(cacheRoot, game->programId)));
        }
        else if (!m_session.IsRunning())
        {
            const std::string message = Tr("Clear the shader cache for %1?\n\nShaders will be rebuilt the next time you play.", {game->title});
            const std::string title = Tr("Clear cache");
            if (g_notify->Query(message.c_str(), title.c_str()) == NotificationResponse::Yes)
            {
                std::string error;
                if (!GameCache::Clear(cacheRoot, game->programId, error))
                {
                    const std::string failure = Tr("Could not clear the cache: %1", {error});
                    g_notify->DisplayError(failure.c_str(), title.c_str());
                }
                m_details = {};
                RequestSelectedDetails();
            }
        }
        break;
    }
    case Action::ToggleDockedMode: SetDockedMode(!IsDocked()); break;
    case Action::SetDockedMode: SetDockedMode(true); break;
    case Action::SetHandheldMode: SetDockedMode(false); break;
    case Action::ToggleGameDetails:
        uiSettings.showGameDetails = !uiSettings.showGameDetails;
        SaveUISetting();
        ApplyLibraryOptions();
        RequestSelectedDetails();
        break;
    case Action::OpenSelectedSaveDirectory:
    case Action::OpenSelectedCacheDirectory:
    case Action::OpenSelectedModDirectory:
        if (m_details.path == m_selectedPath)
        {
            const std::string & directory = action == Action::OpenSelectedSaveDirectory    ? m_details.saveDirectory
                                            : action == Action::OpenSelectedCacheDirectory ? m_details.cacheDirectory
                                                                                           : m_details.modDirectory;
            if (!directory.empty())
            {
                std::error_code error;
                std::filesystem::create_directories(std::filesystem::path{Common::FS::ToU8String(directory)}, error);
                OpenDirectory(directory);
            }
        }
        break;
    case Action::PlaySelectedGame:
        if (!m_selectedPath.empty())
        {
            OpenGame(m_selectedPath);
        }
        break;
    case Action::ToggleCardInformation:
        uiSettings.showCardInformation = !uiSettings.showCardInformation;
        SaveUISetting();
        ApplyLibraryOptions();
        break;
    case Action::ShowGridView:
    case Action::ShowListView:
        if (uiSettings.gameListView != (action == Action::ShowListView))
        {
            uiSettings.gameListView = action == Action::ShowListView;
            SaveUISetting();
            ApplyLibraryOptions();
            m_gamesDirty = true;
        }
        break;
    case Action::TogglePlayTime:
        uiSettings.showPlayTime = !uiSettings.showPlayTime;
        SaveUISetting();
        ApplyLibraryOptions();
        break;
    case Action::ToggleCardTitle:
        uiSettings.showCardTitle = !uiSettings.showCardTitle;
        SaveUISetting();
        ApplyLibraryOptions();
        break;
    case Action::ToggleSpeedLimit:
    {
        SettingsStore & settings = SettingsStore::GetInstance();
        settings.SetBool(NXOsSetting::UseSpeedLimit, !settings.GetBool(NXOsSetting::UseSpeedLimit));
        m_session.FlushSettings();
        m_menuDirty = true;
        UpdateStatusText();
        break;
    }
    case Action::OpenAppDirectory: OpenDirectory(Common::FS::GetYuzuPathString(Common::FS::YuzuPath::YuzuDir)); break;
    case Action::OpenLogDirectory: OpenDirectory(Common::FS::GetYuzuPathString(Common::FS::YuzuPath::LogDir)); break;
    case Action::ToggleFullscreen:
        if (m_session.IsRunning() || m_window.IsFullscreen())
        {
            m_window.SetFullscreen(!m_window.IsFullscreen());
        }
        break;
    case Action::ExitFullscreen: m_window.SetFullscreen(false); break;
    case Action::ResetWindowSize720p: ResetWindowSize(1280, 720); break;
    case Action::ResetWindowSize900p: ResetWindowSize(1600, 900); break;
    case Action::ResetWindowSize1080p: ResetWindowSize(1920, 1080); break;
    case Action::OpenDiscord: OpenUrl(kDiscordUrl); break;
    case Action::OpenReportIssues: OpenUrl(kReportUrl); break;
    case Action::OpenSettings:
        m_settingsContext.Global();
        ShowSettingsPage(SettingsPage::General);
        break;
    case Action::OpenProfiles: m_settingsContext.Global(); ShowSettingsPage(SettingsPage::Profiles); break;
    case Action::OpenGameDirectories: m_settingsContext.Global(); ShowSettingsPage(SettingsPage::Directories); break;
    case Action::ConfigureControllers:
        // Physical mappings need the HID backend. Construct it only for this
        // explicit action, before OnOpen chooses the dialog's input backend.
        if (EnsureUtilityGuest())
            m_controllerDialog->Show();
        else
            g_notify->DisplayError(Tr("Required modules failed to load.").c_str(), "NXEmu");
        break;
    case Action::CopyVersion:
        SDL_SetClipboardText(VER_FILE_VERSION_STR);
        m_versionCopiedUntil = SDL_GetTicks64() + kVersionCopiedMs;
        ShowVersion();
        break;
    default:
    {
        const int scale = static_cast<int>(action) - static_cast<int>(Action::ResolutionScaleFirst);
        if (scale >= 0 && scale < kResolutionScaleCount)
        {
            if (!m_session.IsRunning())
            {
                SettingsStore::GetInstance().SetInt(NXVideoSetting::ResolutionSetup, scale);
                m_session.FlushSettings();
                UpdateStatusWidgets();
            }
            break;
        }
        const int profile = static_cast<int>(action) - static_cast<int>(Action::ProfileFirst);
        if (profile >= 0 && static_cast<int>(action) < static_cast<int>(Action::ResolutionScaleFirst))
        {
            if (!m_session.IsRunning() && profile < static_cast<int>(m_profiles.size()))
            {
                UserProfiles::SetActive(profile);
                m_session.FlushSettings();
                UpdateStatusWidgets();
            }
            break;
        }
        const int filter = static_cast<int>(action) - static_cast<int>(Action::ScalingFilterFirst);
        if (filter >= 0 && static_cast<int>(action) < static_cast<int>(Action::AddOnFirst) && FindScalingFilter(filter) != nullptr)
        {
            SettingsStore::GetInstance().SetInt(NXVideoSetting::ScalingFilter, filter);
            m_session.FlushSettings();
            UpdateStatusWidgets();
            break;
        }
        const int sort = static_cast<int>(action) - static_cast<int>(Action::GameSortFirst);
        if (sort >= 0 && sort < kGameSortCount)
        {
            uiSettings.gameSort = sort;
            SaveUISetting();
            ApplyLibraryOptions();
            m_gamesDirty = true;
            break;
        }
        const int addOn = static_cast<int>(action) - static_cast<int>(Action::AddOnFirst);
        if (addOn >= 0 && static_cast<int>(action) < static_cast<int>(Action::RecentFileFirst))
        {
            ToggleAddOn(static_cast<size_t>(addOn));
            break;
        }
        const size_t recent = static_cast<size_t>(static_cast<int>(action) - static_cast<int>(Action::RecentFileFirst));
        if (static_cast<int>(action) >= static_cast<int>(Action::RecentFileFirst) && recent < m_library.RecentFiles().size())
        {
            OpenGame(m_library.RecentFiles()[recent]);
        }
        break;
    }
    }
}

std::string MainWindow::ShortcutLabel(Action action) const
{
    for (const HotkeyAction & hotkey : kHotkeyActions)
    {
        if (hotkey.action == action)
        {
            return Hotkeys::Get(hotkey.id).Label();
        }
    }
    return {};
}

void MainWindow::BindEvents()
{
    const auto onClick = [this](const char * id, Action action) {
        UiEvents::Listen(m_document->GetElementById(id), "click", [this, action](Rml::Event &) { Post(action); });
    };
    onClick("games-details-toggle", Action::ToggleGameDetails);
    for (const auto & [id, action] : {std::pair{"details-play", Action::PlaySelectedGame}, std::pair{"details-save", Action::OpenSelectedSaveDirectory},
                                      std::pair{"details-cache", Action::OpenSelectedCacheDirectory}, std::pair{"details-mods", Action::OpenSelectedModDirectory}})
    {
        Rml::Element * button = m_document->GetElementById(id);
        UiEvents::Listen(button, "click", [this, button, action = action](Rml::Event &) {
            if (!button->IsClassSet("locked"))
            {
                Post(action);
            }
        });
    }
    onClick("card-information", Action::ToggleCardInformation);
    onClick("view-grid", Action::ShowGridView);
    onClick("view-list", Action::ShowListView);
    onClick("play-time", Action::TogglePlayTime);
    onClick("card-title", Action::ToggleCardTitle);
    UiEvents::Listen(m_document->GetElementById("icon-size-slider"), "change", [this](Rml::Event & event) {
        m_pendingIconSize = std::clamp(static_cast<int>(std::lround(event.GetParameter<float>("value", 0.0f))), kGameIconMin, kGameIconMax);
    });
    for (const char * id : {"games-sort", "docked-mode", "resolution-scale", "scaling-filter", "profile"})
    {
        Rml::Element * select = m_document->GetElementById(id);
        UiEvents::Listen(select, "click", [this, select](Rml::Event &) {
            if (select == m_gameMenu->Origin())
            {
                m_gameMenu->Close();
            }
            else if (!select->IsClassSet("locked"))
            {
                m_pendingSelectMenu = select;
            }
        });
    }
    m_volume = std::make_unique<VolumeControl>(m_document->GetElementById("volume"), [this] {
        m_session.FlushSettings();
        UpdateStatusWidgets();
    });
    UiEvents::Listen(m_document->GetElementById("games-list-header"), "click", [this](Rml::Event & event) {
        const int sort = event.GetTargetElement()->GetAttribute<int>("data-sort", -1);
        if (std::find(std::begin(kOfferedSorts), std::end(kOfferedSorts), sort) != std::end(kOfferedSorts))
        {
            Post(static_cast<Action>(static_cast<int>(Action::GameSortFirst) + sort));
        }
    });
    onClick("title-settings", Action::OpenSettings);
    onClick("title-controllers", Action::ConfigureControllers);
    onClick("title-restart", Action::Restart);
    onClick("title-logo", Action::CopyVersion);
    onClick("title-name", Action::CopyVersion);
    onClick("title-version", Action::CopyVersion);
    onClick("title-pause", Action::PauseOrContinue);
    onClick("title-stop", Action::Stop);
    onClick("window-close", Action::Exit);
    UiEvents::Listen(m_document->GetElementById("games-search"), "change", [this](Rml::Event & event) {
        m_gameSearch = event.GetParameter<Rml::String>("value", "");
        m_gamesDirty = true;
    });
    UiEvents::Listen(m_document->GetElementById("games-grid"), "click", [this](Rml::Event & event) {
        SelectGame(UiEvents::ClosestWithClass(event.GetTargetElement(), "rom-game"));
        if (m_selectedGame != nullptr && UiEvents::ClosestWithClass(event.GetTargetElement(), "row-play") != nullptr)
        {
            Post(Action::PlaySelectedGame);
        }
        Rml::Element * button = UiEvents::ClosestWithClass(event.GetTargetElement(), "rom-more");
        if (button != nullptr && button == m_gameMenu->Origin())
        {
            m_gameMenu->Close();
        }
        else if (button != nullptr)
        {
            m_pendingGameMenu = button;
            m_gameMenuPoint.reset();
        }
    });
    UiEvents::Listen(m_document->GetElementById("games-grid"), "mousedown", [this](Rml::Event & event) {
        Rml::Element * game = UiEvents::ClosestWithClass(event.GetTargetElement(), "rom-game");
        if (game != nullptr && event.GetParameter<int>("button", 0) == 1)
        {
            SelectGame(game);
            m_pendingGameMenu = game;
            m_gameMenuPoint = Rml::Vector2f(event.GetParameter<float>("mouse_x", 0.0f), event.GetParameter<float>("mouse_y", 0.0f));
        }
    });
    UiEvents::Listen(m_document->GetElementById("games-grid"), "dblclick", [this](Rml::Event & event) {
        if (UiEvents::ClosestWithClass(event.GetTargetElement(), "rom-more") != nullptr ||
            UiEvents::ClosestWithClass(event.GetTargetElement(), "row-play") != nullptr)
        {
            return;
        }
        Rml::Element * card = UiEvents::ClosestWithClass(event.GetTargetElement(), "rom-game");
        const size_t index = card != nullptr ? static_cast<size_t>(card->GetAttribute<int>("data-game", -1)) : m_games.size();
        if (index < m_games.size())
        {
            OpenGame(m_games[index].path);
        }
    });
    UiEvents::Listen(m_document->GetElementById("window-minimise"), "click", [this](Rml::Event &) { m_window.Minimise(); });
    UiEvents::Listen(m_document->GetElementById("window-maximise"), "click", [this](Rml::Event &) { m_window.SetMaximised(!m_window.IsMaximised()); });

    Rml::Element * titleBar = m_document->GetElementById("title-bar");
    UiEvents::Listen(titleBar, "dblclick", [this, titleBar](Rml::Event & event) {
        for (Rml::Element * element = event.GetTargetElement(); element != nullptr && element != titleBar; element = element->GetParentNode())
        {
            if (element->HasAttribute("data-window"))
            {
                return;
            }
        }
        m_window.TitleBarDoubleClicked();
    });
}

void MainWindow::RebuildMenu()
{
    const bool running = m_session.IsRunning();
    const auto phase = m_session.CurrentPhase();
    const bool playing = running && (phase == EmulatorSession::Phase::Running || phase == EmulatorSession::Phase::Paused);
    const bool canStop = running || !m_pendingGame.empty();
    const auto action = [this](Action id, std::string label, bool enabled = true) {
        return MenuItem::Action(static_cast<int>(id), std::move(label), ShortcutLabel(id), enabled);
    };
    const auto check = [this](Action id, std::string label, bool checked) {
        return MenuItem::Check(static_cast<int>(id), std::move(label), checked, ShortcutLabel(id));
    };

    std::vector<MenuItem> file;
    file.push_back(action(Action::LoadFile, Tr("&Load File")));
    if (!m_library.RecentFiles().empty())
    {
        std::vector<MenuItem> recent;
        for (size_t i = 0; i < m_library.RecentFiles().size(); i++)
        {
            recent.push_back(MenuItem::Action(static_cast<int>(Action::RecentFileFirst) + static_cast<int>(i), std::to_string(i + 1) + " " + FileName(m_library.RecentFiles()[i])));
        }
        file.push_back(MenuItem::Submenu(Tr("&Recent File"), std::move(recent)));
    }
    file.push_back(MenuItem::Separator());
    file.push_back(MenuItem::Submenu(Tr("Open &NXEmu Folders"), {
                                                                action(Action::OpenAppDirectory, Tr("&App Directory")),
                                                                action(Action::OpenLogDirectory, Tr("&Log Folder")),
                                                            }));
    file.push_back(MenuItem::Separator());
    file.push_back(action(Action::OpenSettings, Tr("&Settings")));
    file.push_back(action(Action::ConfigureControllers, Tr("Controllers"), m_session.FrontendReady()));
    file.push_back(MenuItem::Separator());
    file.push_back(action(Action::InstallFirmware, Tr("Install &Firmware"), !running));
    file.push_back(MenuItem::Separator());
    file.push_back(action(Action::Exit, Tr("E&xit")));

    std::vector<MenuItem> menus;
    menus.push_back(MenuItem::Submenu(Tr("&File"), std::move(file)));

    // Keep the menu positions stable when a game starts or stops.
    std::vector<MenuItem> emulation = {
        action(Action::PauseOrContinue, m_session.IsPaused() ? Tr("Continue") : Tr("Pause")),
        action(Action::Stop, Tr("&Stop")),
        action(Action::Restart, Tr("Restart")),
    };
    for (MenuItem & item : emulation)
    {
        item.enabled = item.id == static_cast<int>(Action::Stop) ? canStop : playing;
    }

    emulation.push_back(check(Action::ToggleEmulationControls, Tr("Show emulation controls"), uiSettings.showEmulationControls));
    emulation.push_back(MenuItem::Separator());
    MenuItem speedLimit = check(Action::ToggleSpeedLimit, Tr("Limit &Speed"), SettingsStore::GetInstance().GetBool(NXOsSetting::UseSpeedLimit));
    speedLimit.enabled = playing;
    emulation.push_back(std::move(speedLimit));

    // Fullscreen and the hidden interface are for a game: without one, the
    // menu says how the next starts.
    emulation.push_back(MenuItem::Separator());
    emulation.push_back(action(Action::ToggleFullscreen, Tr("&Fullscreen"), running));
    emulation.push_back(action(Action::ToggleHideUi, m_hideUi ? Tr("Show &UI") : Tr("Hide &UI"), running));
    emulation.push_back(MenuItem::Submenu(Tr("Window size"), {
        action(Action::ResetWindowSize720p, "720p"),
        action(Action::ResetWindowSize900p, "900p"),
        action(Action::ResetWindowSize1080p, "1080p"),
    }));
    std::vector<MenuItem> performance;
    performance.push_back(check(Action::TogglePerformance, Tr("Show performance"), uiSettings.showPerformance));
    performance.push_back(MenuItem::Separator());
    for (const PerformanceOverlay::Item & item : PerformanceOverlay::kItems)
    {
        const Action id = static_cast<Action>(static_cast<int>(Action::TogglePerformanceHistory) + static_cast<int>(item.id));
        performance.push_back(check(id, Tr(item.label), PerformanceOverlay::Has(item.id)));
    }
    performance.push_back(MenuItem::Separator());
    performance.push_back(MenuItem::Slider(Tr("Size"), {kPerformanceSizeRange, PerformanceOverlay::kLargest, PerformanceOverlay::Size(), true, {}, PerformanceOverlay::kSmallest,
                                                        PerformanceOverlay::kSizeStep}));
    performance.push_back(MenuItem::Slider(Tr("Measures per second"), {kPerformanceRateRange, PerformanceOverlay::kFastest, PerformanceOverlay::Rate(), true, {},
                                                                       PerformanceOverlay::kSlowest, 1, ""}));
    performance.push_back(MenuItem::Slider(Tr("History length"), {kPerformanceLengthRange, PerformanceOverlay::kLongest, PerformanceOverlay::Length(), true,
                                                                  Tr("How many measures the history shows"), PerformanceOverlay::kShortest, 1, ""}));
    emulation.push_back(MenuItem::Separator());
    emulation.push_back(MenuItem::Submenu(Tr("Performance overlay"), std::move(performance)));
    emulation.push_back(MenuItem::Separator());
    emulation.push_back(check(Action::ToggleStartGamesInFullscreen, Tr("Start games in fullscreen"), uiSettings.startGamesInFullscreen));
    emulation.push_back(check(Action::ToggleStartGamesWithUiHidden, Tr("Start games with UI hidden"), uiSettings.startGamesWithUiHidden));
    menus.push_back(MenuItem::Submenu(Tr("&Emulation"), std::move(emulation)));

    menus.push_back(MenuItem::Submenu(Tr("&Help"), {
                                                   action(Action::OpenDiscord, Tr("&Discord")),
                                                   action(Action::OpenReportIssues, Tr("&Report Issues")),
                                                   MenuItem::Separator(),
                                                   action(Action::OpenAbout, Tr("&About NXEmu")),
                                               }));
    m_menuBar->SetMenus(menus);
}

void MainWindow::AddGames(std::vector<FoundGame> found)
{
    std::vector<GameEntry> & games = m_rescanning ? m_nextGames : m_games;
    for (FoundGame & entry : found)
    {
        if (!entry.icon.empty())
        {
            entry.game.icon = m_window.Renderer().AddMemoryImage(std::move(entry.icon));
        }
        const std::vector<GameEntry>::iterator existing = std::find_if(games.begin(), games.end(), [&entry](const GameEntry & game) {
            return game.path == entry.game.path && game.programId == entry.game.programId;
        });
        if (existing == games.end())
        {
            games.push_back(std::move(entry.game));
            continue;
        }
        ReleaseIcons({*existing});
        *existing = std::move(entry.game);
    }
    if (m_rescanning)
    {
        return;
    }
    std::stable_sort(m_games.begin(), m_games.end(), [](const GameEntry & left, const GameEntry & right) {
        return std::forward_as_tuple(Folded(left.title), left.path) < std::forward_as_tuple(Folded(right.title), right.path);
    });
    m_selectedGame = nullptr;
    m_gamesDirty = true;
}

void MainWindow::RescanGames()
{
    ReleaseIcons(m_nextGames);
    m_nextGames.clear();
    m_rescanning = !m_games.empty();
    m_library.StartScan(m_session.Loader());
    m_gamesDirty = true;
}

void MainWindow::FinishRescan()
{
    m_rescanning = false;
    if (!m_library.ScanCompleted())
    {
        ReleaseIcons(m_nextGames);
        m_nextGames.clear();
        return;
    }
    std::stable_sort(m_nextGames.begin(), m_nextGames.end(), [](const GameEntry & left, const GameEntry & right) {
        return std::forward_as_tuple(Folded(left.title), left.path) < std::forward_as_tuple(Folded(right.title), right.path);
    });
    const auto same = [](const GameEntry & left, const GameEntry & right) {
        return left.path == right.path && left.programId == right.programId && left.title == right.title && left.version == right.version &&
               left.publisher == right.publisher && left.size == right.size && left.icon.empty() == right.icon.empty();
    };
    if (std::equal(m_games.begin(), m_games.end(), m_nextGames.begin(), m_nextGames.end(), same))
    {
        ReleaseIcons(m_nextGames);
    }
    else
    {
        ReleaseIcons(m_games);
        m_games.swap(m_nextGames);
        m_selectedGame = nullptr;
        m_gamesDirty = true;
    }
    m_nextGames.clear();
}

void MainWindow::ReleaseIcons(const std::vector<GameEntry> & games)
{
    for (const GameEntry & game : games)
    {
        if (!game.icon.empty())
        {
            m_loadedIcons.erase(game.icon);
            m_window.Renderer().RemoveMemoryImage(game.icon);
        }
    }
}

void MainWindow::UpdateScanIndicator()
{
    Rml::Element * indicator = m_document->GetElementById("games-scan-indicator");
    Rml::Element * fill = m_document->GetElementById("games-scan-fill");
    if (indicator == nullptr || fill == nullptr)
    {
        return;
    }
    const uint64_t now = SDL_GetTicks64();
    const float elapsed = static_cast<float>(now - m_scanIndicatorTime) / 1000.0f;
    m_scanIndicatorTime = now;
    if (!m_library.ReadingGames())
    {
        indicator->SetClass("active", false);
        m_scanReadingSince = 0;
        m_scanIndicatorShown = 0.0f;
        return;
    }
    if (m_scanReadingSince == 0)
    {
        m_scanReadingSince = now;
    }
    indicator->SetClass("active", now - m_scanReadingSince >= kScanIndicatorDelayMs);
    const uint32_t total = m_library.FilesTotal();
    const float target = total != 0 ? (static_cast<float>(m_library.FilesStarted()) - (1.0f - kScanStepShare)) / static_cast<float>(total) : 0.0f;
    if (target > m_scanIndicatorShown)
    {
        m_scanIndicatorShown += (target - m_scanIndicatorShown) * (1.0f - std::exp(-elapsed / kScanEaseSeconds));
    }
    fill->SetProperty(Rml::PropertyId::Width, Rml::Property(m_scanIndicatorShown * 100.0f, Rml::Unit::PERCENT));
}

void MainWindow::RenderGames()
{
    m_gamesDirty = false;
    Rml::Element * grid = m_document->GetElementById("games-grid");
    if (grid == nullptr)
    {
        return;
    }
    m_gameMenu->Close();
    m_pendingGameMenu = nullptr;
    m_selectedGame = nullptr;

    const std::string search = Folded(m_gameSearch);
    std::vector<size_t> shown;
    for (size_t i = 0; i < m_games.size(); i++)
    {
        if (search.empty() || Folded(m_games[i].title).find(search) != std::string::npos || Folded(m_games[i].publisher).find(search) != std::string::npos)
        {
            shown.push_back(i);
        }
    }
    const int sort = GameSort();
    if (sort == kSortByPublisher)
    {
        std::stable_sort(shown.begin(), shown.end(), [this](size_t left, size_t right) { return Folded(m_games[left].publisher) < Folded(m_games[right].publisher); });
    }
    else if (sort == kSortByRecentlyPlayed)
    {
        const std::vector<std::string> & recent = m_library.RecentFiles();
        const auto rank = [this, &recent](size_t game) { return std::find(recent.begin(), recent.end(), m_games[game].path) - recent.begin(); };
        std::stable_sort(shown.begin(), shown.end(), [&rank](size_t left, size_t right) { return rank(left) < rank(right); });
        std::stable_sort(shown.begin(), shown.end(), [this](size_t left, size_t right) {
            return m_playTime.Of(m_games[left].programId).lastPlayed > m_playTime.Of(m_games[right].programId).lastPlayed;
        });
    }
    else if (sort == kSortByPlayTime)
    {
        std::stable_sort(shown.begin(), shown.end(), [this](size_t left, size_t right) {
            return m_playTime.Of(m_games[left].programId).seconds > m_playTime.Of(m_games[right].programId).seconds;
        });
    }
    else if (sort == kSortBySize)
    {
        std::stable_sort(shown.begin(), shown.end(), [this](size_t left, size_t right) { return m_games[left].size > m_games[right].size; });
    }

    std::string count = TrCount("%n game", "%n games", m_games.size());
    if (shown.size() != m_games.size())
    {
        count = Tr("%1 of %2", {std::to_string(shown.size()), count});
    }
    SetText("games-count", count);

    m_shownGames = std::move(shown);
    m_shownDirty = true;
    grid->SetClass("empty", m_shownGames.empty());
    if (m_games.empty() && m_library.Scanning() && !m_library.Directories().empty())
    {
        grid->SetInnerRML("");
        return;
    }
    if (m_shownGames.empty())
    {
        const auto message = [](const std::string & text) {
            return "<div class=\"games-empty\"><img class=\"empty-state-icon\" src=\"images/gamepad.png\" /><div>" + Rml::StringUtilities::EncodeRml(text) +
                   "</div></div>";
        };
        grid->SetInnerRML(!m_games.empty()                  ? message(Tr("No game matches your search."))
                          : m_library.Directories().empty() ? message(Tr("No game directory yet."))
                                                            : message(Tr("No games found in your game directories.")));
        return;
    }
    RenderVisibleGames();
}

void MainWindow::RenderVisibleGames()
{
    Rml::Element * grid = m_document->GetElementById("games-grid");
    if (grid == nullptr || m_shownGames.empty())
    {
        return;
    }
    const bool list = grid->IsClassSet("list");
    float rowHeight = m_gameRowHeight;
    if (Rml::Element * item = grid->QuerySelector(".rom-game"))
    {
        const float height = item->GetOffsetHeight() + item->GetBox().GetEdge(Rml::BoxArea::Margin, Rml::BoxEdge::Bottom);
        rowHeight = height > 0.0f ? height : rowHeight;
    }
    const size_t columns = list ? 1 : static_cast<size_t>(std::max(grid->GetClientWidth() / CardStep(), 1.0f));
    const size_t rows = (m_shownGames.size() + columns - 1) / columns;
    size_t firstInSight = 0;
    size_t lastInSight = std::min<size_t>(rows, kUnmeasuredRows);
    if (rowHeight > 0.0f)
    {
        firstInSight = std::min(static_cast<size_t>(grid->GetScrollTop() / rowHeight), rows);
        lastInSight = std::min(static_cast<size_t>(std::ceil((grid->GetScrollTop() + grid->GetClientHeight()) / rowHeight)), rows);
    }
    const size_t margin = std::max<size_t>((lastInSight - firstInSight) / 2, kMinMarginRows);
    const size_t firstRow = firstInSight > margin ? firstInSight - margin : 0;
    const size_t lastRow = std::min(lastInSight + margin, rows);
    const bool sameLayout = !m_shownDirty && columns == m_gameColumns && rowHeight == m_gameRowHeight;
    if (sameLayout && firstRow == m_firstGameRow && lastRow == m_lastGameRow)
    {
        return;
    }
    const size_t count = m_shownGames.size();
    const auto itemsRml = [this, list, columns, count](size_t fromRow, size_t toRow) {
        std::string rml;
        for (size_t position = fromRow * columns; position < std::min(toRow * columns, count); position++)
        {
            rml += list ? ListRowRml(m_shownGames[position], position + 1) : CardRml(m_shownGames[position]);
        }
        return rml;
    };
    const auto spacerHeight = [rowHeight](size_t spacerRows) { return Rml::Property(static_cast<float>(spacerRows) * rowHeight, Rml::Unit::PX); };

    m_gameMenu->Close();
    m_pendingGameMenu = nullptr;

    Rml::Element * above = grid->GetFirstChild();
    Rml::Element * below = grid->GetLastChild();
    if (sameLayout && firstRow < m_lastGameRow && lastRow > m_firstGameRow && above != below && above->IsClassSet("games-spacer") &&
        below->IsClassSet("games-spacer"))
    {
        const auto drop = [grid, columns, count](size_t fromRow, size_t toRow, bool atStart) {
            for (size_t dropped = std::min(toRow * columns, count) - fromRow * columns; dropped > 0; dropped--)
            {
                grid->RemoveChild(grid->GetChild(atStart ? 1 : grid->GetNumChildren() - 2));
            }
        };
        const auto add = [this, grid](const std::string & rml, Rml::Element * before) {
            Rml::ElementPtr holder = m_document->CreateElement("div");
            holder->SetInnerRML(rml);
            while (holder->GetNumChildren() > 0)
            {
                SizeCard(grid->InsertBefore(holder->RemoveChild(holder->GetFirstChild()), before));
            }
        };
        if (firstRow > m_firstGameRow)
        {
            drop(m_firstGameRow, firstRow, true);
        }
        if (lastRow < m_lastGameRow)
        {
            drop(lastRow, m_lastGameRow, false);
        }
        if (firstRow < m_firstGameRow)
        {
            add(itemsRml(firstRow, m_firstGameRow), grid->GetChild(1));
        }
        if (lastRow > m_lastGameRow)
        {
            add(itemsRml(m_lastGameRow, lastRow), below);
        }
        above->SetProperty(Rml::PropertyId::Height, spacerHeight(firstRow));
        below->SetProperty(Rml::PropertyId::Height, spacerHeight(rows - lastRow));
    }
    else
    {
        grid->SetInnerRML("<div class=\"games-spacer\"/>" + itemsRml(firstRow, lastRow) + "<div class=\"games-spacer\"/>");
        grid->GetFirstChild()->SetProperty(Rml::PropertyId::Height, spacerHeight(firstRow));
        grid->GetLastChild()->SetProperty(Rml::PropertyId::Height, spacerHeight(rows - lastRow));
        ApplyIconSize();
    }
    m_shownDirty = false;
    m_gameColumns = columns;
    m_gameRowHeight = rowHeight;
    m_firstGameRow = firstRow;
    m_lastGameRow = lastRow;
    m_selectedGame = grid->QuerySelector(".rom-game.selected");
    ReleaseUnusedIcons(2 * (std::min(lastRow * columns, count) - firstRow * columns));
}

std::string MainWindow::IconRml(const std::string & icon)
{
    if (icon.empty())
    {
        return {};
    }
    if (const auto loaded = m_loadedIcons.find(icon); loaded != m_loadedIcons.end())
    {
        loaded->second = ++m_iconUse;
        return "<img src=\"" + Rml::StringUtilities::EncodeRml(icon) + "\"/>";
    }
    m_iconsPending = true;
    return "<img data-icon=\"" + Rml::StringUtilities::EncodeRml(icon) + "\"/>";
}

void MainWindow::LoadPendingIcons()
{
    Rml::Element * grid = m_iconsPending ? m_document->GetElementById("games-grid") : nullptr;
    if (grid == nullptr)
    {
        return;
    }
    Rml::ElementList pending;
    grid->QuerySelectorAll(pending, "img[data-icon]");
    m_iconsPending = pending.size() > kIconsPerFrame;
    const float top = grid->GetAbsoluteOffset(Rml::BoxArea::Border).y;
    const float bottom = top + grid->GetClientHeight();
    std::stable_partition(pending.begin(), pending.end(), [top, bottom](Rml::Element * image) {
        const float y = image->GetAbsoluteOffset(Rml::BoxArea::Border).y;
        return y + image->GetOffsetHeight() > top && y < bottom;
    });
    for (size_t i = 0; i < pending.size() && i < kIconsPerFrame; i++)
    {
        const std::string icon = pending[i]->GetAttribute<Rml::String>("data-icon", "");
        pending[i]->RemoveAttribute("data-icon");
        pending[i]->SetAttribute("src", icon);
        m_loadedIcons[icon] = ++m_iconUse;
    }
}

void MainWindow::ReleaseUnusedIcons(size_t inUse)
{
    const size_t kept = std::max(kMaxLoadedIcons, inUse);
    if (m_loadedIcons.size() <= kept)
    {
        return;
    }
    std::vector<std::pair<uint64_t, std::string>> byUse;
    for (const auto & [icon, use] : m_loadedIcons)
    {
        byUse.emplace_back(use, icon);
    }
    std::sort(byUse.begin(), byUse.end());
    byUse.resize(byUse.size() - kept);
    for (const auto & [use, icon] : byUse)
    {
        Rml::ReleaseTexture(icon);
        m_loadedIcons.erase(icon);
    }
}

std::string MainWindow::CardRml(size_t index)
{
    const GameEntry & game = m_games[index];
    const bool selected = game.path == m_selectedPath && game.programId == m_selectedProgramId;
    std::string rml = std::string("<div class=\"rom-game rom-card") + (selected ? " selected" : "") + "\" data-game=\"" + std::to_string(index) +
                      "\"><div class=\"rom-art\" data-title-delay=\"" + std::to_string(kCoverTitleDelayMs) + "\" title=\"" +
                      Rml::StringUtilities::EncodeRml(game.title) + "\">";
    rml += IconRml(game.icon);
    const std::string played = PlayTime::Duration(m_playTime.Of(game.programId).seconds);
    if (!played.empty())
    {
        rml += "<div class=\"rom-played\"><img src=\"images/clock.png\"/><div>" + Rml::StringUtilities::EncodeRml(played) + "</div></div>";
    }
    std::string details;
    if (!game.version.empty())
    {
        details += "<div class=\"rom-chip rom-version\"><img src=\"images/gear.png\"/><div>v" + Rml::StringUtilities::EncodeRml(game.version) + "</div></div>";
    }
    if (!game.version.empty() && !game.publisher.empty())
    {
        details += "<div class=\"rom-chip-rule\"/>";
    }
    if (!game.publisher.empty())
    {
        details += "<div class=\"rom-chip rom-publisher\"><img src=\"images/building.png\"/><div>" + Rml::StringUtilities::EncodeRml(TrUpper(game.publisher)) + "</div></div>";
    }
    rml += "</div><div class=\"rom-info\"><div class=\"rom-title\">" + Rml::StringUtilities::EncodeRml(game.title) + "</div>" +
           (details.empty() ? std::string() : "<div class=\"rom-details\">" + details + "</div>") + "</div></div>";
    return rml;
}

std::string MainWindow::ListRowRml(size_t index, size_t number)
{
    const GameEntry & game = m_games[index];
    const bool selected = game.path == m_selectedPath && game.programId == m_selectedProgramId;
    const PlayTime::Record played = m_playTime.Of(game.programId);
    const std::string format = FileFormat(game.path);
    const char * kind = format.find("XCI") != std::string::npos ? " xci" : format.find("NSP") != std::string::npos ? " nsp" : "";
    const auto cell = [](const char * name, const std::string & text) {
        return std::string("<div class=\"col-") + name + "\">" + Rml::StringUtilities::EncodeRml(text) + "</div>";
    };
    std::string rml = std::string("<div class=\"rom-game rom-row") + (selected ? " selected" : "") + "\" data-game=\"" + std::to_string(index) + "\">" +
                      cell("number", std::to_string(number)) + "<div class=\"col-cover\">";
    rml += IconRml(game.icon);
    rml += "</div>" + cell("game", game.title) + cell("publisher", game.publisher) + cell("version", game.version.empty() ? std::string() : "v" + game.version) +
           cell("last", played.lastPlayed != 0 ? PlayTime::Ago(played.lastPlayed) : Tr("Never")) + cell("time", PlayTime::Duration(played.seconds)) +
           "<div class=\"col-format\">" +
           (format.empty() ? std::string() : "<div class=\"format-badge" + std::string(kind) + "\">" + Rml::StringUtilities::EncodeRml(format) + "</div>") + "</div>" +
           cell("size", FileSize(game.size)) + "<div class=\"col-actions\"><div class=\"row-play\"><img src=\"images/play.png\"/><div>" +
           Rml::StringUtilities::EncodeRml(Tr("Play")) + "</div></div><div class=\"rom-more\" title=\"" + Rml::StringUtilities::EncodeRml(Tr("More actions")) +
           "\"><div class=\"rom-dot\"/><div class=\"rom-dot\"/><div class=\"rom-dot\"/></div></div></div>";
    return rml;
}

void MainWindow::ApplyLibraryOptions()
{
    const bool information = uiSettings.showCardInformation;
    const bool playTime = uiSettings.showPlayTime;
    const bool list = uiSettings.gameListView;
    const bool title = uiSettings.showCardTitle;
    if (Rml::Element * grid = m_document->GetElementById("games-grid"))
    {
        grid->SetClass("list", list);
        grid->SetClass("no-details", !information);
        grid->SetClass("no-play-time", !playTime);
        grid->SetClass("no-title", !title);
    }
    if (Rml::Element * slider = m_document->GetElementById("icon-size-slider"))
    {
        const Rml::String value = std::to_string(m_iconSize);
        if (slider->GetAttribute<Rml::String>("value", "") != value)
        {
            slider->SetAttribute("value", value);
        }
    }
    const auto setClass = [this](const char * id, const char * name, bool set) {
        if (Rml::Element * element = m_document->GetElementById(id))
        {
            element->SetClass(name, set);
        }
    };
    setClass("card-title", "on", title);
    setClass("card-information", "on", information);
    setClass("play-time", "on", playTime);
    setClass("view-grid", "active", !list);
    setClass("view-list", "active", list);
    setClass("games-details-toggle", "active", uiSettings.showGameDetails);
    setClass("rom-browser-panel", "details-open", uiSettings.showGameDetails);
    setClass("games-list-header", "shown", list);
    if (Rml::Element * header = m_document->GetElementById("games-list-header"))
    {
        for (int i = 0; i < header->GetNumChildren(); i++)
        {
            Rml::Element * heading = header->GetChild(i);
            heading->SetClass("sorted", heading->GetAttribute<int>("data-sort", -1) == GameSort());
        }
    }
    SetText("games-sort-text", Tr(kGameSorts[GameSort()]));
}

void MainWindow::LayoutList()
{
    Rml::Element * grid = m_document->GetElementById("games-grid");
    Rml::Element * header = m_document->GetElementById("games-list-header");
    Rml::Element * main = grid != nullptr ? grid->GetParentNode() : nullptr;
    if (header == nullptr || main == nullptr || !grid->IsClassSet("list"))
    {
        return;
    }
    const float width = grid->GetClientWidth() / m_document->GetContext()->GetDensityIndependentPixelRatio();
    for (const ListColumn & column : kOptionalListColumns)
    {
        main->SetClass(column.hiddenClass, width < column.minWidth);
    }
    const float scrollBar = std::max(grid->GetOffsetWidth() - grid->GetClientWidth(), 0.0f);
    if (scrollBar != m_listScrollBar)
    {
        m_listScrollBar = scrollBar;
        header->SetProperty(Rml::PropertyId::PaddingRight, Rml::Property(scrollBar, Rml::Unit::PX));
    }
}

void MainWindow::SaveIconSize()
{
    if (m_iconSizeChanged != 0)
    {
        m_iconSizeChanged = 0;
        uiSettings.gameIconSize = m_iconSize;
        SaveUISetting();
    }
}

void MainWindow::FitWindowToCards()
{
    int width = 0, height = 0;
    m_window.GetClientSize(width, height);
    Rml::Element * grid = m_document->GetElementById("games-grid");
    if (m_phase == EmulatorSession::Phase::Idle && grid != nullptr && !grid->IsClassSet("list") && grid->GetOffsetWidth() > 0.0f)
    {
        const float scrollbar = static_cast<float>(kScrollbarWidth) * m_document->GetContext()->GetDensityIndependentPixelRatio();
        const float around = static_cast<float>(width) * m_window.PixelScale() - grid->GetOffsetWidth();
        const float columns = std::max(std::round((grid->GetOffsetWidth() - scrollbar) / CardStep()), 1.0f);
        width = static_cast<int>(std::ceil((around + columns * CardStep() + scrollbar + 1.0f) / m_window.PixelScale()));
    }
    m_window.SetClientSize(width, height);
    m_window.Centre();
}

void MainWindow::SetMenuRange(int id, int value)
{
    if (id == kPerformanceSizeRange)
    {
        m_performance->SetSize(value);
    }
    else if (id == kPerformanceRateRange)
    {
        m_performance->SetRate(value);
    }
    else if (id == kPerformanceLengthRange)
    {
        m_performance->SetLength(value);
    }
    if (id == kFilterSharpnessRange && value != SettingsStore::GetInstance().GetInt(NXVideoSetting::FSPSharpness))
    {
        SettingsStore::GetInstance().SetInt(NXVideoSetting::FSPSharpness, value);
        m_session.FlushSettings();
        UpdateStatusWidgets();
    }
}

void MainWindow::SetDockedMode(bool docked)
{
    SettingsStore::GetInstance().SetInt(NXOsSetting::DockedMode, static_cast<int32_t>(docked ? DockedMode::Docked : DockedMode::Handheld));
    m_session.FlushSettings();
    UpdateStatusWidgets();
}

void MainWindow::RefreshProfiles()
{
    m_session.Profiles().Refresh();
    m_profiles = m_session.Profiles().List();
    UpdateStatusWidgets();
}

void MainWindow::UpdateStatusWidgets()
{
    SettingsStore & settings = SettingsStore::GetInstance();
    const bool docked = IsDocked();
    const auto & scale = kResolutionScales[ResolutionScale()];
    SetText("docked-text", docked ? Tr("Docked") : Tr("Handheld"));
    if (Rml::Element * icon = m_document->GetElementById("docked-icon"))
    {
        const char * image = docked ? "images/docked.png" : "images/handheld.png";
        if (icon->GetAttribute<Rml::String>("src", "") != image)
        {
            icon->SetAttribute("src", image);
        }
    }
    const ScalingFilterChoice * filter = FindScalingFilter(settings.GetInt(NXVideoSetting::ScalingFilter));
    SetText("scaling-filter-text", filter != nullptr ? Tr(filter->label) : Tr("Gaussian"));
    SetText("resolution-scale-text", scale.label);
    SetText("resolution", std::to_string(static_cast<int>(std::lround((docked ? kDockedHeight : kHandheldHeight) * scale.factor))) + "p");
    if (Rml::Element * select = m_document->GetElementById("resolution-scale"))
    {
        const bool locked = m_session.IsRunning();
        select->SetClass("locked", locked);
        const std::string hint = Tr("Resolution scale: higher values give a sharper picture but need more graphics power.") +
                                 (locked ? "\n" + Tr("The resolution scale cannot change while a game is running") : "");
        if (select->GetAttribute<Rml::String>("title", "") != hint)
        {
            select->SetAttribute("title", hint);
        }
    }
    if (Rml::Element * select = m_document->GetElementById("profile"))
    {
        const bool shown = m_profiles.size() > 1;
        m_document->SetClass("several-profiles", shown);
        if (shown)
        {
            SetText("profile-text", m_profiles[UserProfiles::Active(m_profiles.size())].name);
            const bool locked = m_session.IsRunning();
            select->SetClass("locked", locked);
            const std::string hint = locked ? Tr("The profile cannot change while a game is running") : Tr("The profile games start as: its saves are the ones used");
            if (select->GetAttribute<Rml::String>("title", "") != hint)
            {
                select->SetAttribute("title", hint);
            }
        }
    }
    if (m_volume != nullptr)
    {
        m_volume->Show();
    }
}

void MainWindow::ShowGameMenu(Rml::Element * origin)
{
    Rml::Element * card = UiEvents::ClosestWithClass(origin, "rom-game");
    const size_t index = card != nullptr ? static_cast<size_t>(card->GetAttribute<int>("data-game", -1)) : m_games.size();
    if (index >= m_games.size())
    {
        return;
    }
    m_menuGame = m_games[index].path;
    const uint64_t id = m_games[index].programId;
    std::error_code error;
    const bool cacheExists = id != 0 && std::filesystem::is_directory(GameCache::Directory(Common::FS::GetYuzuPath(Common::FS::YuzuPath::ShaderDir), id), error);
    const std::vector<MenuItem> items = {
        MenuItem::Action(static_cast<int>(Action::StartMenuGame), Tr("Start Game")),
        MenuItem::Separator(),
        MenuItem::Action(static_cast<int>(Action::OpenMenuGameSaves), Tr("Open saves"), {}, !GameDetailsReader::SaveDirectory(id).empty()),
        MenuItem::Submenu(Tr("Cache"), {
            MenuItem::Action(static_cast<int>(Action::OpenMenuGameCache), Tr("Open cache"), {}, cacheExists),
            MenuItem::Action(static_cast<int>(Action::ClearMenuGameCache), Tr("Clear cache"), {}, cacheExists && !m_session.IsRunning()),
        }),
        MenuItem::Action(static_cast<int>(Action::OpenMenuGameDirectory), Tr("Open Containing Folder")),
        MenuItem::Action(static_cast<int>(Action::CopyMenuGamePath), Tr("Copy File Path")),
    };
    if (m_gameMenuPoint)
    {
        m_gameMenu->ShowAt(items, *m_gameMenuPoint);
    }
    else
    {
        m_gameMenu->Show(items, origin);
    }
}

void MainWindow::ShowSelectMenu(Rml::Element * origin)
{
    std::vector<MenuItem> items;
    if (origin->GetId() == "games-sort")
    {
        const int current = GameSort();
        for (const int sort : kOfferedSorts)
        {
            items.push_back(MenuItem::Choice(static_cast<int>(Action::GameSortFirst) + sort, Tr(kGameSorts[sort]), sort == current));
        }
    }
    else if (origin->GetId() == "profile")
    {
        const int active = UserProfiles::Active(m_profiles.size());
        for (int profile = 0; profile < static_cast<int>(m_profiles.size()); profile++)
        {
            items.push_back(MenuItem::Choice(static_cast<int>(Action::ProfileFirst) + profile, m_profiles[profile].name, profile == active));
        }
        items.push_back(MenuItem::Separator());
        items.push_back(MenuItem::Action(static_cast<int>(Action::OpenProfiles), Tr("Manage profiles")));
    }
    else if (origin->GetId() == "docked-mode")
    {
        const bool docked = IsDocked();
        items.push_back(MenuItem::Choice(static_cast<int>(Action::SetDockedMode), Tr("Docked"), docked));
        items.push_back(MenuItem::Choice(static_cast<int>(Action::SetHandheldMode), Tr("Handheld"), !docked));
    }
    else if (origin->GetId() == "scaling-filter")
    {
        const SettingsStore & store = SettingsStore::GetInstance();
        const int current = store.GetInt(NXVideoSetting::ScalingFilter);
        for (const ScalingFilterChoice & filter : kScalingFilters)
        {
            const int value = static_cast<int>(filter.value);
            items.push_back(MenuItem::Check(static_cast<int>(Action::ScalingFilterFirst) + value, Tr(filter.label), value == current));
            MenuItem & item = items.back();
            item.hint = Tr(filter.hint);
            if (filter.value == ScalingFilter::Fsr)
            {
                item.range = MenuItem::Range{kFilterSharpnessRange, kSharpnessMax, std::clamp(store.GetInt(NXVideoSetting::FSPSharpness), 0, kSharpnessMax),
                                             value == current, value == current ? Tr("Sharpness of the FSR filter") : Tr("Sharpness: once FSR is the filter")};
            }
        }
    }
    else if (origin->GetId() == "resolution-scale")
    {
        const int current = ResolutionScale();
        for (int scale = 0; scale < kResolutionScaleCount; scale++)
        {
            items.push_back(MenuItem::Choice(static_cast<int>(Action::ResolutionScaleFirst) + scale, kResolutionScales[scale].label, scale == current));
        }
    }
    if (items.empty())
    {
        m_gameMenu->Close();
    }
    else if (m_gameMenu->IsOpen() && m_gameMenu->Origin() == origin)
    {
        m_gameMenu->Update(items);
    }
    else
    {
        m_gameMenu->Show(items, origin);
    }
}

void MainWindow::ApplyIconSize()
{
    Rml::Element * grid = m_document->GetElementById("games-grid");
    if (grid == nullptr)
    {
        return;
    }
    const int step = GameIconStep(m_iconSize);
    for (int other = 1; other <= kGameIconStepCount; other++)
    {
        grid->SetClass("size-" + std::to_string(other), other == step);
    }
    for (int i = 0; i < grid->GetNumChildren(); i++)
    {
        SizeCard(grid->GetChild(i));
    }
}

void MainWindow::SizeCard(Rml::Element * card) const
{
    Rml::Element * art = card->IsClassSet("rom-card") ? card->GetFirstChild() : nullptr;
    if (art != nullptr)
    {
        card->SetProperty(Rml::PropertyId::Width, Rml::Property(static_cast<float>(m_iconSize + 2 * kCardBorder), Rml::Unit::DP));
        art->SetProperty(Rml::PropertyId::Height, Rml::Property(static_cast<float>(m_iconSize), Rml::Unit::DP));
    }
}

float MainWindow::CardStep() const
{
    const float ratio = m_document->GetContext()->GetDensityIndependentPixelRatio();
    return static_cast<float>(m_iconSize + 2 * kCardBorder + (GameIconStep(m_iconSize) == 1 ? kSmallCardGap : kCardGap)) * ratio;
}

const GameEntry * MainWindow::SelectedGame() const
{
    const auto game = std::find_if(m_games.begin(), m_games.end(), [this](const GameEntry & entry) { return entry.path == m_selectedPath; });
    return game != m_games.end() ? &*game : nullptr;
}

void MainWindow::RequestSelectedDetails()
{
    if (SelectedGame() != nullptr && m_details.path != m_selectedPath && !m_session.IsRunning() && uiSettings.showGameDetails)
    {
        m_library.RequestDetails(m_session.Loader(), m_selectedPath);
    }
}

void MainWindow::RenderDetails()
{
    m_detailsDirty = false;
    const GameEntry * game = SelectedGame();
    if (game == nullptr)
    {
        m_detailsPanel->Clear();
        return;
    }
    const PlayTime::Record played = m_playTime.Of(game->programId);
    m_detailsPanel->ShowGame(game->title, game->icon,
                             {
                                 {Tr("Version"), game->version.empty() ? std::string() : "v" + game->version},
                                 {Tr("Publisher"), game->publisher},
                                 {Tr("Format"), FileFormat(game->path)},
                                 {Tr("Size"), FileSize(game->size)},
                                 {Tr("Last played"), PlayTime::Date(played.lastPlayed)},
                                 {Tr("Play time"), PlayTime::Duration(played.seconds)},
                                 {Tr("Title ID"), game->programId != 0 ? std::string(stdstr_f("%016llX", static_cast<unsigned long long>(game->programId))) : std::string()},
                             });
    m_detailsPanel->ShowDetails(m_details.path == m_selectedPath && m_details.programId != 0 ? &m_details : nullptr, m_session.IsRunning());
}

void MainWindow::ToggleAddOn(size_t index)
{
    if (m_details.path != m_selectedPath || index >= m_details.addOns.size() || m_session.IsRunning())
    {
        return;
    }
    const std::vector<std::string> disabled = GameDetailsReader::DisabledAfterToggle(m_details, index);
    for (GameAddOn & addOn : m_details.addOns)
    {
        addOn.enabled = std::find(disabled.begin(), disabled.end(), addOn.key) == disabled.end();
    }
    m_detailsDirty = true;
    m_library.RequestDetails(m_session.Loader(), m_selectedPath, m_details.programId, disabled);
}

void MainWindow::SelectGame(Rml::Element * card)
{
    if (card == m_selectedGame && (card != nullptr || m_selectedPath.empty()))
    {
        return;
    }
    m_detailsDirty = true;
    if (m_selectedGame != nullptr)
    {
        m_selectedGame->SetClass("selected", false);
    }
    m_selectedGame = card;
    m_selectedPath.clear();
    m_selectedProgramId = 0;
    if (m_selectedGame != nullptr)
    {
        m_selectedGame->SetClass("selected", true);
        const size_t index = static_cast<size_t>(m_selectedGame->GetAttribute<int>("data-game", -1));
        if (index < m_games.size())
        {
            m_selectedPath = m_games[index].path;
            m_selectedProgramId = m_games[index].programId;
        }
    }
    RequestSelectedDetails();
}

bool MainWindow::EnsureUtilityGuest(bool preserveProfiles)
{
    if (m_session.HasGuest())
    {
        return true;
    }
    const bool previous = m_keepProfileDialogDuringSetup;
    m_keepProfileDialogDuringSetup = preserveProfiles;
    bool ready = false;
    try
    {
        ready = m_session.EnsureGuest();
    }
    catch (...)
    {
        m_keepProfileDialogDuringSetup = previous;
        throw;
    }
    m_keepProfileDialogDuringSetup = previous;
    if (ready)
    {
        m_utilityGuest = true;
    }
    return ready;
}

bool MainWindow::ShowSettingsPage(SettingsPage page)
{
    if (m_settingsContext.IsGame() && page != SettingsPage::General && page != SettingsPage::Advanced && page != SettingsPage::Debug) return false;
    switch (page)
    {
    case SettingsPage::General:
        m_generalDialog->Show();
        return m_generalDialog->IsVisible();
    case SettingsPage::Profiles:
        // A2d supplies an offline snapshot: viewing/selecting profiles does
        // not require an OS. Profile mutations attach the OS only on demand.
        if (m_session.FrontendReady())
        {
            m_profilesDialog->Show();
        }
        return m_profilesDialog->IsVisible();
    case SettingsPage::Directories:
        m_directoriesDialog->Show();
        return m_directoriesDialog->IsVisible();
    case SettingsPage::Advanced:
    case SettingsPage::Debug:
        // Audio enumeration is requested lazily by AdvancedDialog on its
        // system tab. Graphics, debug and shortcuts use SettingsStore only.
        m_advancedDialog->ShowPage(page);
        return m_advancedDialog->IsVisible();
    }
    return false;
}

void MainWindow::ShowPanel(Panel panel)
{
    static const struct
    {
        Panel panel;
        const char * id;
    } panels[] = {
        {Panel::RomBrowser, "rom-browser-panel"},
        {Panel::Loading, "loading-panel"},
        {Panel::Pause, "pause-panel"},
    };
    if (panel != m_panel)
    {
        m_gameMenu->Close();
    }
    m_panel = panel;
    for (const auto & entry : panels)
    {
        if (Rml::Element * element = m_document->GetElementById(entry.id))
        {
            element->SetClass("active", entry.panel == panel);
        }
    }
    m_document->SetClass("video-visible", panel == Panel::Renderer || panel == Panel::Pause);
}

void MainWindow::SyncInterfaceLanguage()
{
    const std::string language = InterfaceLanguage::Code();
    if (language == m_interfaceLanguage)
    {
        return;
    }
    m_interfaceLanguage = language;
    InterfaceLanguage::Use(language, m_assetDir);

    Translation::RefreshDocument(*m_document);
    m_aboutDialog->RefreshTranslation();
    m_firmwareDialog->RefreshTranslation();
    m_controllerDialog->RefreshTranslation();
    m_advancedDialog->RefreshLanguage();
    m_directoriesDialog->RefreshTranslation();
    m_profilesDialog->RefreshTranslation();
    m_generalDialog->RefreshLanguage();

    m_menuBar->Close();
    m_gameMenu->Close();
    m_pendingGameMenu = nullptr;
    m_pendingSelectMenu = nullptr;
    RebuildMenu();
    ApplyLibraryOptions();
    RenderGames();
    RenderVisibleGames();
    RenderDetails();
    UpdateScanIndicator();
    SyncPhase();
    UpdateStatusWidgets();
    UpdateStatusText();
    if (m_phase == EmulatorSession::Phase::Loading)
    {
        UpdateLoadingScreen();
    }
}

void MainWindow::SyncPhase()
{
    const EmulatorSession::Phase phase = m_session.CurrentPhase();
    const bool emulating = m_session.IsRunning() && (phase == EmulatorSession::Phase::Running || phase == EmulatorSession::Phase::Paused);
    const bool paused = emulating && phase == EmulatorSession::Phase::Paused;
    m_document->SetClass("emulating", emulating);
    m_document->SetClass("game-paused", paused);
    m_document->SetClass("game-controls-visible", uiSettings.showEmulationControls && (m_session.IsRunning() || !m_pendingGame.empty()));
    for (const char * id : {"title-pause", "title-restart"})
    {
        if (Rml::Element * button = m_document->GetElementById(id))
        {
            if (!emulating && !button->HasAttribute("disabled"))
            {
                button->SetAttribute("disabled", "");
            }
            else if (emulating && button->HasAttribute("disabled"))
            {
                button->RemoveAttribute("disabled");
            }
        }
    }
    if (Rml::Element * pause = m_document->GetElementById("title-pause"))
    {
        pause->SetClass("active", paused);
        const std::string label = paused ? Tr("Resume emulation (F2)") : Tr("Pause emulation (F2)");
        if (pause->GetAttribute<Rml::String>("title", "") != label)
        {
            pause->SetAttribute("title", label);
        }
    }
    if (phase != m_phase)
    {
        m_phase = phase;
        m_menuDirty = true;
        m_lastMeasure = 0;
        m_lastStatusUpdate = 0;
        UpdateDiscord();
        if (phase == EmulatorSession::Phase::Idle)
        {
            m_playingProgram = 0;
            m_details = {};
            RequestSelectedDetails();
            m_gamesDirty = true;
            m_hideUi = false;
            m_window.SetFullscreen(false);
            SetTitle({});
        }
        else if (phase == EmulatorSession::Phase::Running && m_applyStartOptions)
        {
            m_applyStartOptions = false;
            if (IRomInfoPtr info(m_session.Loader().LoadedRomInfo()); info)
            {
                info->ReadProgramId(m_playingProgram);
            }
            m_hideUi = uiSettings.startGamesWithUiHidden;
            if (uiSettings.startGamesInFullscreen)
            {
                m_window.SetFullscreen(true);
            }
        }
    }

    switch (m_phase)
    {
    case EmulatorSession::Phase::Idle: ShowPanel(Panel::RomBrowser); break;
    case EmulatorSession::Phase::Loading:
        if (m_gameShown)
        {
            ShowPanel(Panel::RomBrowser);
            break;
        }
        UpdateLoadingScreen();
        ShowPanel(Panel::Loading);
        break;
    case EmulatorSession::Phase::Paused: ShowPanel(Panel::Pause); break;
    case EmulatorSession::Phase::Running:
        m_gameShown = true;
        ShowPanel(Panel::Renderer);
        break;
    }
    if (m_launchReady && !m_pendingGame.empty())
    {
        ShowPanel(Panel::Loading);
    }
    SetUiHidden(m_phase != EmulatorSession::Phase::Idle && (m_hideUi || m_window.IsFullscreen()));
    m_performance->Show(uiSettings.showPerformance && m_gameShown && m_phase != EmulatorSession::Phase::Idle);
}

void MainWindow::UpdateDiscord()
{
    m_discord.SetEnabled(uiSettings.enableDiscordPresence);
    m_discord.Update(m_phase != EmulatorSession::Phase::Idle, SettingsStore::GetInstance().GetString(NXCoreSetting::GameName));
}

void MainWindow::DiscordSettingChanged(const char * /*setting*/, void * userData)
{
    static_cast<MainWindow *>(userData)->UpdateDiscord();
}

void MainWindow::SetUiHidden(bool hidden)
{
    m_document->SetClass("ui-hidden", hidden);
}

void MainWindow::RefreshFirmwareVersion()
{
    char firmware[32]{};
    const uint32_t length = m_session.Loader().GetInstalledFirmwareDisplayVersion(firmware, sizeof(firmware));
    m_firmwareVersion.assign(firmware, length);
    UpdateStatusText();
}

void MainWindow::LoadFile()
{
    const std::string filter = FileFilter({{Tr("Switch games"), "*.dxci;*.dnsp;*.nro"}, {Tr("All files"), "*.*"}});
    Path selected;
    if (FileSelect(m_window.NativeHandle(), Path(Path::CURRENT_DIRECTORY), filter.c_str(), true, selected))
    {
        OpenGame(static_cast<const char *>(selected));
    }
}

void MainWindow::AnnounceGame(const std::string & path)
{
    m_gameShown = false;
    if (Rml::Element * search = m_document->GetElementById("games-search"))
    {
        search->Blur();
    }
    const auto game = std::find_if(m_games.begin(), m_games.end(), [&path](const GameEntry & entry) { return entry.path == path; });
    if (game != m_games.end())
    {
        m_loadingScreen->Announce(game->title, game->icon);
    }
    else
    {
        m_loadingScreen->Announce({}, {});
    }
}

void MainWindow::UpdateLoadingScreen()
{
    if (!m_loadingScreen->HasGame())
    {
        IRomInfoPtr info(m_session.Loader().LoadedRomInfo());
        if (info)
        {
            m_loadingScreen->SetGame(*info);
        }
    }
    m_loadingScreen->Update();
}

void MainWindow::LayoutGameArea()
{
    Rml::Element * contents = m_document->GetElementById("main-contents");
    if (contents == nullptr)
    {
        return;
    }
    const Rml::Vector2f offset = contents->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = contents->GetBox().GetSize(Rml::BoxArea::Border);
    const PixelRect area{
        static_cast<int>(std::lround(offset.x)),
        static_cast<int>(std::lround(offset.y)),
        static_cast<int>(std::lround(size.x)),
        static_cast<int>(std::lround(size.y)),
    };
    if (area.width <= 0 || area.height <= 0 || area == m_gameArea)
    {
        return;
    }
    m_gameArea = area;
    const float scale = m_window.PixelScale();
    m_session.SetViewportSize(static_cast<uint32_t>(std::lround(size.x / scale)), static_cast<uint32_t>(std::lround(size.y / scale)));
}

void MainWindow::SetTitle(const std::string & game)
{
    SetText("title-text", game);
    if (Rml::Element * title = m_document->GetElementById("title-text"))
    {
        title->SetAttribute("title", game);
    }
    if (game.empty())
    {
        m_document->SetClass("has-game-icon", false);
        if (Rml::Element * icon = m_document->GetElementById("title-game-icon"))
        {
            icon->RemoveAttribute("src");
        }
    }
    m_document->SetClass("has-game", !game.empty());
    ShowVersion();
    m_window.SetTitle("NXEmu " + std::string(VER_FILE_VERSION_STR) + (game.empty() ? std::string() : " - " + game));
}

void MainWindow::ShowVersion()
{
    const std::string version = VER_FILE_VERSION_STR;
    const size_t dash = version.find('-');
    const std::string shortVersion = version.substr(0, dash == std::string::npos ? dash : version.find('-', dash + 1));
    SetText("title-version", shortVersion);
    if (Rml::Element * badge = m_document->GetElementById("title-version"))
    {
        badge->SetAttribute("title", version);
        badge->SetClass("copied", m_versionCopiedUntil != 0);
    }
}

void MainWindow::RevealMouseCursor()
{
    m_mouseMovedAt = SDL_GetTicks64();
    if (m_mouseHidden)
    {
        m_mouseHidden = false;
        SDL_ShowCursor(SDL_ENABLE);
    }
}

void MainWindow::UpdateMouseCursor()
{
    bool hide = false;
    if (m_phase == EmulatorSession::Phase::Running && GameArea() != nullptr && SDL_GetTicks64() - m_mouseMovedAt >= kMouseHideMs && !DialogOpen() &&
        !m_menuBar->IsOpen() && !m_gameMenu->IsOpen())
    {
        int x = 0;
        int y = 0;
        SDL_GetMouseState(&x, &y);
        const float scale = m_window.PixelScale();
        const int px = static_cast<int>(x * scale);
        const int py = static_cast<int>(y * scale);
        const bool over = px >= m_gameArea.x && py >= m_gameArea.y && px < m_gameArea.x + m_gameArea.width && py < m_gameArea.y + m_gameArea.height;
        hide = over && (SDL_GetWindowFlags(m_window.Handle()) & SDL_WINDOW_MOUSE_FOCUS) != 0;
    }
    if (hide != m_mouseHidden)
    {
        m_mouseHidden = hide;
        SDL_ShowCursor(hide ? SDL_DISABLE : SDL_ENABLE);
    }
}

void MainWindow::SyncWindowState()
{
    const bool fullscreen = m_window.IsFullscreen();
    const Hidden hiddenBy = fullscreen ? Hidden::Fullscreen : m_hideUi ? Hidden::Interface : Hidden::Nothing;
    if (hiddenBy != m_hiddenBy)
    {
        m_hiddenBy = hiddenBy;
        m_hintDue = hiddenBy != Hidden::Nothing;
        m_hintUntil = 0;
    }
    if (m_hintDue && m_panel == Panel::Renderer)
    {
        m_hintDue = false;
        m_hintUntil = SDL_GetTicks64() + kGameHintMs;
        SetText("game-hint-text", fullscreen ? Tr("Press %1 or %2 to leave fullscreen", {Hotkeys::Get(HotkeyId::ExitFullscreen).Label(), Hotkeys::Get(HotkeyId::Fullscreen).Label()})
                                             : Tr("Press %1 to show the interface", {Hotkeys::Get(HotkeyId::HideUi).Label()}));
    }
    if (m_hintUntil != 0 && SDL_GetTicks64() >= m_hintUntil)
    {
        m_hintUntil = 0;
    }
    if (Rml::Element * hint = m_document->GetElementById("game-hint"))
    {
        hint->SetClass("shown", m_hintUntil != 0);
    }
    if (m_versionCopiedUntil != 0 && SDL_GetTicks64() >= m_versionCopiedUntil)
    {
        m_versionCopiedUntil = 0;
        ShowVersion();
    }
    m_document->SetClass("fullscreen", fullscreen);
    m_document->SetClass("maximised", m_window.IsMaximised());
    if (m_window.HiddenFrame() != m_hiddenFrame)
    {
        m_hiddenFrame = m_window.HiddenFrame();
        m_document->SetProperty(Rml::PropertyId::PaddingTop, Rml::Property(static_cast<float>(m_hiddenFrame), Rml::Unit::PX));
        m_document->SetProperty(Rml::PropertyId::PaddingRight, Rml::Property(static_cast<float>(m_hiddenFrame), Rml::Unit::PX));
        m_document->SetProperty(Rml::PropertyId::PaddingBottom, Rml::Property(static_cast<float>(m_hiddenFrame), Rml::Unit::PX));
        m_document->SetProperty(Rml::PropertyId::PaddingLeft, Rml::Property(static_cast<float>(m_hiddenFrame), Rml::Unit::PX));
    }
}

const PixelRect * MainWindow::GameArea() const
{
    const bool playing = m_phase == EmulatorSession::Phase::Running || m_phase == EmulatorSession::Phase::Paused;
    return playing && m_gameArea.width > 0 ? &m_gameArea : nullptr;
}

void MainWindow::ResetWindowSize(int width, int height)
{
    m_window.SetFullscreen(false);
    m_window.SetClientSize(width, height);
}

void MainWindow::MeasureGame()
{
    m_speedText.clear();
    const bool measured = m_longestMeasured;
    m_longestMeasured = false;
    if (m_session.IsRunning())
    {
        IOperatingSystem & os = m_session.Modules().OperatingSystem();
        m_longestMeasured = m_performance->Shown();
        os.MeasureGameFrames(m_longestMeasured);
        const PerfStatsResults stats = os.GetAndResetPerfStats();
        if (measured && m_longestMeasured && !os.IsEmulationPaused())
        {
            m_performance->Add(stats, m_session.Modules().Video().ShadersBuilding(), !SettingsStore::GetInstance().GetBool(NXOsSetting::UseSpeedLimit));
        }
        if (std::isfinite(stats.average_game_fps) && stats.average_game_fps != 0)
        {
            const double frametime = std::isfinite(stats.frametime) ? stats.frametime * 1000.0 : 0.0;
            m_speedText = stdstr_f("%.0f FPS (%.2f ms)", std::round(stats.average_game_fps), frametime);
        }
    }
    UpdateStatusText();
}

void MainWindow::UpdateStatusText()
{
    std::string cpu_backend;
    if (uiSettings.showCpuBackend && m_session.IsRunning())
    {
        if (IKernelProcess* process = m_session.Modules().OperatingSystem().CurrentProcess())
        {
            // Use the same eligibility as CreateCpuCore, after process creation. The
            // configured backend alone can still fall back for this particular game.
            const bool native = process->Is64Bit() && process->IsApplication() &&
                                SettingsStore::GetInstance().GetBool(NXCpuSetting::NceEnabled);
            cpu_backend = std::string(native ? "NCE" : "Dynarmic") +
                          (process->Is64Bit() ? " · AArch64 · " : " · AArch32 · ") +
                          std::to_string(process->GetPageTable().GetAddressSpaceWidth()) + "-bit";
        }
    }
    SetText("title-cpu-backend", cpu_backend);
    m_document->SetClass("show-cpu-backend", !cpu_backend.empty());

    std::vector<std::string> parts;
    if (m_session.IsRunning())
    {
        if (m_session.Modules().OperatingSystem().IsEmulationPaused())
        {
            parts.push_back(Tr("Paused"));
        }
        const uint32_t shaders = m_session.Modules().Video().ShadersBuilding();
        if (shaders > 0)
        {
            parts.push_back(TrCount("Building: %n shader", "Building: %n shaders", shaders));
        }
        if (!m_speedText.empty())
        {
            parts.push_back(m_speedText);
        }
    }
    else if (!m_firmwareVersion.empty())
    {
        parts.push_back(Tr("Firmware: %1", {m_firmwareVersion}));
    }

    std::string status;
    for (const std::string & part : parts)
    {
        status += (status.empty() ? "" : " | ") + part;
    }
    SetText("status-text", status);
    m_document->SetClass("speed-unlocked", m_session.IsRunning() && !m_speedText.empty() && !SettingsStore::GetInstance().GetBool(NXOsSetting::UseSpeedLimit));
}

void MainWindow::SetText(const char * id, const std::string & text)
{
    Rml::Element * element = m_document->GetElementById(id);
    if (element == nullptr)
    {
        return;
    }
    const Rml::String rml = Rml::StringUtilities::EncodeRml(text);
    if (element->GetInnerRML() != rml)
    {
        element->SetInnerRML(rml);
    }
}

void MainWindow::StopGame()
{
    if (!m_pendingGame.empty())
    {
        m_pendingGame.clear();
        m_launchReady = false;
        m_firmwareOfferedFor.clear();
        m_menuDirty = true;
    }
    if (m_session.IsRunning() && ConfirmStop(Tr("NXEmu - Stop Emulation")))
    {
        m_session.Stop();
    }
}

bool MainWindow::ConfirmStop(const std::string & title)
{
    if (!uiSettings.confirmBeforeStopping)
    {
        return true;
    }
    const std::string message = m_session.Modules().OperatingSystem().GetExitLocked()
                                    ? Tr("The currently running application has requested NXEmu to not exit.\n\nWould you like to bypass this and stop emulation anyway?")
                                    : Tr("Are you sure you want to stop the emulation?\n\nAny unsaved progress will be lost.");
    return g_notify->Query(message.c_str(), title.c_str()) == NotificationResponse::Yes;
}
