#include "main_window.h"
#include <nxemu/ui/i18n/translation.h>
#include "rml_events.h"
#include <RmlUi/Core.h>
#include <algorithm>
#include <cmath>
#include <nxemu-core/notification.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/version.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu/ui/host/shell.h>
#include <nxemu/ui/host/ui_renderer.h>
#include <nxemu/ui/host/ui_window.h>
#include <tuple>
#include <yuzu_common/fs/path_util.h>

namespace
{
constexpr const char * kDiscordUrl = "https://discord.gg/hEa4hNyFWU";
constexpr const char * kReportUrl = "https://report.nxemu.com/";
constexpr float kNarrowWidth = 800.0f;
constexpr float kCompactWidth = 560.0f;
constexpr uint64_t kVersionCopiedMs = 1500;
constexpr int kGameIconDefault = 146;
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

MainWindow::MainWindow(UiWindow & window, EmulatorSession & session, GameLibrary & library, std::filesystem::path assetDir) :
    m_window(window),
    m_session(session),
    m_library(library),
    m_assetDir(std::move(assetDir)),
    m_iconSize(kGameIconDefault)
{
    m_session.SetListener(this);
}

MainWindow::~MainWindow()
{
    m_session.SetListener(nullptr);
    m_window.DetachVideo();
}

bool MainWindow::Create()
{
    m_document = Translation::LoadDocument(*m_window.Context(), m_assetDir / "main_window.rml");
    if (m_document == nullptr)
    {
        return false;
    }
    m_menuBar = std::make_unique<MenuBar>(m_document->GetElementById("menu-bar"), [this](int id) { Post(static_cast<Action>(id)); });
    m_gameMenu = std::make_unique<PopupMenu>(m_document->GetElementById("popup-menu"), [this](int id) { Post(static_cast<Action>(id)); });
    const OpenSettingsPage openPage = [this](SettingsPage page) { return ShowSettingsPage(page); };
    m_generalDialog = std::make_unique<GeneralDialog>(m_window, m_session, m_settingsContext, m_assetDir, GeneralDialog::Host{[] {}, openPage});
    m_advancedDialog = std::make_unique<AdvancedDialog>(m_window, m_session, m_settingsContext, m_assetDir, openPage);
    m_controllerDialog = std::make_unique<ControllerDialog>(m_window, m_session, m_assetDir);
    m_directoriesDialog = std::make_unique<DirectoriesDialog>(m_window, m_library, m_assetDir,
                                                        DirectoriesDialog::Host{
                                                            [this] { return !m_session.IsRunning() && m_pendingGame.empty(); },
                                                            [this] { RescanGames(); },
                                                            openPage,
                                                        });
    m_profilesDialog = std::make_unique<ProfilesDialog>(m_window, m_session, m_assetDir,
                                                        ProfilesDialog::Host{
                                                            [this] { return !m_session.IsRunning() && m_pendingGame.empty(); },
                                                            [] {},
                                                            openPage,
                                                        });
    BindEvents();

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
    ApplyIconSize();
    m_library.StartScan(m_session.Modules());
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
    if (!m_session.IsRunning() || ConfirmStop(Tr("NXEmu - Exit")))
    {
        m_quit = true;
    }
}

void MainWindow::HandleEvent(const SDL_Event & event)
{
    if (event.type == SDL_MOUSEMOTION || event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEWHEEL)
    {
        m_mouseMovedAt = SDL_GetTicks64();
        UpdateMouseCursor();
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
    m_session.Update();
    if (!m_session.IsValid())
    {
        m_quit = true;
        return;
    }
    m_controllerDialog->Update();
    m_generalDialog->Update();
    m_advancedDialog->Update();
    m_directoriesDialog->Update();
    m_profilesDialog->Update();
    if (m_library.Scanning() != m_gamesScanning)
    {
        m_gamesScanning = !m_gamesScanning;
        m_gamesDirty = true;
    }
    std::vector<FoundGame> found;
    if (m_library.TakeFound(found))
    {
        AddGames(std::move(found));
    }
    if (m_gamesDirty)
    {
        RenderGames();
    }
    RenderVisibleGames();
    LoadPendingIcons();
    SyncPhase();
    if (m_rescanGames && m_phase == EmulatorSession::Phase::Idle && m_pendingGame.empty())
    {
        m_rescanGames = false;
        RescanGames();
    }
    SyncWindowState();
    UpdateMouseCursor();
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
    m_window.Context()->Update();
    LayoutGameArea();
}

bool MainWindow::Still() const
{
    return m_phase != EmulatorSession::Phase::Loading && m_pendingGame.empty() && !m_controllerDialog->Listening() && !m_profilesDialog->Busy() &&
           m_window.Context()->GetNextUpdateDelay() > 0;
}

bool MainWindow::DialogOpen() const
{
    return m_controllerDialog->IsVisible() || m_generalDialog->IsVisible() || m_advancedDialog->IsVisible() || m_directoriesDialog->IsVisible() ||
           m_profilesDialog->IsVisible();
}

void MainWindow::OnModulesReleasing()
{
    if (m_library.Scanning())
    {
        m_library.StopScan();
        m_rescanGames = true;
    }
    m_controllerDialog->Abandon();
    m_profilesDialog->Abandon();
    m_window.DetachVideo();
}

void MainWindow::OnModulesReady()
{
    AttachVideo();
}

void MainWindow::OnGameStarted(const std::string & path)
{
    const auto game = std::find_if(m_games.begin(), m_games.end(), [&path](const GameEntry & entry) { return entry.path == path; });
    SetTitle(game != m_games.end() ? game->title : SettingsStore::GetInstance().GetString(NXCoreSetting::GameName));
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
    if (!m_pendingGame.empty())
    {
        if (!m_launchReady)
        {
            m_launchReady = true;
            AnnounceGame();
            if (!m_session.IsRunning())
            {
                return;
            }
        }
        m_launchReady = false;
        std::string path;
        path.swap(m_pendingGame);
        m_session.LoadGame(path);
    }
}

void MainWindow::Run(Action action)
{
    switch (action)
    {
    case Action::Exit: RequestQuit(); break;
    case Action::PauseOrContinue: m_session.TogglePause(); break;
    case Action::Stop: StopGame(); break;
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
        if (m_session.IsValid())
        {
            m_controllerDialog->Show();
        }
        break;
    case Action::CopyVersion:
        SDL_SetClipboardText(VER_FILE_VERSION_STR);
        m_versionCopiedUntil = SDL_GetTicks64() + kVersionCopiedMs;
        ShowVersion();
        break;
    default:
        break;
    }
}

std::string MainWindow::ShortcutLabel(Action /*action*/) const
{
    return {};
}

void MainWindow::BindEvents()
{
    const auto onClick = [this](const char * id, Action action) {
        UiEvents::Listen(m_document->GetElementById(id), "click", [this, action](Rml::Event &) { Post(action); });
    };
    onClick("title-settings", Action::OpenSettings);
    onClick("title-controllers", Action::ConfigureControllers);
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
    });
    UiEvents::Listen(m_document->GetElementById("games-grid"), "dblclick", [this](Rml::Event & event) {
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
        MenuItem::Separator(),
        check(Action::ToggleSpeedLimit, Tr("Limit &Speed"), SettingsStore::GetInstance().GetBool(NXOsSetting::UseSpeedLimit)),
    };
    for (MenuItem & item : emulation)
    {
        item.enabled = item.id == static_cast<int>(Action::Stop) ? canStop : playing;
    }

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
    emulation.push_back(MenuItem::Separator());
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
    for (FoundGame & entry : found)
    {
        if (!entry.icon.empty())
        {
            entry.game.icon = m_window.Renderer().AddMemoryImage(std::move(entry.icon));
        }
        m_games.push_back(std::move(entry.game));
    }
    std::stable_sort(m_games.begin(), m_games.end(), [](const GameEntry & left, const GameEntry & right) {
        return std::forward_as_tuple(Folded(left.title), left.path) < std::forward_as_tuple(Folded(right.title), right.path);
    });
    m_gamesDirty = true;
}

void MainWindow::RescanGames()
{
    for (const GameEntry & game : m_games)
    {
        if (!game.icon.empty())
        {
            m_loadedIcons.erase(game.icon);
            m_window.Renderer().RemoveMemoryImage(game.icon);
        }
    }
    m_games.clear();
    m_selectedGame = nullptr;
    m_library.StartScan(m_session.Modules());
    m_gamesDirty = true;
}

void MainWindow::RenderGames()
{
    m_gamesDirty = false;
    Rml::Element * grid = m_document->GetElementById("games-grid");
    if (grid == nullptr)
    {
        return;
    }
    m_selectedGame = nullptr;

    const std::string search = Folded(m_gameSearch);
    std::vector<size_t> shown;
    for (size_t i = 0; i < m_games.size(); i++)
    {
        if (search.empty() || Folded(m_games[i].title).find(search) != std::string::npos)
        {
            shown.push_back(i);
        }
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
    float rowHeight = m_gameRowHeight;
    if (Rml::Element * item = grid->QuerySelector(".rom-game"))
    {
        const float height = item->GetOffsetHeight() + item->GetBox().GetEdge(Rml::BoxArea::Margin, Rml::BoxEdge::Bottom);
        rowHeight = height > 0.0f ? height : rowHeight;
    }
    const size_t columns = static_cast<size_t>(std::max(grid->GetClientWidth() / CardStep(), 1.0f));
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
    const auto itemsRml = [this, columns, count](size_t fromRow, size_t toRow) {
        std::string rml;
        for (size_t position = fromRow * columns; position < std::min(toRow * columns, count); position++)
        {
            rml += CardRml(m_shownGames[position]);
        }
        return rml;
    };
    const auto spacerHeight = [rowHeight](size_t spacerRows) { return Rml::Property(static_cast<float>(spacerRows) * rowHeight, Rml::Unit::PX); };

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
    std::string details;
    if (!game.version.empty())
    {
        details += "<div class=\"rom-chip rom-version\"><img src=\"images/gear.png\"/><div>v" + Rml::StringUtilities::EncodeRml(game.version) + "</div></div>";
    }
    rml += "</div><div class=\"rom-info\"><div class=\"rom-title\">" + Rml::StringUtilities::EncodeRml(game.title) + "</div>" +
           (details.empty() ? std::string() : "<div class=\"rom-details\">" + details + "</div>") + "</div></div>";
    return rml;
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

void MainWindow::SelectGame(Rml::Element * card)
{
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
}

bool MainWindow::ShowSettingsPage(SettingsPage page)
{
    if (m_settingsContext.IsGame() && page != SettingsPage::General && page != SettingsPage::Advanced) return false;
    switch (page)
    {
    case SettingsPage::General:
        m_generalDialog->Show();
        return m_generalDialog->IsVisible();
    case SettingsPage::Profiles:
        if (m_session.IsValid())
        {
            m_profilesDialog->Show();
        }
        return m_profilesDialog->IsVisible();
    case SettingsPage::Directories:
        m_directoriesDialog->Show();
        return m_directoriesDialog->IsVisible();
    case SettingsPage::Advanced:
        m_advancedDialog->Show();
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

void MainWindow::SyncPhase()
{
    const EmulatorSession::Phase phase = m_session.CurrentPhase();
    const bool emulating = m_session.IsRunning() && (phase == EmulatorSession::Phase::Running || phase == EmulatorSession::Phase::Paused);
    const bool paused = emulating && phase == EmulatorSession::Phase::Paused;
    m_document->SetClass("emulating", emulating);
    m_document->SetClass("game-paused", paused);
    m_document->SetClass("game-controls-visible", m_session.IsRunning() || !m_pendingGame.empty());
    if (Rml::Element * pause = m_document->GetElementById("title-pause"))
    {
        if (!emulating && !pause->HasAttribute("disabled"))
        {
            pause->SetAttribute("disabled", "");
        }
        else if (emulating && pause->HasAttribute("disabled"))
        {
            pause->RemoveAttribute("disabled");
        }
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
        if (phase == EmulatorSession::Phase::Idle)
        {
            m_window.SetFullscreen(false);
            SetTitle({});
        }
    }

    switch (m_phase)
    {
    case EmulatorSession::Phase::Idle: ShowPanel(Panel::RomBrowser); break;
    case EmulatorSession::Phase::Loading: ShowPanel(m_gameShown ? Panel::RomBrowser : Panel::Loading); break;
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
}

void MainWindow::AnnounceGame()
{
    m_gameShown = false;
    if (Rml::Element * search = m_document->GetElementById("games-search"))
    {
        search->Blur();
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
    const float scale = m_window.PixelScale();
    m_session.SetViewportSize(static_cast<uint32_t>(std::lround(size.x / scale)), static_cast<uint32_t>(std::lround(size.y / scale)));
}

void MainWindow::SetTitle(const std::string & game)
{
    SetText("title-text", game);
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

void MainWindow::UpdateMouseCursor()
{
}

void MainWindow::SyncWindowState()
{
    const bool fullscreen = m_window.IsFullscreen();
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
    m_window.SetFullscreen(false);
    m_window.SetClientSize(width, height);
}

void MainWindow::UpdateStatusText()
{
    SetText("status-text", {});
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
        m_menuDirty = true;
    }
    if (m_session.IsRunning() && ConfirmStop(Tr("NXEmu - Stop Emulation")))
    {
        m_session.Stop();
    }
}

bool MainWindow::ConfirmStop(const std::string & title)
{
    const std::string message = m_session.Modules().OperatingSystem().GetExitLocked()
                                    ? Tr("The currently running application has requested NXEmu to not exit.\n\nWould you like to bypass this and stop emulation anyway?")
                                    : Tr("Are you sure you want to stop the emulation?\n\nAny unsaved progress will be lost.");
    return g_notify->Query(message.c_str(), title.c_str()) == NotificationResponse::Yes;
}
