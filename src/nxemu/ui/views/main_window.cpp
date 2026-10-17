#include "main_window.h"
#include <nxemu/ui/i18n/translation.h>
#include <nxemu/ui/library/firmware_avatars.h>
#include "resolution_scales.h"
#include "scaling_filters.h"
#include <nxemu/ui/library/game_cache.h>
#include "rml_events.h"
#include <RmlUi/Core.h>
#include <algorithm>
#include <cmath>
#include <common/path.h>
#include <nxemu-core/notification.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/version.h>
#include <nxemu-module-spec/operating_system.h>
#include <nxemu-module-spec/video.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu-video/video_settings_identifiers.h>
#include <nxemu/ui/host/shell.h>
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/ui/library/ui_preferences.h>
#include <nxemu/ui/platform/file_dialogs.h>
#include <yuzu_common/fs/filesystem_interfaces.h>
#include <yuzu_common/fs/fs_util.h>
#include <yuzu_common/fs/path_util.h>

namespace
{
constexpr const char * kDiscordUrl = "https://discord.gg/hEa4hNyFWU";
constexpr const char * kReportUrl = "https://report.nxemu.com/";
constexpr uint64_t kStatusIntervalMs = 1000;
constexpr float kScanStepShare = 0.85f;  // of a game's step, shown while it is read
constexpr float kScanEaseSeconds = 0.7f; // time for the bar to cover 63% of what is left

#ifdef __APPLE__
constexpr bool kShowEmulationControlsDefault = true;
#else
constexpr bool kShowEmulationControlsDefault = false;
#endif

bool ShowEmulationControls()
{
    return UiPreferences::GetBool(UiPreferences::ShowEmulationControls, kShowEmulationControlsDefault);
}


// The orders the games can be shown in; the preference stores the index.
const char * const kGameSorts[] = {TrMark("Name"), TrMark("Publisher"), TrMark("Recent"), TrMark("Play time"), TrMark("Size")};
constexpr int kGameSortCount = static_cast<int>(sizeof(kGameSorts) / sizeof(kGameSorts[0]));
constexpr int kSortByPublisher = 1;
constexpr int kSortByRecentlyPlayed = 2;
constexpr int kSortByPlayTime = 3;
constexpr int kSortBySize = 4;

// The columns of the list the narrowing list gives up, first to last, and
// the width in dp under which each goes.
struct ListColumn
{
    const char * hiddenClass;
    float minWidth;
};
constexpr ListColumn kOptionalListColumns[] = {
    {"hide-size", 930.0f}, {"hide-last", 820.0f}, {"hide-publisher", 700.0f}, {"hide-format", 560.0f},
    {"hide-number", 480.0f}, {"hide-version", 410.0f}, {"hide-play-label", 400.0f},
};

// The side of a card's picture, in dp. The preference used to hold one of
// four steps, which read as the widths they stood for.
constexpr int kGameIconMin = 96;
constexpr int kGameIconMax = 240;
constexpr int kGameIconDefault = 146;
constexpr int kGameIconSteps[] = {108, 146, 176, 216};
constexpr int kGameIconStepCount = static_cast<int>(sizeof(kGameIconSteps) / sizeof(kGameIconSteps[0]));
// Below these widths of the window, in dp, the library's tools and the status
// bar drop what matters least.
constexpr float kNarrowWidth = 800.0f;
constexpr float kCompactWidth = 560.0f;
constexpr uint64_t kVersionCopiedMs = 1500;
constexpr int kCardGap = 16;                   // between cards, and 10 between the smallest: see the style sheet
constexpr int kSmallCardGap = 10;
constexpr size_t kUnmeasuredRows = 3;          // rows of games shown before the height of one is known
constexpr size_t kIconsPerFrame = 2;           // pictures read from their files in one frame, each taking a millisecond or two
constexpr size_t kMaxLoadedIcons = 256;        // pictures kept as textures, unless more are on screen at once
constexpr size_t kMinMarginRows = 2;           // rows kept ready beyond those in sight, each way
constexpr uint64_t kGameHintMs = 4000;         // how long what hides the interface says how to get it back
constexpr int kCoverTitleDelayMs = 1200;       // a cover names its game to a mouse that stays
// The ranges of the menus.
constexpr int kFilterSharpnessRange = 1;
constexpr int kPerformanceSizeRange = 2;
constexpr int kPerformanceRateRange = 3;
constexpr int kPerformanceLengthRange = 4;
constexpr int kCardBorder = 2;                 // .rom-card's, on each side
constexpr int kScrollbarWidth = 12;            // a vertical scrollbar and its margin: see base.rcss
constexpr uint64_t kIconSizeSaveDelayMs = 400; // the slider must rest that long before its value is written

int GameIconSize()
{
    const int size = UiPreferences::GetInt(UiPreferences::GameIconSize, kGameIconDefault);
    return size >= 1 && size <= kGameIconStepCount ? kGameIconSteps[size - 1] : std::clamp(size, kGameIconMin, kGameIconMax);
}

// The cards' text and spacing follow the size by steps: a face of the font
// is made for every size the text is drawn at.
int GameIconStep(int size)
{
    int step = 1;
    while (step < kGameIconStepCount && size >= (kGameIconSteps[step - 1] + kGameIconSteps[step]) / 2)
    {
        step++;
    }
    return step;
}

int GameSort()
{
    return std::clamp(UiPreferences::GetInt(UiPreferences::GameSort, 0), 0, kGameSortCount - 1);
}

std::string Folded(const std::string & text)
{
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return result;
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

std::string FileName(const std::string & utf8Path)
{
    return Common::FS::PathToUTF8String(std::filesystem::path{Common::FS::ToU8String(utf8Path)}.filename());
}

// The file's extension, as the kind of file the game is.
std::string FileFormat(const std::string & utf8Path)
{
    std::string extension = Common::FS::PathToUTF8String(std::filesystem::path{Common::FS::ToU8String(utf8Path)}.extension());
    extension.erase(0, extension.find_first_not_of('.'));
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    return extension;
}

std::string Format(const char * format, double value);

std::string FileSize(uint64_t bytes)
{
    if (bytes == 0)
    {
        return {};
    }
    constexpr double kMegabyte = 1024.0 * 1024.0;
    constexpr double kGigabyte = kMegabyte * 1024.0;
    return bytes >= kGigabyte ? Tr("%1 GB", {Format("%.2f", bytes / kGigabyte)}) : Tr("%1 MB", {Format("%.0f", bytes / kMegabyte)});
}

std::string ProgramIdText(uint64_t programId)
{
    char text[17];
    std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(programId));
    return programId != 0 ? text : std::string();
}

std::string Format(const char * format, double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), format, value);
    return buffer;
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
    m_session.SetListener(nullptr);
    SaveIconSize();
    m_library.StopScan();
    // The interface is drawn with the video module, which goes away next.
    m_window.DetachVideo();
}

bool MainWindow::Create()
{
    m_interfaceLanguage = UiPreferences::InterfaceLanguageCode();
    m_document = Translation::LoadDocument(*m_window.Context(), m_assetDir / "main_window.rml");
    if (m_document == nullptr)
    {
        return false;
    }
    m_menuBar = std::make_unique<MenuBar>(m_document->GetElementById("menu-bar"), [this](int id) { Post(static_cast<Action>(id)); }, [this](int id, int value) { SetMenuRange(id, value); });
    m_gameMenu = std::make_unique<PopupMenu>(
        m_document->GetElementById("popup-menu"), [this](int id) { Post(static_cast<Action>(id)); }, [this](int id, int value) { SetMenuRange(id, value); });
    m_tooltip = std::make_unique<Tooltip>(m_window.Context(), m_assetDir);
    m_aboutDialog = std::make_unique<AboutDialog>(m_window.Context(), m_assetDir);
    const OpenSettingsPage openPage = [this](SettingsPage page) { return ShowSettingsPage(page); };
    m_generalDialog = std::make_unique<GeneralDialog>(m_window, m_session, m_settingsContext, m_assetDir, GeneralDialog::Host{[this] { UpdateStatusWidgets(); }, openPage});
    m_advancedDialog = std::make_unique<AdvancedDialog>(m_window, m_session, m_settingsContext, m_assetDir, openPage);
    m_controllerDialog = std::make_unique<ControllerDialog>(m_window, m_session, m_assetDir);
    m_directoriesDialog = std::make_unique<DirectoriesDialog>(m_window, m_library, m_assetDir,
                                                        DirectoriesDialog::Host{
                                                            [this] { return !m_session.IsRunning() && m_pendingGame.empty(); },
                                                            [this] {
                                                                m_library.StartScan(m_session.Modules());
                                                                RenderGames();
                                                            },
                                                            openPage,
                                                        });
    m_profilesDialog = std::make_unique<ProfilesDialog>(m_window, m_session, m_assetDir,
                                                        ProfilesDialog::Host{
                                                            [this] { return !m_session.IsRunning() && m_pendingGame.empty(); },
                                                            [this] { RefreshProfiles(); },
                                                            openPage,
                                                        });
    m_firmwareDialog = std::make_unique<FirmwareDialog>(m_window, m_session.Modules().Systemloader(), m_assetDir,
                                                        FirmwareDialog::Host{
                                                            [this] {
                                                                if (m_library.StopScan())
                                                                {
                                                                    m_rescanGames = true;
                                                                }
                                                            },
                                                            [this] {
                                                                // Its pictures for the profiles are other ones.
                                                                FirmwareAvatars::Forget();
                                                                RefreshFirmwareVersion();
                                                            },
                                                        });
    m_loadingScreen = std::make_unique<LoadingScreen>(m_document->GetElementById("loading-panel"), m_window.Renderer());
    m_detailsPanel = std::make_unique<GameDetailsPanel>(m_document->GetElementById("games-details"), [this](size_t index) {
        Post(static_cast<Action>(static_cast<int>(Action::AddOnFirst) + static_cast<int>(index)));
    }, [this](size_t index) {
        if (m_details.path == m_selectedPath && m_details.programId != 0 && index < m_details.overrides.size())
        {
            m_pendingOverrides.push_back({m_details.programId, m_details.overrides[index]});
        }
    });
    BindEvents();

    m_document->SetClass(m_window.DrawsWindowControls() ? "window-controls" : "system-controls", true);
    if (m_window.SystemTitleBarHeight() > 0)
    {
        // Window coordinates are dp wherever the system draws the bar.
        // Leave a little breathing room below the native window buttons.
        m_document->GetElementById("title-bar")->SetProperty(Rml::PropertyId::Height, Rml::Property(static_cast<float>(m_window.SystemTitleBarHeight() + 6), Rml::Unit::DP));
    }
    SetTitle({});
    m_iconSize = GameIconSize();
    ApplyLibraryOptions();
    ShowPanel(Panel::RomBrowser);
    // The scan starts before the games are shown: with none known yet, the
    // list waits for it rather than say there are none.
    std::vector<GameEntry> cached = m_library.CachedGames();
    m_library.StartScan(m_session.Modules());
    ShowGames(std::move(cached));
    RefreshFirmwareVersion();
    RefreshProfiles();
    UpdateStatusText();
    // The keyboard goes to the search field only once it is clicked.
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
    // An installation stopped here leaves the installed firmware as it was.
    m_firmwareDialog->CancelInstall();
    if (!m_session.IsRunning() || ConfirmStop(Tr("NXEmu - Exit")))
    {
        m_quit = true;
    }
}

void MainWindow::HandleEvent(const SDL_Event & event)
{
    const auto chord = Hotkeys::TrackGamepad(event);
    if (m_advancedDialog->HandleGamepad(event, chord)) return;
    if (event.type == SDL_JOYBUTTONDOWN && chord.buttons != 0 && !DialogOpen() &&
        m_session.IsRunning() && (SDL_GetWindowFlags(m_window.Handle()) & SDL_WINDOW_INPUT_FOCUS) != 0)
    {
        for (const HotkeyAction & hotkey : kHotkeyActions)
        {
            if (Hotkeys::GetGamepad(hotkey.id) == chord &&
                (hotkey.action != Action::ExitFullscreen || m_window.IsFullscreen()))
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
            // What is dropped on the firmware dialog is a firmware; under
            // another dialog it is for nobody.
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
        // A shortcut being chosen takes the key from everything else.
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
        m_pendingGameMenu = m_pendingSelectMenu = nullptr;
        return;
    }
    // A dialog keeps the keyboard: neither shortcuts nor the game see it.
    if ((event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) && !DialogOpen())
    {
        for (const HotkeyAction & hotkey : kHotkeyActions)
        {
            if (!Hotkeys::Get(hotkey.id).Matches(event.key.keysym))
            {
                continue;
            }
            // Escape only leaves fullscreen; otherwise it is an ordinary key.
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
        // The press went to the window manager, not to the document.
        m_menuBar->Close();
        m_gameMenu->Close();
    }
    if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
    {
        // The cards move; the menu would stay behind.
        m_gameMenu->Close();
    }
    m_window.HandleEvent(event);
}

void MainWindow::Update()
{
    RunPendingWork();
    // Let background reads finish while the UI keeps processing input. Their
    // loader must remain alive until they return, but joining them here freezes
    // the whole window when an add-on container takes seconds to read.
    m_session.Update(!m_firmwareDialog->Installing() && !m_library.UsingModules());
    if (m_session.IsValid() && m_detailsAfterStop && !m_session.StoppedResourcesPending())
    {
        m_detailsAfterStop = false;
        RequestSelectedDetails();
    }
    if (!m_session.IsValid())
    {
        m_quit = true;
        return;
    }

    std::vector<GameEntry> games;
    if (m_library.TakeScanResult(games))
    {
        // An empty list was waiting for the scan to say whether it is.
        if (games != m_games || games.empty())
        {
            ShowGames(std::move(games));
        }
    }
    GameDetails details;
    if (m_library.TakeDetails(details) && details.path == m_selectedPath)
    {
        // Switching an update changes the version the card shows too.
        const auto game = std::find_if(m_games.begin(), m_games.end(), [this](const GameEntry & entry) { return entry.path == m_selectedPath; });
        if (game != m_games.end() && details.programId != 0 && game->version != details.version)
        {
            game->version = details.version;
            m_gamesDirty = true;
        }
        m_details = std::move(details);
        m_detailsDirty = true;
    }
    if (m_gamesDirty)
    {
        RenderGames();
    }
    RenderVisibleGames();
    LoadPendingIcons();
    if (m_settingsContext.TakeChanges()) m_detailsDirty = true;
    if (m_detailsDirty)
    {
        RenderDetails();
    }
    UpdateScanIndicator();
    SyncPhase();
    m_playTime.SetPlaying(m_phase == EmulatorSession::Phase::Running ? m_playingProgram : 0);
    if (m_rescanGames && m_phase == EmulatorSession::Phase::Idle && m_pendingGame.empty() && !m_library.Scanning() &&
        !m_firmwareDialog->Installing())
    {
        m_rescanGames = false;
        m_library.StartScan(m_session.Modules());
        if (m_games.empty())
        {
            RenderGames();
        }
    }
    SyncWindowState();
    UpdateMouseCursor();
    if (m_hotkeyRevision != Hotkeys::Revision())
    {
        m_hotkeyRevision = Hotkeys::Revision();
        m_menuDirty = true;
    }
    // An open menu is rebuilt once closed: one of its boxes may just have been ticked.
    if (m_menuDirty && !m_menuBar->IsOpen())
    {
        m_menuDirty = false;
        RebuildMenu();
    }
    const Rml::Context * context = m_document->GetContext();
    const float width = static_cast<float>(context->GetDimensions().x) / context->GetDensityIndependentPixelRatio();
    m_document->SetClass("narrow", width < kNarrowWidth);
    m_document->SetClass("compact", width < kCompactWidth);
    // Use logical pixels and the actual translated menu width. Windows/Linux
    // reserve their window buttons on the right; macOS reserves them on the left.
    const float density = context->GetDensityIndependentPixelRatio();
    const float menuWidth = m_document->GetElementById("menu-bar")->GetOffsetWidth() / density;
    const float windowSpace = m_window.DrawsWindowControls() ? 150.0f : 82.0f;
    const float titleSpace = width - menuWidth - windowSpace;
    m_document->SetClass("title-hide-settings", titleSpace < 720.0f);
    m_document->SetClass("title-hide-text", titleSpace < 520.0f);
    m_document->SetClass("title-hide-secondary", titleSpace < 400.0f);
    m_document->SetClass("title-minimal", titleSpace < 260.0f);
    LayoutList();
    if (m_fitWindow)
    {
        m_fitWindow = false;
        FitWindowToCards();
    }
    const uint64_t now = SDL_GetTicks64();
    if (m_pendingIconSize != 0 && m_pendingIconSize != m_iconSize)
    {
        m_iconSize = m_pendingIconSize;
        m_iconSizeChanged = now;
        ApplyIconSize();
    }
    m_pendingIconSize = 0;
    if (m_iconSizeChanged != 0 && now - m_iconSizeChanged >= kIconSizeSaveDelayMs)
    {
        SaveIconSize();
    }
    // The game is measured at the pace of the overlay while it is shown.
    if (now - m_lastMeasure >= (m_performance->Shown() ? m_performance->Interval() : kStatusIntervalMs))
    {
        m_lastMeasure = now;
        MeasureGame();
    }
    if (now - m_lastStatusUpdate >= kStatusIntervalMs)
    {
        m_lastStatusUpdate = now;
        UpdateStatusWidgets();
        UpdateStatusText();
    }

    m_aboutDialog->Update();
    m_firmwareDialog->Update();
    m_controllerDialog->Update();
    m_generalDialog->Update();
    m_advancedDialog->Update();
    m_directoriesDialog->Update();
    m_profilesDialog->Update();
    SyncInterfaceLanguage();
    // An open menu explains its own choices, and nothing else is explained.
    m_tooltip->Update(m_menuBar->IsOpen() || (m_gameMenu->IsOpen() && !m_gameMenu->Hovered()));
    m_window.Context()->Update();
    LayoutGameArea();
}

bool MainWindow::Still() const
{
    // The game presents independently and reuses the latest interface image.
    // Running or paused does not itself require redrawing the interface at
    // the monitor's refresh rate; animations and input still wake it normally.
    return m_phase != EmulatorSession::Phase::Loading && m_pendingGame.empty() && !m_library.Scanning() && !m_iconsPending &&
           m_iconSizeChanged == 0 && !m_firmwareDialog->Installing() && !m_controllerDialog->Listening() && !m_profilesDialog->Busy() &&
           m_window.Context()->GetNextUpdateDelay() > 0;
}

bool MainWindow::DialogOpen() const
{
    return m_aboutDialog->IsVisible() || m_firmwareDialog->IsVisible() || m_controllerDialog->IsVisible() || m_generalDialog->IsVisible() || m_advancedDialog->IsVisible() ||
           m_directoriesDialog->IsVisible() ||
           m_profilesDialog->IsVisible();
}

void MainWindow::RefreshFirmwareVersion()
{
    char firmware[32]{};
    const uint32_t length = m_session.Modules().Systemloader().GetInstalledFirmwareDisplayVersion(firmware, sizeof(firmware));
    m_firmwareVersion.assign(firmware, length);
    m_lastStatusUpdate = 0;
}

void MainWindow::OnModulesReleasing()
{
    // Only a scan cut short is owed again: running a game changes no library.
    if (m_library.StopScan())
    {
        m_rescanGames = true;
    }
    // The controllers being set up go with the modules.
    m_controllerDialog->Abandon();
    m_profilesDialog->Abandon();
    m_window.DetachVideo();
}

void MainWindow::OnModulesReady()
{
    m_firmwareDialog->SetLoader(m_session.Modules().Systemloader());
    AttachVideo();
}

void MainWindow::OnGameStarted(const std::string & path)
{
    const auto game = std::find_if(m_games.begin(), m_games.end(), [&path](const GameEntry & entry) { return entry.path == path; });
    const bool hasIcon = game != m_games.end() && !game->icon.empty();
    m_document->SetClass("has-game-icon", hasIcon);
    if (Rml::Element * icon = m_document->GetElementById("title-game-icon"))
    {
        if (hasIcon) icon->SetAttribute("src", UiRenderer::FileImageSource(game->icon));
        else icon->RemoveAttribute("src");
    }
    m_library.AddRecentFile(path);
    m_gamesDirty = true; // the order may follow what was played
    SetTitle(game != m_games.end() ? game->title : SettingsStore::GetInstance().GetString(NXCoreSetting::GameName));
    m_applyStartOptions = true;
    m_menuDirty = true;
}

void MainWindow::OnSessionError(const std::string & message)
{
    g_notify->DisplayError(message.c_str(), "NXEmu");
}

void MainWindow::Post(Action action)
{
    // Actions arrive from inside RmlUi's event dispatch. They run on the next
    // Update(), when changing the document cannot invalidate that dispatch.
    m_pendingActions.push_back(action);
}

void MainWindow::RunPendingWork()
{
    std::vector<OverrideAction> overrides;
    overrides.swap(m_pendingOverrides);
    for (const auto & action : overrides)
    {
        std::string error;
        auto & settings = GameSettings::GetInstance();
        const bool saved = action.setting.user ? settings.Inherit(action.programId, action.setting.key.c_str(), error) :
            settings.SetDatabaseEnabled(action.programId, action.setting.key.c_str(), !action.setting.enabled, error);
        if (!saved) OnSessionError(error);
        m_detailsDirty = true;
    }
    std::vector<Action> actions;
    actions.swap(m_pendingActions);
    for (const Action action : actions)
    {
        Run(action);
    }
    // A status bar menu left open by a choice shows what it changed.
    if (!actions.empty() && m_gameMenu->IsOpen() && m_gameMenu->Origin() != nullptr && m_pendingSelectMenu == nullptr &&
        (m_gameMenu->Origin()->IsClassSet("select") || m_gameMenu->Origin()->GetId() == "volume"))
    {
        m_pendingSelectMenu = m_gameMenu->Origin();
    }
    if (m_pendingGameMenu != nullptr)
    {
        Rml::Element * button = m_pendingGameMenu;
        m_pendingGameMenu = nullptr;
        ShowGameMenu(button);
    }
    if (m_pendingSelectMenu != nullptr)
    {
        Rml::Element * origin = m_pendingSelectMenu;
        m_pendingSelectMenu = nullptr;
        ShowSelectMenu(origin);
    }
    if (!m_pendingGame.empty() && !m_firmwareDialog->IsVisible())
    {
        if (!m_launchReady && m_firmwareOfferedFor != m_pendingGame && !m_session.IsRunning())
        {
            // A game card carries the firmware its game needs: offered once,
            // and the game starts when the dialog closes.
            m_firmwareOfferedFor = m_pendingGame;
            char version[64]{};
            if (m_session.Modules().Systemloader().GetGameCardFirmwareUpdate(m_pendingGame.c_str(), version, sizeof(version)) != 0)
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
                // Render the loading panel once before the synchronous module setup.
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
    SettingsStore & settings = SettingsStore::GetInstance();
    switch (action)
    {
    case Action::LoadFile: LoadFile(); break;
    case Action::Exit: RequestQuit(); break;
    case Action::OpenAppDirectory: OpenDirectory(Common::FS::GetYuzuPathString(Common::FS::YuzuPath::YuzuDir)); break;
    case Action::OpenLogDirectory: OpenDirectory(Common::FS::GetYuzuPathString(Common::FS::YuzuPath::LogDir)); break;
    case Action::PauseOrContinue: m_session.TogglePause(); break;
    case Action::Stop: StopGame(); break;
    case Action::Restart:
        if (m_session.IsRunning())
        {
            // Copy before stopping: module teardown clears the running game's settings.
            const std::string path = settings.GetString(NXCoreSetting::GameFile);
            if (!path.empty() && ConfirmStop(Tr("Restart")))
            {
                m_session.Stop();
                OpenGame(path);
            }
        }
        break;
    case Action::ToggleEmulationControls:
        UiPreferences::SetBool(UiPreferences::ShowEmulationControls, !ShowEmulationControls());
        SyncPhase();
        break;
    case Action::ToggleSpeedLimit:
        settings.SetBool(NXOsSetting::UseSpeedLimit, !settings.GetBool(NXOsSetting::UseSpeedLimit));
        m_session.FlushSettings();
        break;
    case Action::ToggleStartGamesInFullscreen:
        UiPreferences::SetBool(UiPreferences::StartGamesInFullscreen, !UiPreferences::GetBool(UiPreferences::StartGamesInFullscreen, false));
        break;
    case Action::ToggleStartGamesWithUiHidden:
        UiPreferences::SetBool(UiPreferences::StartGamesWithUiHidden, !UiPreferences::GetBool(UiPreferences::StartGamesWithUiHidden, false));
        break;
    case Action::ToggleFullscreen:
        if (m_session.IsRunning() || m_window.IsFullscreen())
        {
            m_window.SetFullscreen(!m_window.IsFullscreen());
        }
        break;
    case Action::ExitFullscreen: m_window.SetFullscreen(false); break;
    case Action::ToggleHideUi:
        if (m_session.IsRunning())
        {
            m_hideUi = !m_hideUi;
        }
        break;
    case Action::TogglePerformance:
        m_showPerformance = !m_showPerformance;
        UiPreferences::SetBool(UiPreferences::ShowPerformance, m_showPerformance);
        break;
    case Action::TogglePerformanceHistory:
    case Action::TogglePerformanceRange:
    case Action::TogglePerformanceFrameTime:
    case Action::TogglePerformanceLongest:
    case Action::TogglePerformanceSpeed:
    case Action::TogglePerformanceProcessor:
    case Action::TogglePerformanceMemory:
        m_performance->Toggle(static_cast<PerformanceItem>(static_cast<int>(action) - static_cast<int>(Action::TogglePerformanceHistory)));
        break;
    case Action::ToggleDockedMode: SetDockedMode(!IsDocked()); break;
    case Action::SetDockedMode: SetDockedMode(true); break;
    case Action::SetHandheldMode: SetDockedMode(false); break;
    case Action::ToggleGameDetails:
        UiPreferences::SetBool(UiPreferences::ShowGameDetails, !UiPreferences::GetBool(UiPreferences::ShowGameDetails, false));
        ApplyLibraryOptions();
        RequestSelectedDetails();
        break;
    case Action::PlaySelectedGame:
        if (SelectedGame() != nullptr)
        {
            OpenGame(m_selectedPath);
        }
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
                // Mods go in a directory that is only made once it is wanted.
                std::error_code error;
                std::filesystem::create_directories(std::filesystem::path{Common::FS::ToU8String(directory)}, error);
                OpenDirectory(directory);
            }
        }
        break;
    case Action::ToggleCardInformation:
        UiPreferences::SetBool(UiPreferences::ShowCardInformation, !UiPreferences::GetBool(UiPreferences::ShowCardInformation, true));
        ApplyLibraryOptions();
        break;
    case Action::ShowGridView:
    case Action::ShowListView:
        if (UiPreferences::GetBool(UiPreferences::GameListView, false) != (action == Action::ShowListView))
        {
            UiPreferences::SetBool(UiPreferences::GameListView, action == Action::ShowListView);
            ApplyLibraryOptions();
            m_gamesDirty = true;
        }
        break;
    case Action::TogglePlayTime:
        UiPreferences::SetBool(UiPreferences::ShowPlayTime, !UiPreferences::GetBool(UiPreferences::ShowPlayTime, true));
        ApplyLibraryOptions();
        break;
    case Action::ToggleCardTitle:
        UiPreferences::SetBool(UiPreferences::ShowCardTitle, !UiPreferences::GetBool(UiPreferences::ShowCardTitle, true));
        ApplyLibraryOptions();
        break;
    case Action::ResetWindowSize720p: ResetWindowSize(1280, 720); break;
    case Action::ResetWindowSize900p: ResetWindowSize(1600, 900); break;
    case Action::ResetWindowSize1080p: ResetWindowSize(1920, 1080); break;
    case Action::OpenSettings:
        m_settingsContext.Global();
        ShowSettingsPage(SettingsPage::General);
        break;
    case Action::ConfigureSelectedGame:
        if (const GameEntry * game = SelectedGame(); game != nullptr && game->programId != 0)
        {
            m_settingsContext.Game(game->programId, game->title);
            ShowSettingsPage(SettingsPage::General);
        }
        break;
    case Action::OpenProfiles: m_settingsContext.Global(); ShowSettingsPage(SettingsPage::Profiles); break;
    case Action::OpenGameDirectories: m_settingsContext.Global(); ShowSettingsPage(SettingsPage::Directories); break;
    case Action::OpenDiscord: OpenUrl(kDiscordUrl); break;
    case Action::OpenReportIssues: OpenUrl(kReportUrl); break;
    case Action::OpenAbout: m_aboutDialog->Show(); break;
    case Action::CopyVersion:
        if (SDL_SetClipboardText(VER_FILE_VERSION_STR) == 0)
        {
            m_versionCopiedUntil = SDL_GetTicks64() + kVersionCopiedMs;
            ShowVersion();
        }
        break;
    case Action::ConfigureControllers:
        if (m_session.IsValid())
        {
            m_controllerDialog->Show();
        }
        break;
    case Action::InstallFirmware:
        if (!m_session.IsRunning() && m_pendingGame.empty())
        {
            m_firmwareDialog->Show();
        }
        break;
    case Action::StartMenuGame: OpenGame(m_menuGame); break;
    case Action::OpenMenuGameDirectory:
        OpenDirectory(Common::FS::PathToUTF8String(std::filesystem::path{Common::FS::ToU8String(m_menuGame)}.parent_path()));
        break;
    case Action::CopyMenuGamePath: SDL_SetClipboardText(m_menuGame.c_str()); break;
    case Action::ConfigureMenuGame:
    case Action::OpenMenuGameSaves:
    case Action::OpenMenuGameCache:
    case Action::ClearMenuGameCache:
    {
        const auto game = std::find_if(m_games.begin(), m_games.end(), [this](const GameEntry & entry) { return entry.path == m_menuGame; });
        if (game == m_games.end() || game->programId == 0) break;
        if (action == Action::ConfigureMenuGame)
        {
            m_settingsContext.Game(game->programId, game->title);
            ShowSettingsPage(SettingsPage::General);
        }
        else if (action == Action::OpenMenuGameSaves)
        {
            const std::string directory = GameDetailsReader::SaveDirectory(game->programId);
            if (!directory.empty()) OpenDirectory(directory);
        }
        else
        {
            const auto root = Common::FS::GetYuzuPath(Common::FS::YuzuPath::ShaderDir);
            if (action == Action::OpenMenuGameCache)
                OpenDirectory(Common::FS::PathToUTF8String(GameCache::Directory(root, game->programId)));
            else if (!m_session.IsRunning())
            {
                const auto message = Tr("Clear the shader cache for %1?\n\nShaders will be rebuilt the next time you play.", {game->title});
                const auto title = Tr("Clear cache");
                if (g_notify->Query(message.c_str(), title.c_str()) == NotificationResponse::Yes)
                {
                    std::string error;
                    if (!GameCache::Clear(root, game->programId, error))
                    {
                        const auto failure = Tr("Could not clear the cache: %1", {error});
                        g_notify->DisplayError(failure.c_str(), title.c_str());
                    }
                    RequestSelectedDetails();
                }
            }
        }
        break;
    }
    case Action::ResetVolumeOverrides:
        UiPreferences::ResetStatusOverride(NXOsSetting::AudioVolume);
        UiPreferences::ResetStatusOverride(NXOsSetting::AudioMuted);
        break;
    default:
    {
        const int scale = static_cast<int>(action) - static_cast<int>(Action::ResolutionScaleFirst);
        if (scale >= 0 && scale < kResolutionScaleCount)
        {
            if (!m_session.IsRunning())
            {
                UiPreferences::SetStatusInt(NXVideoSetting::ResolutionSetup, scale);
                m_session.FlushSettings();
            }
            break;
        }
        const int profile = static_cast<int>(action) - static_cast<int>(Action::ProfileFirst);
        if (profile >= 0 && profile < HOST_PROFILE_MAX_USERS)
        {
            // A game keeps the player it was started as.
            if (!m_session.IsRunning() && profile < static_cast<int>(m_profiles.size()))
            {
                UiPreferences::SetStatusInt(NXOsSetting::CurrentUser, profile);
                m_session.FlushSettings();
                UpdateStatusWidgets();
            }
            break;
        }
        const int filter = static_cast<int>(action) - static_cast<int>(Action::ScalingFilterFirst);
        if (FindScalingFilter(filter) != nullptr)
        {
            UiPreferences::SetStatusInt(NXVideoSetting::ScalingFilter, filter);
            // The game's filter comes back with the game's sharpness.
            if (!UiPreferences::HasStatusOverride({NXVideoSetting::ScalingFilter}))
            {
                UiPreferences::ResetStatusOverride(NXVideoSetting::FSPSharpness);
            }
            m_session.FlushSettings();
            break;
        }
        const int sort = static_cast<int>(action) - static_cast<int>(Action::GameSortFirst);
        if (sort >= 0 && sort < kGameSortCount)
        {
            UiPreferences::SetInt(UiPreferences::GameSort, sort);
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
    m_menuDirty = true;
    UpdateStatusWidgets();
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
    onClick("card-information", Action::ToggleCardInformation);
    onClick("view-grid", Action::ShowGridView);
    onClick("view-list", Action::ShowListView);
    onClick("play-time", Action::TogglePlayTime);
    onClick("card-title", Action::ToggleCardTitle);
    const auto onButton = [this](const char * id, Action action) {
        Rml::Element * button = m_document->GetElementById(id);
        UiEvents::Listen(button, "click", [this, button, action](Rml::Event &) {
            if (!button->IsClassSet("locked"))
            {
                Post(action);
            }
        });
    };
    onButton("details-configure", Action::ConfigureSelectedGame);
    onButton("details-play", Action::PlaySelectedGame);
    onButton("details-save", Action::OpenSelectedSaveDirectory);
    onButton("details-cache", Action::OpenSelectedCacheDirectory);
    onButton("details-mods", Action::OpenSelectedModDirectory);
    UiEvents::Listen(m_document->GetElementById("games-search"), "change", [this](Rml::Event & event) {
        m_gameSearch = event.GetParameter<Rml::String>("value", "");
        m_gamesDirty = true;
    });
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
    onClick("title-settings", Action::OpenSettings);
    onClick("title-controllers", Action::ConfigureControllers);
    onClick("title-restart", Action::Restart);
    onClick("title-logo", Action::CopyVersion);
    onClick("title-name", Action::CopyVersion);
    onClick("title-version", Action::CopyVersion);
    onClick("title-pause", Action::PauseOrContinue);
    onClick("title-stop", Action::Stop);
    onClick("window-close", Action::Exit);
    UiEvents::Listen(m_document->GetElementById("window-minimise"), "click", [this](Rml::Event &) { m_window.Minimise(); });
    UiEvents::Listen(m_document->GetElementById("window-maximise"), "click", [this](Rml::Event &) { m_window.SetMaximised(!m_window.IsMaximised()); });

    Rml::Element * titleBar = m_document->GetElementById("title-bar");
    UiEvents::Listen(titleBar, "dblclick", [this, titleBar](Rml::Event & event) {
        // Only the bar itself, as for moving the window: not its menus and buttons.
        for (Rml::Element * element = event.GetTargetElement(); element != nullptr && element != titleBar; element = element->GetParentNode())
        {
            if (element->HasAttribute("data-window"))
            {
                return;
            }
        }
        m_window.TitleBarDoubleClicked();
    });

    Rml::Element * grid = m_document->GetElementById("games-grid");
    UiEvents::Listen(grid, "click", [this](Rml::Event & event) {
        if (UiEvents::ClosestWithClass(event.GetTargetElement(), "open-settings") != nullptr)
        {
            Post(Action::OpenGameDirectories);
            return;
        }
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
    // The right button opens a game's actions where it was pressed.
    UiEvents::Listen(grid, "mousedown", [this](Rml::Event & event) {
        Rml::Element * game = UiEvents::ClosestWithClass(event.GetTargetElement(), "rom-game");
        if (game != nullptr && event.GetParameter<int>("button", 0) == 1)
        {
            SelectGame(game);
            m_pendingGameMenu = game;
            m_gameMenuPoint = Rml::Vector2f(event.GetParameter<float>("mouse_x", 0.0f), event.GetParameter<float>("mouse_y", 0.0f));
        }
    });
    // A heading orders the games by its column.
    UiEvents::Listen(m_document->GetElementById("games-list-header"), "click", [this](Rml::Event & event) {
        const int sort = event.GetTargetElement()->GetAttribute<int>("data-sort", -1);
        if (sort >= 0 && sort < kGameSortCount)
        {
            Post(static_cast<Action>(static_cast<int>(Action::GameSortFirst) + sort));
        }
    });
    UiEvents::Listen(grid, "dblclick", [this](Rml::Event & event) {
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

    m_performance = std::make_unique<PerformanceOverlay>(m_document->GetElementById("performance"));
    m_showPerformance = UiPreferences::GetBool(UiPreferences::ShowPerformance, false);
    m_volume = std::make_unique<VolumeControl>(m_document->GetElementById("volume"), [this] { UpdateStatusWidgets(); });
    UiEvents::Listen(m_document->GetElementById("volume"), "mousedown", [this](Rml::Event & event) {
        if (event.GetParameter<int>("button", -1) == 1)
        {
            m_pendingSelectMenu = m_document->GetElementById("volume");
            event.StopPropagation();
        }
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
            recent.push_back(MenuItem::Action(static_cast<int>(Action::RecentFileFirst) + static_cast<int>(i),
                                              std::to_string(i + 1) + " " + FileName(m_library.RecentFiles()[i])));
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
    file.push_back(action(Action::ConfigureControllers, Tr("Controllers"), m_session.IsValid()));
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

    emulation.push_back(check(Action::ToggleEmulationControls, Tr("Show emulation controls"), ShowEmulationControls()));
    emulation.push_back(MenuItem::Separator());
    MenuItem speedLimit = check(Action::ToggleSpeedLimit, Tr("Limit &Speed"), SettingsStore::GetInstance().GetBool(NXOsSetting::UseSpeedLimit));
    speedLimit.enabled = playing;
    emulation.push_back(std::move(speedLimit));

    // Fullscreen and the hidden interface are for a game: without one, the
    // menu says how the next starts.
    emulation.push_back(MenuItem::Separator());
    emulation.push_back(action(Action::ToggleFullscreen, Tr("&Fullscreen"), running));
    emulation.push_back(action(Action::ToggleHideUi, m_hideUi ? Tr("Show &UI") : Tr("Hide &UI"), running));
    // The overlay over games, and what it says beside the pictures a second:
    // an action for each, in their order.
    static_assert(static_cast<int>(Action::TogglePerformanceMemory) - static_cast<int>(Action::TogglePerformanceHistory) + 1 == std::size(PerformanceOverlay::kItems));
    std::vector<MenuItem> performance;
    performance.push_back(check(Action::TogglePerformance, Tr("Show performance"), m_showPerformance));
    performance.push_back(MenuItem::Separator());
    for (const PerformanceOverlay::Item & item : PerformanceOverlay::kItems)
    {
        const auto id = static_cast<Action>(static_cast<int>(Action::TogglePerformanceHistory) + static_cast<int>(item.id));
        performance.push_back(check(id, Tr(item.label), PerformanceOverlay::Has(item.id)));
    }
    performance.push_back(MenuItem::Separator());
    performance.push_back(MenuItem::Slider(Tr("Size"), {kPerformanceSizeRange, PerformanceOverlay::kLargest, PerformanceOverlay::Size(), true, {}, PerformanceOverlay::kSmallest,
                                                        PerformanceOverlay::kSizeStep}));
    performance.push_back(MenuItem::Slider(Tr("Measures per second"), {kPerformanceRateRange, PerformanceOverlay::kFastest, PerformanceOverlay::Rate(), true, {},
                                                                       PerformanceOverlay::kSlowest, 1, ""}));
    performance.push_back(MenuItem::Slider(Tr("History length"), {kPerformanceLengthRange, PerformanceOverlay::kLongest, PerformanceOverlay::Length(), true,
                                                                  Tr("How many measures the history shows"), PerformanceOverlay::kShortest, 1, ""}));
    emulation.push_back(MenuItem::Submenu(Tr("Window size"), {
        action(Action::ResetWindowSize720p, "720p"),
        action(Action::ResetWindowSize900p, "900p"),
        action(Action::ResetWindowSize1080p, "1080p"),
    }));
    emulation.push_back(MenuItem::Separator());
    emulation.push_back(MenuItem::Submenu(Tr("Performance overlay"), std::move(performance)));
    emulation.push_back(MenuItem::Separator());
    emulation.push_back(check(Action::ToggleStartGamesInFullscreen, Tr("Start games in fullscreen"), UiPreferences::GetBool(UiPreferences::StartGamesInFullscreen, false)));
    emulation.push_back(check(Action::ToggleStartGamesWithUiHidden, Tr("Start games with UI hidden"), UiPreferences::GetBool(UiPreferences::StartGamesWithUiHidden, false)));
    menus.push_back(MenuItem::Submenu(Tr("&Emulation"), std::move(emulation)));

    menus.push_back(MenuItem::Submenu(Tr("&Help"), {
                                                   action(Action::OpenDiscord, Tr("&Discord")),
                                                   action(Action::OpenReportIssues, Tr("&Report Issues")),
                                                   MenuItem::Separator(),
                                                   action(Action::OpenAbout, Tr("&About NXEmu")),
                                               }));
    m_menuBar->SetMenus(menus);
}

void MainWindow::ShowGames(std::vector<GameEntry> games)
{
    m_games = std::move(games);
    RenderGames();
}

void MainWindow::RenderGames()
{
    m_gamesDirty = false;
    Rml::Element * grid = m_document->GetElementById("games-grid");
    if (grid == nullptr)
    {
        return;
    }
    // The cards the menu and the selection refer to are about to be replaced.
    m_gameMenu->Close();
    m_pendingGameMenu = nullptr;
    m_selectedGame = nullptr;
    m_detailsDirty = true;

    // The games the search keeps, in the chosen order. The library gives
    // them by title, which every order falls back on.
    const std::string search = Folded(m_gameSearch);
    std::vector<size_t> shown;
    for (size_t i = 0; i < m_games.size(); i++)
    {
        if (search.empty() || Folded(m_games[i].title).find(search) != std::string::npos ||
            Folded(m_games[i].publisher).find(search) != std::string::npos)
        {
            shown.push_back(i);
        }
    }
    const int sort = GameSort();
    if (sort == kSortByPublisher)
    {
        std::stable_sort(shown.begin(), shown.end(),
                         [this](size_t left, size_t right) { return Folded(m_games[left].publisher) < Folded(m_games[right].publisher); });
    }
    else if (sort == kSortByRecentlyPlayed)
    {
        const std::vector<std::string> & recent = m_library.RecentFiles();
        const auto rank = [this, &recent](size_t game) { return std::find(recent.begin(), recent.end(), m_games[game].path) - recent.begin(); };
        std::stable_sort(shown.begin(), shown.end(), [&rank](size_t left, size_t right) { return rank(left) < rank(right); });
        // The time a game was last played is known since play time is kept;
        // the recent files order the games played before that.
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
        // The scan may yet find games: its indicator is all there is to show.
        grid->SetInnerRML("");
        return;
    }
    if (m_shownGames.empty())
    {
        // A message, with the way out of it as a link to the settings.
        const auto message = [](const std::string & text, const std::string & link = {}) {
            return "<div class=\"games-empty\"><img class=\"empty-state-icon\" src=\"images/gamepad.png\" /><div>" + Rml::StringUtilities::EncodeRml(text) +
                   (link.empty() ? std::string() : " <span class=\"link open-settings\">" + Rml::StringUtilities::EncodeRml(link) + "</span>") + "</div></div>";
        };
        grid->SetInnerRML(!m_games.empty() ? message(Tr("No game matches your search."))
                          : m_library.Directories().empty() ? message(Tr("No game directory yet."), Tr("Add one in the settings"))
                                                            : message(Tr("No games found in your game directories."), Tr("Change them in the settings")));
        return;
    }
    RenderVisibleGames();
}

// Only the games in sight are elements of the document, with some rows
// around them for the scrolling to come: what a library costs to lay out and
// to draw does not grow with it. Empty blocks stand for the rows above and
// below, so that the scroll bar tells of them all.
void MainWindow::RenderVisibleGames()
{
    Rml::Element * grid = m_document->GetElementById("games-grid");
    if (grid == nullptr || m_shownGames.empty())
    {
        return;
    }
    const bool list = grid->IsClassSet("list");
    // A row is as high as the games shown make it.
    float rowHeight = m_gameRowHeight;
    if (Rml::Element * item = grid->QuerySelector(".rom-game"))
    {
        const float height = item->GetOffsetHeight() + item->GetBox().GetEdge(Rml::BoxArea::Margin, Rml::BoxEdge::Bottom);
        rowHeight = height > 0.0f ? height : rowHeight;
    }
    size_t columns = 1;
    if (!list)
    {
        columns = static_cast<size_t>(std::max(grid->GetClientWidth() / CardStep(), 1.0f));
    }
    const size_t rows = (m_shownGames.size() + columns - 1) / columns;
    // Until a row has been measured, a few are shown to measure one.
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
    // The elements the menu refers to may be about to go.
    m_gameMenu->Close();
    m_pendingGameMenu = nullptr;

    // Scrolling moves the rows in sight by a few: the rows that stay keep
    // their elements, and only those that come or go are made or dropped.
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

// A game's picture. One seen before is a texture already, and is shown at
// once; another has its file read and decoded first, which would hold the
// frame up if every picture of a row did it together: it is left for
// LoadPendingIcons() to show.
std::string MainWindow::IconRml(const std::string & icon)
{
    if (icon.empty())
    {
        return {};
    }
    if (const auto loaded = m_loadedIcons.find(icon); loaded != m_loadedIcons.end())
    {
        loaded->second = ++m_iconUse;
        return "<img src=\"" + Rml::StringUtilities::EncodeRml(UiRenderer::FileImageSource(icon)) + "\"/>";
    }
    m_iconsPending = true;
    return "<img data-icon=\"" + Rml::StringUtilities::EncodeRml(icon) + "\"/>";
}

// Shows a few of the pictures that are not textures yet, those in sight first.
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
        pending[i]->SetAttribute("src", UiRenderer::FileImageSource(icon));
        m_loadedIcons[icon] = ++m_iconUse;
    }
}

// Forgets the textures of the pictures shown longest ago, beyond a number of
// them: a library scrolled through would keep every picture otherwise.
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

// A game as a card of the grid.
std::string MainWindow::CardRml(size_t index)
{
    const GameEntry & game = m_games[index];
    std::string rml = std::string("<div class=\"rom-game rom-card") + (game.path == m_selectedPath ? " selected" : "") + "\" data-game=\"" + std::to_string(index) +
           "\"><div class=\"rom-art\" data-title-delay=\"" + std::to_string(kCoverTitleDelayMs) + "\" title=\"" + Rml::StringUtilities::EncodeRml(game.title) + "\">";
    rml += IconRml(game.icon);
    // The time played lies over the picture; absent until the game has been played.
    const std::string played = PlayTime::Duration(m_playTime.Of(game.programId).seconds);
    if (!played.empty())
    {
        rml += "<div class=\"rom-played\"><img src=\"images/clock.png\"/><div>" + Rml::StringUtilities::EncodeRml(played) + "</div></div>";
    }
    // Version and publisher share a line under a rule; either may be missing.
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

// Shows the library as its preferences say.
void MainWindow::ApplyLibraryOptions()
{
    const bool information = UiPreferences::GetBool(UiPreferences::ShowCardInformation, true);
    const bool playTime = UiPreferences::GetBool(UiPreferences::ShowPlayTime, true);
    const bool list = UiPreferences::GetBool(UiPreferences::GameListView, false);
    const bool title = UiPreferences::GetBool(UiPreferences::ShowCardTitle, true);
    const bool details = UiPreferences::GetBool(UiPreferences::ShowGameDetails, false);
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
    setClass("games-list-header", "shown", list);
    // The column the games are ordered by shows it in its heading.
    if (Rml::Element * header = m_document->GetElementById("games-list-header"))
    {
        for (int i = 0; i < header->GetNumChildren(); i++)
        {
            Rml::Element * heading = header->GetChild(i);
            heading->SetClass("sorted", heading->GetAttribute<int>("data-sort", -1) == GameSort());
        }
    }
    setClass("games-details-toggle", "active", details);
    setClass("rom-browser-panel", "details-open", details);
    SetText("games-sort-text", Tr(kGameSorts[GameSort()]));
}

// A game as a line of the list: what the card says, and what only the
// details panel says otherwise.
std::string MainWindow::ListRowRml(size_t index, size_t number)
{
    const GameEntry & game = m_games[index];
    const PlayTime::Record played = m_playTime.Of(game.programId);
    const std::string format = FileFormat(game.path);
    // A decrypted file is of the same kind as the one it was made from.
    const char * kind = format.find("XCI") != std::string::npos ? " xci" : format.find("NSP") != std::string::npos ? " nsp" : "";
    const auto cell = [](const char * name, const std::string & text) {
        return std::string("<div class=\"col-") + name + "\">" + Rml::StringUtilities::EncodeRml(text) + "</div>";
    };
    std::string rml = std::string("<div class=\"rom-game rom-row") + (game.path == m_selectedPath ? " selected" : "") + "\" data-game=\"" +
                      std::to_string(index) + "\">" + cell("number", std::to_string(number)) + "<div class=\"col-cover\">";
    rml += IconRml(game.icon);
    rml += "</div>" + cell("game", game.title) + cell("publisher", game.publisher) + cell("version", game.version.empty() ? std::string() : "v" + game.version) +
           cell("last", played.lastPlayed != 0 ? PlayTime::Ago(played.lastPlayed) : Tr("Never")) + cell("time", PlayTime::Duration(played.seconds)) +
           "<div class=\"col-format\">" + (format.empty() ? std::string() : "<div class=\"format-badge" + std::string(kind) + "\">" + Rml::StringUtilities::EncodeRml(format) + "</div>") +
           "</div>" + cell("size", FileSize(game.size)) +
           "<div class=\"col-actions\"><div class=\"row-play\"><img src=\"images/play.png\"/><div>" + Rml::StringUtilities::EncodeRml(Tr("Play")) + "</div></div>"
           "<div class=\"rom-more\" title=\"" + Rml::StringUtilities::EncodeRml(Tr("More actions")) + "\"><div class=\"rom-dot\"/><div class=\"rom-dot\"/><div class=\"rom-dot\"/></div></div></div>";
    return rml;
}

// Leaves the list the columns it has the room for, and keeps their headings
// clear of the scroll bar the lines may have.
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

// Gives the cards the size the slider says.
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

// Gives a card the size the slider says; a line of the list has its own.
void MainWindow::SizeCard(Rml::Element * card) const
{
    Rml::Element * art = card->IsClassSet("rom-card") ? card->GetFirstChild() : nullptr;
    if (art != nullptr)
    {
        card->SetProperty(Rml::PropertyId::Width, Rml::Property(static_cast<float>(m_iconSize + 2 * kCardBorder), Rml::Unit::DP));
        art->SetProperty(Rml::PropertyId::Height, Rml::Property(static_cast<float>(m_iconSize), Rml::Unit::DP));
    }
}

void MainWindow::SaveIconSize()
{
    if (m_iconSizeChanged != 0)
    {
        m_iconSizeChanged = 0;
        UiPreferences::SetInt(UiPreferences::GameIconSize, m_iconSize);
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
    const bool reading = m_library.ReadingGames();
    if (!reading)
    {
        indicator->SetClass("active", false);
        m_scanReadingSince = 0;
        m_scanIndicatorShown = 0.0f;
        return;
    }
    // A cache miss can finish within a few frames. Avoid flashing an overlay
    // for these short checks, while keeping progress for genuinely slow reads.
    if (m_scanReadingSince == 0) m_scanReadingSince = now;
    indicator->SetClass("active", now - m_scanReadingSince >= 250);
    // Each game is a step of the bar. The bar heads for most of the step of
    // the game being read, quickly at first and ever more slowly: it moves
    // while a large game is read, and completes the step when that is done.
    const GameLibrary::Progress progress = m_library.ScanProgress();
    const float target = progress.total != 0 ? (static_cast<float>(progress.started) - (1.0f - kScanStepShare)) / static_cast<float>(progress.total) : 0.0f;
    if (target > m_scanIndicatorShown)
    {
        m_scanIndicatorShown += (target - m_scanIndicatorShown) * (1.0f - std::exp(-elapsed / kScanEaseSeconds));
    }
    fill->SetProperty(Rml::PropertyId::Width, Rml::Property(m_scanIndicatorShown * 100.0f, Rml::Unit::PERCENT));
}

void MainWindow::SelectGame(Rml::Element * card)
{
    // The selected game may be out of sight, and so have no element.
    if (card == m_selectedGame && (card != nullptr || m_selectedPath.empty()))
    {
        return;
    }
    if (m_selectedGame != nullptr)
    {
        m_selectedGame->SetClass("selected", false);
    }
    m_selectedGame = card;
    m_selectedPath.clear();
    if (m_selectedGame != nullptr)
    {
        m_selectedGame->SetClass("selected", true);
        const size_t index = static_cast<size_t>(m_selectedGame->GetAttribute<int>("data-game", -1));
        if (index < m_games.size())
        {
            m_selectedPath = m_games[index].path;
        }
    }
    m_detailsDirty = true;
    RequestSelectedDetails();
}

const GameEntry * MainWindow::SelectedGame() const
{
    const auto game = std::find_if(m_games.begin(), m_games.end(), [this](const GameEntry & entry) { return entry.path == m_selectedPath; });
    return game != m_games.end() && !m_selectedPath.empty() ? &*game : nullptr;
}

// Has the selected game's file read for the details panel, if it is open and
// does not have it yet. The loader is busy with the game while one runs.
void MainWindow::RequestSelectedDetails()
{
    // Returning to the library must not start an expensive read on the loader
    // we are about to destroy. Refresh against the replacement modules instead.
    if (m_session.StoppedResourcesPending())
    {
        m_detailsAfterStop = true;
        return;
    }
    if (SelectedGame() != nullptr && m_details.path != m_selectedPath && !m_session.IsRunning() &&
        UiPreferences::GetBool(UiPreferences::ShowGameDetails, false))
    {
        m_library.RequestDetails(m_session.Modules(), m_selectedPath);
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
    m_detailsPanel->ShowGame(game->title, game->icon.empty() ? std::string() : UiRenderer::FileImageSource(game->icon),
                             {
                                 {Tr("Version"), game->version.empty() ? std::string() : "v" + game->version},
                                 {Tr("Publisher"), game->publisher},
                                 {Tr("Format"), FileFormat(game->path)},
                                 {Tr("Size"), FileSize(game->size)},
                                 {Tr("Last played"), PlayTime::Date(played.lastPlayed)},
                                 {Tr("Play time"), PlayTime::Duration(played.seconds)},
                                 {Tr("Title ID"), ProgramIdText(game->programId)},
                             });
    m_details.overrides.clear();
    for (const auto & entry : GameSettings::GetInstance().Entries(game->programId))
    {
        m_details.overrides.push_back({entry.key, entry.name, entry.value, entry.source == GameSettings::Source::User, entry.effective, entry.enabled});
    }
    m_detailsPanel->ShowDetails(m_details.path == m_selectedPath && m_details.programId != 0 ? &m_details : nullptr);
}

void MainWindow::ToggleAddOn(size_t index)
{
    if (m_details.path != m_selectedPath || index >= m_details.addOns.size() || m_session.IsRunning())
    {
        return;
    }
    // Shown at once as it will be; the loader's answer follows.
    const std::vector<std::string> disabled = GameDetailsReader::DisabledAfterToggle(m_details, index);
    for (GameAddOn & addOn : m_details.addOns)
    {
        addOn.enabled = std::find(disabled.begin(), disabled.end(), addOn.key) == disabled.end();
    }
    m_detailsDirty = true;
    m_library.RequestDetails(m_session.Modules(), m_selectedPath, m_details.programId, disabled);
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
    const auto id = m_games[index].programId;
    std::error_code error;
    const bool cacheExists = id != 0 && std::filesystem::is_directory(
        GameCache::Directory(Common::FS::GetYuzuPath(Common::FS::YuzuPath::ShaderDir), id), error);
    const std::vector<MenuItem> items = {
        MenuItem::Action(static_cast<int>(Action::StartMenuGame), Tr("Start Game")),
        MenuItem::Action(static_cast<int>(Action::ConfigureMenuGame), Tr("Game settings"), {}, id != 0),
        MenuItem::Separator(),
        MenuItem::Action(static_cast<int>(Action::OpenMenuGameSaves), Tr("Open saves"), {}, !GameDetailsReader::SaveDirectory(id).empty()),
        MenuItem::Submenu(Tr("Cache"), {
            MenuItem::Action(static_cast<int>(Action::OpenMenuGameCache), Tr("Open cache"), {}, cacheExists),
            MenuItem::Action(static_cast<int>(Action::ClearMenuGameCache), Tr("Clear cache"), {}, cacheExists && !m_session.IsRunning()),
        }),
        MenuItem::Action(static_cast<int>(Action::OpenMenuGameDirectory), Tr("Open Containing Folder")),
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

void MainWindow::SyncInterfaceLanguage()
{
    const std::string language = UiPreferences::InterfaceLanguageCode();
    if (language == m_interfaceLanguage) return;
    m_interfaceLanguage = language;
    Translation::UseLanguage(language, m_assetDir / "lang", std::filesystem::path(NXEMU_UI_SOURCE_ASSETS).parent_path() / "lang");

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
    if (m_phase == EmulatorSession::Phase::Loading) UpdateLoadingScreen();
}

void MainWindow::SyncPhase()
{
    const EmulatorSession::Phase phase = m_session.CurrentPhase();
    const bool emulating = m_session.IsRunning() &&
                           (phase == EmulatorSession::Phase::Running || phase == EmulatorSession::Phase::Paused);
    m_document->SetClass("emulating", emulating);
    const bool paused = emulating && phase == EmulatorSession::Phase::Paused;
    m_document->SetClass("game-paused", paused);
    const bool controlsVisible = ShowEmulationControls() && (m_session.IsRunning() || !m_pendingGame.empty());
    m_document->SetClass("game-controls-visible", controlsVisible);
    for (const char * id : {"title-pause", "title-restart"})
    {
        if (Rml::Element * button = m_document->GetElementById(id))
        {
            if (!emulating && !button->HasAttribute("disabled")) button->SetAttribute("disabled", "");
            else if (emulating && button->HasAttribute("disabled")) button->RemoveAttribute("disabled");
        }
    }
    if (Rml::Element * pause = m_document->GetElementById("title-pause"))
    {
        const bool paused = emulating && phase == EmulatorSession::Phase::Paused;
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
        m_lastStatusUpdate = 0;
        m_discord.SetEnabled(UiPreferences::GetBool(UiPreferences::EnableDiscordPresence, true));
        m_discord.Update(phase != EmulatorSession::Phase::Idle, SettingsStore::GetInstance().GetString(NXCoreSetting::GameName));
        if (phase == EmulatorSession::Phase::Idle)
        {
            // The cards and the details show how long the game was played.
            m_playingProgram = 0;
            m_gamesDirty = true;
            m_details = {};
            RequestSelectedDetails();
            m_hideUi = false;
            m_window.SetFullscreen(false);
            SetTitle({});
        }
        else if (phase == EmulatorSession::Phase::Running && m_applyStartOptions)
        {
            m_applyStartOptions = false;
            if (IRomInfoPtr info(m_session.Modules().Systemloader().LoadedRomInfo()); info)
            {
                info->ReadProgramId(m_playingProgram);
            }
            m_hideUi = UiPreferences::GetBool(UiPreferences::StartGamesWithUiHidden, false);
            if (UiPreferences::GetBool(UiPreferences::StartGamesInFullscreen, false))
            {
                m_window.SetFullscreen(true);
            }
            SetTitle(SettingsStore::GetInstance().GetString(NXCoreSetting::GameName));
        }
    }

    switch (m_phase)
    {
    case EmulatorSession::Phase::Idle: ShowPanel(Panel::RomBrowser); break;
    case EmulatorSession::Phase::Loading:
        if (m_gameShown)
        {
            // A game that has been on screen is not loading: it is stopping.
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
    m_performance->Show(m_showPerformance && m_gameShown && m_phase != EmulatorSession::Phase::Idle);
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
    // The content area turns transparent to reveal the game underneath.
    m_document->SetClass("video-visible", panel == Panel::Renderer || panel == Panel::Pause);
}

void MainWindow::SetUiHidden(bool hidden)
{
    m_document->SetClass("ui-hidden", hidden);
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
        if (Rml::Element * icon = m_document->GetElementById("title-game-icon")) icon->RemoveAttribute("src");
    }
    m_document->SetClass("has-game", !game.empty());
    ShowVersion();
    // What the system lists the window under.
    m_window.SetTitle("NXEmu " + std::string(VER_FILE_VERSION_STR) + (game.empty() ? std::string() : " - " + game));
}

void MainWindow::ShowVersion()
{
    m_document->GetElementById("version-notification")->SetClass("shown", m_versionCopiedUntil != 0);
    // The badge keeps to the release and the build; the whole version shows on hover.
    const std::string version = VER_FILE_VERSION_STR;
    const size_t dash = version.find('-');
    const std::string shortVersion = version.substr(0, dash == std::string::npos ? dash : version.find('-', dash + 1));
    // Copying lights the badge up; its text stays, not to move the menus beside it.
    SetText("title-version", shortVersion);
    if (Rml::Element * badge = m_document->GetElementById("title-version"))
    {
        badge->SetAttribute("title", version);
        badge->SetClass("copied", m_versionCopiedUntil != 0);
    }
}

// The mouse resting over a running game is hidden, as a console has none.
void MainWindow::UpdateMouseCursor()
{
    constexpr uint64_t kMouseHideMs = 2500;
    bool hide = false;
    const uint64_t now = SDL_GetTicks64();
    if (m_phase == EmulatorSession::Phase::Running && GameArea() != nullptr && now - m_mouseMovedAt >= kMouseHideMs && !DialogOpen() &&
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
    // What hides the interface says how to get it back, for a moment: once
    // the game shows, as the loading screen would hide it.
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
        SetText("game-hint-text", fullscreen ? Tr("Press %1 or %2 to leave fullscreen", {Hotkeys::Get(HotkeyId::ExitFullscreen).Label(), Hotkeys::Get(HotkeyId::Fullscreen).Label()}) :
                                               Tr("Press %1 to show the interface", {Hotkeys::Get(HotkeyId::HideUi).Label()}));
    }
    if (m_hintUntil != 0 && SDL_GetTicks64() >= m_hintUntil)
    {
        m_hintUntil = 0;
    }
    if (m_versionCopiedUntil != 0 && SDL_GetTicks64() >= m_versionCopiedUntil)
    {
        m_versionCopiedUntil = 0;
        ShowVersion();
    }
    m_document->GetElementById("game-hint")->SetClass("shown", m_hintUntil != 0);
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
    // The game is told the area's size in window coordinates: a high-density
    // display sharpens the interface without enlarging what the game draws.
    const float scale = m_window.PixelScale();
    m_session.SetViewportSize(static_cast<uint32_t>(std::lround(size.x / scale)), static_cast<uint32_t>(std::lround(size.y / scale)));
}

const PixelRect * MainWindow::GameArea() const
{
    const bool playing = m_phase == EmulatorSession::Phase::Running || m_phase == EmulatorSession::Phase::Paused;
    return playing && m_gameArea.width > 0 ? &m_gameArea : nullptr;
}

bool MainWindow::AttachVideo()
{
    if (m_session.IsValid() && m_window.AttachVideo(m_session.Modules().Video()))
    {
        return true;
    }
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "NXEmu", m_window.Error().c_str(), m_window.Handle());
    m_quit = true;
    return false;
}

void MainWindow::ResetWindowSize(int width, int height)
{
    // The requested size is the game's; the window adds its menu and status bar.
    int windowWidth = 0, windowHeight = 0;
    m_window.SetFullscreen(false);
    m_window.GetClientSize(windowWidth, windowHeight);
    const float scale = m_window.PixelScale();
    const int gameWidth = static_cast<int>(std::lround(m_gameArea.width / scale));
    const int gameHeight = static_cast<int>(std::lround(m_gameArea.height / scale));
    m_window.SetClientSize(width + windowWidth - gameWidth, height + windowHeight - gameHeight);
}

// The width a card takes in a row, with the gap that follows it, in pixels.
float MainWindow::CardStep() const
{
    const float ratio = m_document->GetContext()->GetDensityIndependentPixelRatio();
    return static_cast<float>(m_iconSize + 2 * kCardBorder + (GameIconStep(m_iconSize) == 1 ? kSmallCardGap : kCardGap)) * ratio;
}

// Gives the window the nearest width that whole cards fill, and keeps it
// within the screen. The room of the scrollbar is kept free: the cards do
// not move when it comes.
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
        // A pixel more: a row that is exactly full must not lose its last card to rounding.
        width = static_cast<int>(std::ceil((around + columns * CardStep() + scrollbar + 1.0f) / m_window.PixelScale()));
    }
    m_window.SetClientSize(width, height);
    m_window.Centre();
}

void MainWindow::AnnounceGame(const std::string & path)
{
    m_gameShown = false;
    if (Rml::Element * search = m_document->GetElementById("games-search"))
    {
        // What is typed from here on is for the game.
        search->Blur();
    }
    const auto game = std::find_if(m_games.begin(), m_games.end(), [&path](const GameEntry & entry) { return entry.path == path; });
    if (game != m_games.end())
    {
        m_loadingScreen->Announce(game->title, UiRenderer::FileImageSource(game->icon));
    }
    else
    {
        m_loadingScreen->Announce({}, {});
    }
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
        // The profiles are the modules'.
        if (m_session.IsValid())
        {
            m_profilesDialog->Show();
        }
        return m_profilesDialog->IsVisible();
    case SettingsPage::Directories:
        m_directoriesDialog->Show();
        return m_directoriesDialog->IsVisible();
    case SettingsPage::Advanced:
    case SettingsPage::Debug:
        m_advancedDialog->ShowPage(page);
        return m_advancedDialog->IsVisible();
    }
    return false;
}

void MainWindow::SetDockedMode(bool docked)
{
    UiPreferences::SetStatusInt(NXOsSetting::DockedMode, static_cast<int32_t>(docked ? DockedMode::Docked : DockedMode::Handheld));
    m_session.FlushSettings();
    UpdateStatusWidgets();
}

// A range of a menu moved. The menu is not made again for it: it shows the
// value already.
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
    // A menu that opens shows the sharpness: that is not the user choosing it.
    if (id == kFilterSharpnessRange && value != SettingsStore::GetInstance().GetInt(NXVideoSetting::FSPSharpness))
    {
        UiPreferences::SetStatusInt(NXVideoSetting::FSPSharpness, value);
        UpdateStatusWidgets();
    }
}

void MainWindow::ShowSelectMenu(Rml::Element * origin)
{
    std::vector<MenuItem> items;
    const SettingsStore & store = SettingsStore::GetInstance();
    // Choosing what the game settings say goes back to them.
    const auto choice = [&items](int id, std::string label, bool chosen) -> MenuItem & {
        items.push_back(MenuItem::Choice(id, std::move(label), chosen));
        return items.back();
    };
    if (origin->GetId() == "games-sort")
    {
        const int current = GameSort();
        for (int sort = 0; sort < kGameSortCount; sort++)
        {
            items.push_back(MenuItem::Choice(static_cast<int>(Action::GameSortFirst) + sort, Tr(kGameSorts[sort]), sort == current));
        }
    }
    else if (origin->GetId() == "profile")
    {
        const int active = UserProfiles::Active(m_profiles.size());
        for (int profile = 0; profile < static_cast<int>(m_profiles.size()); profile++)
        {
            choice(static_cast<int>(Action::ProfileFirst) + profile, m_profiles[profile].name, profile == active);
        }
        items.push_back(MenuItem::Separator());
        items.push_back(MenuItem::Action(static_cast<int>(Action::OpenProfiles), Tr("Manage profiles")));
    }
    else if (origin->GetId() == "docked-mode")
    {
        const bool docked = IsDocked();
        choice(static_cast<int>(Action::SetDockedMode), Tr("Docked"), docked);
        choice(static_cast<int>(Action::SetHandheldMode), Tr("Handheld"), !docked);
    }
    else if (origin->GetId() == "scaling-filter")
    {
        const int current = store.GetInt(NXVideoSetting::ScalingFilter);
        const int game = store.GetIntWithoutControl(NXVideoSetting::ScalingFilter);
        for (const auto & filter : kScalingFilters)
        {
            const int value = static_cast<int>(filter.value);
            items.push_back(MenuItem::Check(static_cast<int>(Action::ScalingFilterFirst) + value, Tr(filter.label), value == current));
            MenuItem & item = items.back();
            // The filter the game settings choose
            item.tag = value == game ? Tr("Default") : std::string();
            item.hint = Tr(filter.hint);
            if (filter.value == ScalingFilter::Fsr)
            {
                // Only FSR has a sharpness; it is set once FSR is the filter.
                item.range = MenuItem::Range{kFilterSharpnessRange, kSharpnessMax, std::clamp(store.GetInt(NXVideoSetting::FSPSharpness), 0, kSharpnessMax),
                                             value == current, value == current ? Tr("Sharpness of the FSR filter") : Tr("Sharpness: once FSR is the filter")};
            }
        }
    }
    else if (origin->GetId() == "volume")
    {
        if (UiPreferences::HasStatusOverride({NXOsSetting::AudioVolume, NXOsSetting::AudioMuted}))
        {
            items.push_back(MenuItem::Action(static_cast<int>(Action::ResetVolumeOverrides), Tr("Use game settings")));
        }
    }
    else
    {
        const int current = ResolutionScale();
        for (int scale = 0; scale < kResolutionScaleCount; scale++)
        {
            choice(static_cast<int>(Action::ResolutionScaleFirst) + scale, kResolutionScales[scale].label, scale == current);
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

void MainWindow::UpdateLoadingScreen()
{
    if (!m_loadingScreen->HasGame())
    {
        // The loader opens the game shortly after loading starts.
        IRomInfoPtr info(m_session.Modules().Systemloader().LoadedRomInfo());
        if (info)
        {
            m_loadingScreen->SetGame(*info);
        }
    }
    m_loadingScreen->Update();
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
    // The only filter no longer offered is the one the settings may still name.
    const ScalingFilterChoice * filter = FindScalingFilter(settings.GetInt(NXVideoSetting::ScalingFilter));
    SetText("scaling-filter-text", filter != nullptr ? Tr(filter->label) : Tr("Gaussian"));
    // A control that chose over the settings shows it.
    const auto mark = [this](const char * id, std::initializer_list<const char *> chosen) {
        if (Rml::Element * element = m_document->GetElementById(id))
        {
            element->SetClass("overridden", UiPreferences::HasStatusOverride(chosen));
        }
    };
    mark("docked-mode", {NXOsSetting::DockedMode});
    mark("resolution-scale", {NXVideoSetting::ResolutionSetup});
    mark("scaling-filter", {NXVideoSetting::ScalingFilter, NXVideoSetting::FSPSharpness});
    mark("profile", {NXOsSetting::CurrentUser});
    mark("volume", {NXOsSetting::AudioVolume, NXOsSetting::AudioMuted});
    SetText("resolution-scale-text", scale.label);
    // What the game is drawn at: the console's resolution in this mode, scaled.
    SetText("resolution", std::to_string(static_cast<int>(std::lround((docked ? kDockedHeight : kHandheldHeight) * scale.factor))) + "p");
    if (Rml::Element * select = m_document->GetElementById("resolution-scale"))
    {
        // The video module reads the scale when a game starts.
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
        // Who plays matters only when there is someone else to play as.
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

void MainWindow::RefreshProfiles()
{
    m_profiles = m_session.IsValid() ? UserProfiles::List(m_session.Modules().OperatingSystem()) : std::vector<UserProfile>();
    UpdateStatusWidgets();
}

// How the game ran since it was last measured, for the overlay and for the
// status bar: measuring starts again from nothing, so it is done here alone.
void MainWindow::MeasureGame()
{
    m_speedText.clear();
    const bool measured = m_longestMeasured;
    m_longestMeasured = false;
    if (!m_session.IsRunning())
    {
        return;
    }
    IOperatingSystem & os = m_session.Modules().OperatingSystem();
    // The longest picture is measured for the overlay alone. Its first
    // measure began before that was asked for, and is not one to show.
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
        m_speedText = Format("%.0f FPS", std::round(stats.average_game_fps)) + Format(" (%.2f ms)", frametime);
    }
    UpdateStatusText();
}

void MainWindow::UpdateStatusText()
{
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
    // The firmware matters to choose a game, not while one runs.
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
    // A speed that is not held to that of the console has its sign after it.
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

void MainWindow::LoadFile()
{
    const std::string filter = FileFilter({{Tr("Switch games"), "*.dxci;*.dnsp;*.nro"}, {Tr("All files"), "*.*"}});
    Path selected;
    if (FileSelect(m_window.NativeHandle(), Path(Path::CURRENT_DIRECTORY), filter.c_str(), true, selected))
    {
        OpenGame(static_cast<const char *>(selected));
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
    if (!UiPreferences::GetBool(UiPreferences::ConfirmBeforeStopping, true))
    {
        return true;
    }
    const std::string message = m_session.Modules().OperatingSystem().GetExitLocked()
                                    ? Tr("The currently running application has requested NXEmu to not exit.\n\nWould you like to bypass this and stop emulation anyway?")
                                    : Tr("Are you sure you want to stop the emulation?\n\nAny unsaved progress will be lost.");
    return g_notify->Query(message.c_str(), title.c_str()) == NotificationResponse::Yes;
}
