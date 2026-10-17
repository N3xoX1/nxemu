#include "main_window.h"
#include <nxemu/ui/i18n/translation.h>
#include "rml_events.h"
#include <RmlUi/Core.h>
#include <algorithm>
#include <cmath>
#include <nxemu-core/notification.h>
#include <nxemu-core/version.h>
#include <nxemu/ui/host/shell.h>
#include <nxemu/ui/host/ui_window.h>
#include <yuzu_common/fs/path_util.h>

namespace
{
constexpr const char * kDiscordUrl = "https://discord.gg/hEa4hNyFWU";
constexpr const char * kReportUrl = "https://report.nxemu.com/";
constexpr float kNarrowWidth = 800.0f;
constexpr float kCompactWidth = 560.0f;
constexpr uint64_t kVersionCopiedMs = 1500;
} // namespace

MainWindow::MainWindow(UiWindow & window, EmulatorSession & session, std::filesystem::path assetDir) :
    m_window(window),
    m_session(session),
    m_assetDir(std::move(assetDir))
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
    m_document->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
    return true;
}

void MainWindow::RequestQuit()
{
    if (!m_session.IsRunning())
    {
        m_quit = true;
    }
}

void MainWindow::HandleEvent(const SDL_Event & event)
{
    if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE && (m_menuBar->IsOpen() || m_gameMenu->IsOpen()))
    {
        m_menuBar->Close();
        m_gameMenu->Close();
        return;
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
    SyncWindowState();
    if (m_menuDirty && !m_menuBar->IsOpen())
    {
        m_menuDirty = false;
        RebuildMenu();
    }
    const Rml::Context * context = m_document->GetContext();
    const float width = static_cast<float>(context->GetDimensions().x) / context->GetDensityIndependentPixelRatio();
    m_document->SetClass("narrow", width < kNarrowWidth);
    m_document->SetClass("compact", width < kCompactWidth);
    m_window.Context()->Update();
}

bool MainWindow::Still() const
{
    return m_window.Context()->GetNextUpdateDelay() > 0;
}

void MainWindow::OnModulesReleasing()
{
    m_window.DetachVideo();
}

void MainWindow::OnModulesReady()
{
    AttachVideo();
}

void MainWindow::OnGameStarted(const std::string & /*path*/)
{
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
}

void MainWindow::Run(Action action)
{
    switch (action)
    {
    case Action::Exit: RequestQuit(); break;
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
    onClick("title-version", Action::CopyVersion);
    onClick("title-pause", Action::PauseOrContinue);
    onClick("title-stop", Action::Stop);
    onClick("window-close", Action::Exit);
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
    const auto action = [this](Action id, std::string label, bool enabled = true) {
        return MenuItem::Action(static_cast<int>(id), std::move(label), ShortcutLabel(id), enabled);
    };

    std::vector<MenuItem> file;
    file.push_back(action(Action::LoadFile, Tr("&Load File")));
    file.push_back(MenuItem::Separator());
    file.push_back(action(Action::OpenGameDirectories, Tr("Game directories")));
    file.push_back(MenuItem::Submenu(Tr("Open &NXEmu Folders"), {
                                                                action(Action::OpenAppDirectory, Tr("&App Directory")),
                                                                action(Action::OpenLogDirectory, Tr("&Log Folder")),
                                                            }));
    file.push_back(MenuItem::Separator());
    file.push_back(action(Action::Exit, Tr("E&xit")));

    std::vector<MenuItem> menus;
    menus.push_back(MenuItem::Submenu(Tr("&File"), std::move(file)));

    if (running)
    {
        menus.push_back(MenuItem::Submenu(Tr("&Emulation"), {
                                                         action(Action::PauseOrContinue, Tr("Pause")),
                                                         action(Action::Stop, Tr("&Stop")),
                                                     }));
    }

    std::vector<MenuItem> view;
    view.push_back(action(Action::ToggleFullscreen, Tr("&Fullscreen"), running));
    view.push_back(action(Action::ToggleHideUi, Tr("Hide &UI"), running));
    view.push_back(MenuItem::Separator());
    view.push_back(MenuItem::Submenu(Tr("Window size"), {
        action(Action::ResetWindowSize720p, "720p"),
        action(Action::ResetWindowSize900p, "900p"),
        action(Action::ResetWindowSize1080p, "1080p"),
    }));
    menus.push_back(MenuItem::Submenu(Tr("&View"), std::move(view)));

    menus.push_back(MenuItem::Submenu(Tr("&Options"), {
                                                      action(Action::OpenSettings, Tr("&Settings")),
                                                      action(Action::ConfigureControllers, Tr("&Controllers")),
                                                      action(Action::OpenProfiles, Tr("Profiles")),
                                                      action(Action::InstallFirmware, Tr("Install &Firmware"), !running),
                                                  }));
    menus.push_back(MenuItem::Submenu(Tr("&Help"), {
                                                   action(Action::OpenDiscord, Tr("&Discord")),
                                                   action(Action::OpenReportIssues, Tr("&Report Issues")),
                                                   MenuItem::Separator(),
                                                   action(Action::OpenAbout, Tr("&About NXEmu")),
                                               }));
    m_menuBar->SetMenus(menus);
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
    m_document->SetClass("video-visible", panel == Panel::Renderer);
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
    return nullptr;
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
