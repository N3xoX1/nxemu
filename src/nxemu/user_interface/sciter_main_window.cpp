#include "sciter_main_window.h"
#include "settings/input_config.h"
#include "settings/system_config.h"
#include "settings/ui_settings.h"
#include "user_interface/about_dialog.h"
#include "user_interface/key_mappings.h"
#include "user_interface/notification.h"
#include <common/base64.h>
#include <common/shell_open.h>
#include <common/std_string.h>
#include <nxemu-core/notification.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/version.h>
#include <nxemu-loader/loader_settings_identifiers.h>
#include <nxemu-module-spec/operating_system.h>
#include <nxemu-module-spec/system_loader.h>
#include <nxemu-module-spec/video.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu-video/video_settings_identifiers.h>
#include <nxemu/settings/ui_identifiers.h>
#include <sciter_element.h>
#include <widgets/menubar.h>
#include <yuzu_common/fs/filesystem_interfaces.h>
#include <yuzu_common/fs/path_util.h>
#include <yuzu_common/settings.h>
#include <yuzu_common/settings_input.h>

#include <Windows.h>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace
{

constexpr const char * kNxEmuDiscordUrl = "https://discord.gg/hEa4hNyFWU";

const char * RendererBackendLabel(RendererBackend backend)
{
    switch (backend)
    {
    case RendererBackend::OpenGL: return "OpenGL";
    case RendererBackend::Vulkan: return "Vulkan";
    case RendererBackend::Null: return "Null";
    }
    return "OpenGL";
}

} // namespace

struct Win32FullscreenState
{
    bool active = false;
    uint32_t pendingSwallowKeyUp = 0;
    WINDOWPLACEMENT placement{};
    LONG_PTR savedStyle = 0;
    LONG_PTR savedExStyle = 0;
};

namespace
{
enum
{
    EVENT_EMULATION_LOADING = 0x2000,
    EVENT_EMULATION_RUNNING = 0x2001,
    EVENT_EMULATION_STOPPED = 0x2002,
    EVENT_EMULATION_FIRST_FRAME = 0x2003,
    EVENT_DISK_CACHE_STATUS = 0x2004,
    EVENT_FIRMWARE_INSTALL_DONE = 0x2005,
    EVENT_FIRMWARE_INSTALL_ACTIVE = 0x2006,
    EVENT_FIRMWARE_INSTALL_FINISHED = 0x2007,
};

const uint32_t kKeyboardStateControl = 0x0040u | 0x0080u;
const uint32_t kKeyboardStateAlt = 0x0100u | 0x0200u;
const uint32_t kKeyboardStateShift = 0x0001u | 0x0002u;

bool AcceleratorMatchesKey(const MenuBarAccelerator & accel, uint32_t keyCode, uint32_t keyboardState)
{
    if (accel.IsNone())
    {
        return false;
    }
    if (keyCode != accel.key)
    {
        return false;
    }
    bool ctrl = (keyboardState & kKeyboardStateControl) != 0;
    bool alt = (keyboardState & kKeyboardStateAlt) != 0;
    bool shift = (keyboardState & kKeyboardStateShift) != 0;
    return ctrl == accel.ctrl && alt == accel.alt && shift == accel.shift;
}

void LoadImageToElement(SciterElement elem, const std::vector<uint8_t> & data)
{
    if (!elem.IsValid())
    {
        return;
    }
    if (!data.empty())
    {
        const char * mime = "image/png";
        if (data.size() >= 2 && data[0] == 0xff && data[1] == 0xd8)
        {
            mime = "image/jpeg";
        }
        else if (data.size() >= 4 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F')
        {
            mime = "image/gif";
        }
        const std::string uri = stdstr_f("data:%s;base64,%s", mime, base64_encode(data.data(), data.size()).c_str());
        elem.SetAttribute("src", uri.c_str());
    }
    elem.SetStyleAttribute("display", data.empty() ? "none" : "block");
}

std::string GetInstalledFirmwareDisplayVersion(ISystemloader & loader)
{
    char buffer[32]{};
    const uint32_t length = loader.GetInstalledFirmwareDisplayVersion(buffer, sizeof(buffer));
    if (length == 0)
    {
        return {};
    }
    return std::string(buffer, length);
}

void UpdateLoadingProgressBar(SciterElement & fillEl, bool indeterminate, int widthPercent, bool shaderBuilding)
{
    if (!fillEl.IsValid())
    {
        return;
    }
    if (indeterminate)
    {
        fillEl.AddClassName("indeterminate");
        fillEl.RemoveClassName("building");
        fillEl.SetStyleAttribute("width", "42%");
    }
    else
    {
        fillEl.RemoveClassName("indeterminate");
        if (shaderBuilding)
        {
            fillEl.AddClassName("building");
        }
        else
        {
            fillEl.RemoveClassName("building");
        }
        const int w = (std::max)(0, (std::min)(100, widthPercent));
        fillEl.SetStyleAttribute("width", stdstr_f("%d%%", w).c_str());
    }
}

std::string HtmlEscapeForHtmlContent(const std::string & s)
{
    std::string out;
    out.reserve(s.size());
    for (const char c : s)
    {
        switch (c)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

std::string GetGameTitleLoadingHtml(ISystemloader & loader, const char * verb)
{
    IRomInfoPtr info(loader.LoadedRomInfo());
    if (!info)
    {
        return stdstr_f("<span class=\"loading-verb\">%s</span> <span class=\"loading-game-name\">...</span>", verb);
    }
    uint32_t sz = 0;
    if (info->ReadTitle(nullptr, &sz) != LoaderResultStatus::Success || sz == 0)
    {
        return stdstr_f("<span class=\"loading-verb\">%s</span> <span class=\"loading-game-name\">...</span>", verb);
    }
    std::vector<char> buf(static_cast<size_t>(sz) + 1, 0);
    if (info->ReadTitle(buf.data(), &sz) != LoaderResultStatus::Success)
    {
        return stdstr_f("<span class=\"loading-verb\">%s</span> <span class=\"loading-game-name\">...</span>", verb);
    }
    const std::string titleEsc = HtmlEscapeForHtmlContent(std::string(buf.data()));
    return stdstr_f("<span class=\"loading-verb\">%s</span> <span class=\"loading-game-name\">%s</span>", verb, titleEsc.c_str());
}

} // namespace

SciterMainWindow::SciterMainWindow(ISciterUI & sciterUI, const char * windowTitle) :
    m_sciterUI(sciterUI),
    m_window(nullptr),
    m_renderWindow(nullptr),
    m_windowTitle(windowTitle),
    m_emulationRunning(false),
    m_pendingStartInFullscreen(false),
    m_hideUi(false),
    m_lastDiskCacheStatusPostMs(0),
    m_lastPostedDiskCacheStage(0),
    m_shownFirstFrame(false),
    m_win32Fullscreen(std::make_unique<Win32FullscreenState>()),
    m_firmwareInstallInProgress(false),
    m_firmwareInstallUiActive(false)
{
    SettingsStore & settings = SettingsStore::GetInstance();
    settings.RegisterCallback(NXCoreSetting::EmulationRunning, SciterMainWindow::EmulationRunning, this);
    settings.RegisterCallback(NXCoreSetting::EmulationState, SciterMainWindow::EmulationStateChanged, this);
    settings.RegisterCallback(NXCoreSetting::GameFile, SciterMainWindow::GameFileChanged, this);
    settings.RegisterCallback(NXCoreSetting::GameName, SciterMainWindow::GameNameChanged, this);
    settings.RegisterCallback(NXCoreSetting::DisplayedFrames, SciterMainWindow::DisplayedFramesChanged, this);
    settings.RegisterCallback(NXCoreSetting::DiskCacheLoadTick, SciterMainWindow::DiskCacheLoadChanged, this);
    settings.RegisterCallback(NXLoaderSetting::FirmwareInstallTotal, SciterMainWindow::FirmwareInstallTotalChanged, this);
    settings.RegisterCallback(NXOsSetting::AudioMuted, SciterMainWindow::SettingChanged, this);
    settings.RegisterCallback(NXOsSetting::AudioVolume, SciterMainWindow::SettingChanged, this);
    settings.RegisterCallback(NXOsSetting::SpeedLimit, SciterMainWindow::SettingChanged, this);
    settings.RegisterCallback(NXOsSetting::UseMultiCore, SciterMainWindow::SettingChanged, this);
    settings.RegisterCallback(NXOsSetting::UseSpeedLimit, SciterMainWindow::SettingChanged, this);
    settings.RegisterCallback(NXOsSetting::DockedMode, SciterMainWindow::SettingChanged, this);
    settings.RegisterCallback(NXUISetting::Hotkeys, SciterMainWindow::HotKeysChanged, this);
    settings.RegisterCallback(NXVideoSetting::ResolutionUpFactor, SciterMainWindow::SettingChanged, this);

    m_useMultiCore = settings.GetBool(NXOsSetting::UseMultiCore);
    m_useSpeedLimit = settings.GetBool(NXOsSetting::UseSpeedLimit);
    m_speedLimit = settings.GetBool(NXOsSetting::SpeedLimit);
    m_resolutionUpFactor = settings.GetFloat(NXVideoSetting::ResolutionUpFactor);
}

const char * SciterMainWindow::MenuIconResource(GuiAction action) const
{
    switch (action)
    {
    case GuiAction::LoadFile:
        return "open-file.svg";
    case GuiAction::ExitApplication:
        return "close.svg";
    case GuiAction::InstallFirmwareFromFile:
    case GuiAction::InstallFirmwareFromFolder:
        return "install.svg";
    case GuiAction::ToggleFullscreen:
        return "fullscreen.svg";
    case GuiAction::OpenControllersDialog:
        return "controller.svg";
    case GuiAction::OpenSystemConfiguration:
        return "settings.svg";
    case GuiAction::PauseOrContinueEmulation:
        return "pause.svg";
    case GuiAction::StopEmulation:
        return "stop.svg";
    case GuiAction::ToggleHideUi:
        return "hide-ui.svg";
    case GuiAction::ResetWindowSize720p:
    case GuiAction::ResetWindowSize900p:
    case GuiAction::ResetWindowSize1080p:
        return "resize.svg";
    case GuiAction::OpenAboutDialog:
        return "about.svg";
    case GuiAction::OpenDiscord:
        return "discord.svg";
    default:
        return nullptr;
    }
}

const std::string * SciterMainWindow::MenuIconSvgForResource(const char * resource)
{
    if (resource == nullptr)
    {
        return nullptr;
    }
    const auto found = m_menuIconSvgs.find(resource);
    if (found != m_menuIconSvgs.end())
    {
        return &found->second;
    }
    std::vector<uint8_t> data;
    if (!m_sciterUI.LoadResource(resource, data) || data.empty())
    {
        return nullptr;
    }
    std::string & cached = m_menuIconSvgs[resource];
    cached.assign(reinterpret_cast<const char *>(data.data()), data.size());
    return &cached;
}

const std::string * SciterMainWindow::MenuIconSvg(GuiAction action)
{
    const char * resource = MenuIconResource(action);
    return MenuIconSvgForResource(resource);
}

SciterMainWindow::~SciterMainWindow()
{
    Notification::GetInstance().ClearSciterContext();
    m_WebBrowser.DetachWindow();

    SettingsStore & settings = SettingsStore::GetInstance();
    settings.UnregisterCallback(NXCoreSetting::EmulationRunning, SciterMainWindow::EmulationRunning, this);
    settings.UnregisterCallback(NXCoreSetting::EmulationState, SciterMainWindow::EmulationStateChanged, this);
    settings.UnregisterCallback(NXCoreSetting::GameFile, SciterMainWindow::GameFileChanged, this);
    settings.UnregisterCallback(NXCoreSetting::GameName, SciterMainWindow::GameNameChanged, this);
    settings.UnregisterCallback(NXCoreSetting::DisplayedFrames, SciterMainWindow::DisplayedFramesChanged, this);
    settings.UnregisterCallback(NXCoreSetting::DiskCacheLoadTick, SciterMainWindow::DiskCacheLoadChanged, this);
    settings.UnregisterCallback(NXLoaderSetting::FirmwareInstallTotal, SciterMainWindow::FirmwareInstallTotalChanged, this);
    settings.UnregisterCallback(NXOsSetting::AudioMuted, SciterMainWindow::SettingChanged, this);
    settings.UnregisterCallback(NXOsSetting::AudioVolume, SciterMainWindow::SettingChanged, this);
    settings.UnregisterCallback(NXOsSetting::SpeedLimit, SciterMainWindow::SettingChanged, this);
    settings.UnregisterCallback(NXOsSetting::UseMultiCore, SciterMainWindow::SettingChanged, this);
    settings.UnregisterCallback(NXOsSetting::UseSpeedLimit, SciterMainWindow::SettingChanged, this);
    settings.UnregisterCallback(NXOsSetting::DockedMode, SciterMainWindow::SettingChanged, this);
    settings.UnregisterCallback(NXUISetting::Hotkeys, SciterMainWindow::HotKeysChanged, this);
    settings.UnregisterCallback(NXVideoSetting::ResolutionUpFactor, SciterMainWindow::SettingChanged, this);

    m_rootElement.SetTimer(0, (uint32_t *)TIMER_UPDATE_INSTALL_FIRMWARE);
    if (m_firmwareInstallThread.joinable())
    {
        m_firmwareInstallThread.join();
    }
    m_systemConfig.reset(nullptr);
    m_inputConfig.reset(nullptr);

    settings.SetBool(NXCoreSetting::ShuttingDown, true);
    if (m_romBrowser)
    {
        m_romBrowser->ClearItems();
    }
    m_modules.ShutDown();
}

void SciterMainWindow::RegisterApplets()
{
    if (!m_modules.IsValid())
    {
        return;
    }
    IOperatingSystem & os = m_modules.Modules().OperatingSystem();
    m_WebBrowser.AttachToWindow(m_window->GetHandle());
    os.SetFrontendApplets(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &m_WebBrowser);
}

void SciterMainWindow::ResetMenu()
{
    if (m_menuBar == nullptr)
    {
        return;
    }

    MenuBarItemList mainTitleMenu;
    MenuBarItemList fileMenu;
    fileMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::LoadFile), "&Load File...", nullptr, HotkeyAccelerator(Hotkey::LoadFile), MenuBarItem::CheckState::None, MenuIconSvg(GuiAction::LoadFile)));

    Stringlist & recentFiles = uiSettings.recentFiles;
    MenuBarItemList RecentFileMenu;
    if (recentFiles.size() > 0)
    {
        int32_t recentFileIndex = 0;
        for (Stringlist::const_iterator itr = recentFiles.begin(); itr != recentFiles.end(); itr++)
        {
            stdstr_f MenuString("%d %s", recentFileIndex + 1, itr->c_str());
            RecentFileMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::RecentFileMenuFirst) + recentFileIndex, MenuString.c_str()));
            recentFileIndex += 1;
        }
        fileMenu.push_back(MenuBarItem(MenuBarItem::SUB_MENU, "&Recent File", &RecentFileMenu, nullptr, MenuBarItem::CheckState::None, MenuIconSvgForResource("recent.svg")));
    }

    fileMenu.push_back(MenuBarItem(MenuBarItem::SPLITER));
    MenuBarItemList openFoldersMenu;
    openFoldersMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::OpenAppDirectory), "&App Directory", nullptr, nullptr, MenuBarItem::CheckState::None, nullptr));
    openFoldersMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::OpenLogDirectory), "&Log Folder", nullptr, nullptr, MenuBarItem::CheckState::None, nullptr));
    fileMenu.push_back(MenuBarItem(MenuBarItem::SUB_MENU, "Open &NXEmu Folders", &openFoldersMenu));
    fileMenu.push_back(MenuBarItem(MenuBarItem::SPLITER));
    fileMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::ExitApplication), "E&xit", nullptr, HotkeyAccelerator(Hotkey::Exit), MenuBarItem::CheckState::None, MenuIconSvg(GuiAction::ExitApplication)));
    mainTitleMenu.push_back(MenuBarItem(MenuBarItem::SUB_MENU, "&File", &fileMenu));

    MenuBarItemList systemMenu;
    if (m_emulationRunning)
    {
        bool paused = false;
        if (m_modules.IsValid())
        {
            paused = m_modules.Modules().OperatingSystem().IsEmulationPaused();
        }
        systemMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::PauseOrContinueEmulation), paused ? "Continue" : "Pause", nullptr, HotkeyAccelerator(Hotkey::PauseContinue), MenuBarItem::CheckState::None, MenuIconSvg(GuiAction::PauseOrContinueEmulation)));
        systemMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::StopEmulation), "&Stop", nullptr, HotkeyAccelerator(Hotkey::StopEmulation), MenuBarItem::CheckState::None, MenuIconSvg(GuiAction::StopEmulation)));
        mainTitleMenu.push_back(MenuBarItem(MenuBarItem::SUB_MENU, "&System", &systemMenu));
    }

    MenuBarItemList viewMenu;
    if (!m_emulationRunning)
    {
        viewMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::ToggleStartGamesInFullscreen), "&Start games in fullscreen", nullptr, nullptr, uiSettings.startGamesInFullscreen ? MenuBarItem::CheckState::Checked : MenuBarItem::CheckState::Unchecked));
        viewMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::ToggleStartGamesWithUiHidden), "Start games with &UI hidden", nullptr, nullptr, uiSettings.startGamesWithUiHidden ? MenuBarItem::CheckState::Checked : MenuBarItem::CheckState::Unchecked));
        viewMenu.push_back(MenuBarItem(MenuBarItem::SPLITER));
    }
    if (m_emulationRunning)
    {
        viewMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::ToggleFullscreen), "&Fullscreen", nullptr, HotkeyAccelerator(Hotkey::Fullscreen), MenuBarItem::CheckState::None, MenuIconSvg(GuiAction::ToggleFullscreen)));
    }
    if (m_emulationRunning)
    {
        viewMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::ToggleHideUi), m_hideUi ? "Show &UI" : "Hide &UI", nullptr, HotkeyAccelerator(Hotkey::HideUi), MenuBarItem::CheckState::None, MenuIconSvg(GuiAction::ToggleHideUi)));
    }
    MenuBarItemList resetWindowSizeMenu;
    const std::string * resizeMenuSvg = MenuIconSvg(GuiAction::ResetWindowSize720p);
    resetWindowSizeMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::ResetWindowSize720p), "Reset Window Size to 720p", nullptr, nullptr, MenuBarItem::CheckState::None, resizeMenuSvg));
    resetWindowSizeMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::ResetWindowSize900p), "Reset Window Size to 900p", nullptr, nullptr, MenuBarItem::CheckState::None, resizeMenuSvg));
    resetWindowSizeMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::ResetWindowSize1080p), "Reset Window Size to 1080p", nullptr, nullptr, MenuBarItem::CheckState::None, resizeMenuSvg));
    viewMenu.push_back(MenuBarItem(MenuBarItem::SUB_MENU, "Reset Window Size", &resetWindowSizeMenu, nullptr, MenuBarItem::CheckState::None, resizeMenuSvg));
    mainTitleMenu.push_back(MenuBarItem(MenuBarItem::SUB_MENU, "&View", &viewMenu));

    MenuBarItemList optionsMenu;
    optionsMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::OpenControllersDialog), "&Controllers...", nullptr, HotkeyAccelerator(Hotkey::Controllers), MenuBarItem::CheckState::None, MenuIconSvg(GuiAction::OpenControllersDialog)));
    optionsMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::OpenSystemConfiguration), "Confi&gure...", nullptr, HotkeyAccelerator(Hotkey::Configure), MenuBarItem::CheckState::None, MenuIconSvg(GuiAction::OpenSystemConfiguration)));
    MenuBarItemList installFirmwareMenu;
    if (!m_emulationRunning && !m_firmwareInstallInProgress && m_modules.IsValid())
    {
        optionsMenu.push_back(MenuBarItem(MenuBarItem::SPLITER));
        const std::string * installFirmwareSvg = MenuIconSvg(GuiAction::InstallFirmwareFromFile);
        installFirmwareMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::InstallFirmwareFromFile), "Install from &DXCI or ZIP...", nullptr, nullptr, MenuBarItem::CheckState::None, installFirmwareSvg));
        installFirmwareMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::InstallFirmwareFromFolder), "Install from &folder...", nullptr, nullptr, MenuBarItem::CheckState::None, installFirmwareSvg));
        optionsMenu.push_back(MenuBarItem(MenuBarItem::SUB_MENU, "Install &Firmware", &installFirmwareMenu));
    }
    mainTitleMenu.push_back(MenuBarItem(MenuBarItem::SUB_MENU, "&Options", &optionsMenu));

    MenuBarItemList helpMenu;
    helpMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::OpenDiscord), "&Discord", nullptr, nullptr, MenuBarItem::CheckState::None, MenuIconSvg(GuiAction::OpenDiscord)));
    helpMenu.push_back(MenuBarItem(MenuBarItem::SPLITER));
    helpMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::OpenAboutDialog), "&About NXEmu...", nullptr, nullptr, MenuBarItem::CheckState::None, MenuIconSvg(GuiAction::OpenAboutDialog)));
    mainTitleMenu.push_back(MenuBarItem(MenuBarItem::SUB_MENU, "&Help", &helpMenu));

    m_menuBar->AddSink(this);
    m_menuBar->SetMenuContent(mainTitleMenu);
}

bool SciterMainWindow::Show()
{
    enum
    {
        WINDOW_HEIGHT = 507,
        WINDOW_WIDTH = 760,
    };

    if (!m_sciterUI.WindowCreate(nullptr, "main_window.html", 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, SUIW_MAIN | SUIW_HIDDEN, m_window))
    {
        return false;
    }
    m_rootElement = m_window->GetRootElement();
    m_sciterUI.AttachHandler(m_rootElement, IID_IKEYSINK, (IKeySink *)this);
    m_sciterUI.AttachHandler(m_rootElement, IID_EVENTSINK, (IEventSink *)this);
    m_window->OnCloseSinkAdd(this);
    m_window->OnDestroySinkAdd(this);
    m_window->CenterWindow();
    SetCaption(m_windowTitle);

    SciterElement menuElement(m_rootElement.GetElementByID("MainMenu"));
    std::shared_ptr<void> interfacePtr = menuElement.IsValid() ? m_sciterUI.GetElementInterface(menuElement, IID_IMENUBAR) : nullptr;
    if (interfacePtr)
    {
        m_menuBar = std::static_pointer_cast<IMenuBar>(interfacePtr);
        ResetMenu();
    }
    m_sciterUI.UpdateWindow(m_rootElement.GetElementHwnd(true));
    SciterElement::RECT rect = {20, 20, 660, 500};
    SciterElement mainContents(m_rootElement.GetElementByID("MainContents"));
    if (mainContents.IsValid())
    {
        m_sciterUI.AttachHandler(mainContents, IID_IRESIZESINK, (IResizeSink *)this);
        rect = mainContents.GetLocation();
    }

    CreateRenderWindow();
    m_modules.Setup(*this);
    RegisterApplets();
    ResetMenu();
    UpdateStatusWidgets();
    UpdateEmulationStatusText();

    m_sciterUI.AttachHandler(m_rootElement.GetElementByID("dockedMode"), IID_ICLICKSINK, (IClickSink *)this);
    m_sciterUI.AttachHandler(m_rootElement.GetElementByID("renderer"), IID_ICLICKSINK, (IClickSink *)this);
    m_sciterUI.AttachHandler(m_rootElement.GetElementByID("volume"), IID_ICLICKSINK, (IClickSink *)this);
    m_sciterUI.AttachHandler(m_rootElement.GetElementByID("volumePopupBtn"), IID_ICLICKSINK, (IClickSink *)this);
    m_sciterUI.AttachHandler(m_rootElement.GetElementByID("audioVolume"), IID_ISTATECHANGESINK, (IStateChangeSink *)this);
    SciterElement volumePopup(m_rootElement.GetElementByID("VolumePopup"));
    if (volumePopup.IsValid())
    {
        m_sciterUI.AttachHandler(volumePopup, IID_EVENTSINK, (IEventSink *)this);
    }

    m_sciterUI.AttachHandler(m_rootElement, IID_ITIMERSINK, (ITimerSink *)this);
    m_rootElement.SetTimer(25, (uint32_t *)TIMER_UPDATE_INPUT);

    ShowPanel(Panel::RomBrowser);

    SciterElement rombrowser = m_rootElement.FindFirst("rombrowser");
    interfacePtr = rombrowser.IsValid() ? m_sciterUI.GetElementInterface(rombrowser, IID_ROMBROWSER) : nullptr;
    if (interfacePtr)
    {
        m_romBrowser = std::static_pointer_cast<IRomBrowser>(interfacePtr);
        m_romBrowser->SetMainWindow(this, &m_modules.Modules());
        m_romBrowser->PopulateAsync();
    }

    if (!uiSettings.hasBrokenVulkan)
    {
        PopulateVulkanRecords(m_vkDeviceRecords, RenderSurface());
    }
    Notification::GetInstance().SetSciterContext(&m_sciterUI, (void *)m_window->GetHandle());
    m_window->Show();
    return true;
}

void SciterMainWindow::ShowConfig(const char * startPage)
{
    m_systemConfig.reset(new SystemConfig(m_sciterUI, m_modules, m_vkDeviceRecords));
    m_systemConfig->Display((void *)m_window->GetHandle(), startPage);
}

void SciterMainWindow::LoadGame(const char * path)
{
    m_modules.Setup(*this);
    RegisterApplets();

    ISystemloader & loader = m_modules.Modules().Systemloader();
    loader.LoadRom(path);
    UpdateEmulationStatusText();
}

void SciterMainWindow::UpdateStatusWidgets()
{
    SettingsStore & settings = SettingsStore::GetInstance();
    SciterElement dockedMode(m_rootElement.GetElementByID("dockedMode"));
    if (dockedMode.IsValid())
    {
        stdstr_f text("%s", Settings::CanonicalizeEnum((Settings::DockedMode)settings.GetInt(NXOsSetting::DockedMode)).c_str());
        dockedMode.SetHTML((const uint8_t *)text.c_str(), text.size());
    }
    SciterElement renderer(m_rootElement.GetElementByID("renderer"));
    if (renderer.IsValid())
    {
        stdstr_f text("%s", RendererBackendLabel((RendererBackend)settings.GetInt(NXVideoSetting::GraphicsAPI)));
        renderer.SetHTML((const uint8_t *)text.c_str(), text.size());
    }

    SciterElement volume(m_rootElement.GetElementByID("volume"));
    if (volume.IsValid())
    {
        bool muted = settings.GetBool(NXOsSetting::AudioMuted);
        stdstr_f text(muted ? "Vol: Mute" : "Vol: %d %%", settings.GetInt(NXOsSetting::AudioVolume));
        volume.SetHTML((const uint8_t *)text.c_str(), text.size());
    }
}

void SciterMainWindow::UpdateInputDrivers()
{
    if (m_modules.IsValid())
    {
        IOperatingSystem & operatingSystem = m_modules.Modules().OperatingSystem();
        operatingSystem.PumpInputEvents();
    }
}

void SciterMainWindow::CreateRenderWindow()
{
    SciterElement mainContents(m_rootElement.GetElementByID("MainContents"));
    SciterElement::RECT rect = mainContents.GetLocation();
    uint32_t width = rect.right - rect.left;
    uint32_t height = rect.bottom - rect.top;
    m_renderWindow = CreateWindowExW(0, L"Static", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                     rect.left, rect.top, width, height, (HWND)m_window->GetHandle(), nullptr, GetModuleHandle(nullptr), nullptr);
    ShowWindow((HWND)m_renderWindow, SW_HIDE);

    if (m_modules.IsValid())
    {
        IVideo & video = m_modules.Modules().Video();
        video.UpdateFramebufferLayout(rect.right - rect.left, rect.bottom - rect.top);
    }
}

void SciterMainWindow::SetCaption(const std::string & caption)
{
    m_rootElement.Eval(stdstr_f("Window.this.caption = \"%s\";", caption.c_str()).c_str());
    SciterElement captionElement(m_rootElement.FindFirst("[role='window-caption'] > span"));
    if (captionElement.IsValid())
    {
        captionElement.SetHTML((uint8_t *)caption.data(), caption.size());
    }
}

void SciterMainWindow::EmulationRunning(const char * /*setting*/, void * userData)
{
    SciterMainWindow * impl = (SciterMainWindow *)userData;
    SettingsStore & settings = SettingsStore::GetInstance();
    impl->m_emulationRunning = settings.GetBool(NXCoreSetting::EmulationRunning);
    impl->m_pendingStartInFullscreen = impl->m_emulationRunning && uiSettings.startGamesInFullscreen;
    impl->m_pendingStartWithUiHidden = impl->m_emulationRunning && uiSettings.startGamesWithUiHidden;
    if (!impl->m_emulationRunning && impl->m_win32Fullscreen && impl->m_win32Fullscreen->active)
    {
        impl->ExitFullscreen();
    }
    if (!impl->m_emulationRunning)
    {
        impl->m_hideUi = false;
    }
    if (settings.GetBool(NXCoreSetting::ShuttingDown))
    {
        return;
    }
    if (impl->m_emulationRunning)
    {
        if (impl->m_renderWindow != nullptr)
        {
            DestroyWindow((HWND)impl->m_renderWindow);
            impl->m_renderWindow = nullptr;
        }
        impl->CreateRenderWindow();
    }
    SciterElement renderer(impl->m_rootElement.GetElementByID("renderer"));
    if (renderer)
    {
        renderer.SetState(impl->m_emulationRunning ? SciterElement::STATE_DISABLED : 0, impl->m_emulationRunning ? 0 : SciterElement::STATE_DISABLED, true);
    }
    impl->ResetMenu();
    impl->UpdateUIVisibility();
}

void SciterMainWindow::ShowPanel(Panel panel)
{
    struct PanelEntry
    {
        Panel panel;
        const char * id;
    };
    static const PanelEntry panels[] = {
        {Panel::RomBrowser, "RomBrowserPanel"},
        {Panel::Loading, "LoadingPanel"},
        {Panel::Pause, "PausePanel"},
    };

    for (const PanelEntry & entry : panels)
    {
        SciterElement elem(m_rootElement.GetElementByID(entry.id));
        if (!elem.IsValid())
        {
            continue;
        }
        elem.SetStyleAttribute("display", panel == entry.panel ? "block" : "none");
    }
    ShowWindow((HWND)m_renderWindow, panel == Panel::Renderer ? SW_SHOW : SW_HIDE);
    m_sciterUI.UpdateWindow(m_rootElement.GetElementHwnd(true));
}

void SciterMainWindow::EmulationStateChanged(const char * /*setting*/, void * userData)
{
    SciterMainWindow * impl = (SciterMainWindow *)userData;
    EmulationState state = (EmulationState)SettingsStore::GetInstance().GetInt(NXCoreSetting::EmulationState);

    if (state == EmulationState::RomLoaded)
    {
        impl->UpdateLoadingScreenDetails();
        SciterElement loadingMain(impl->m_rootElement.FindFirst("#LoadingPanel .loading-main"));
        if (loadingMain.IsValid())
        {
            loadingMain.RemoveClassName("no-game-icon");
        }
        impl->RefreshDiskCacheLoadingText();
        impl->m_shownFirstFrame = false;
        impl->ShowPanel(Panel::Loading);
    }
    else if (state == EmulationState::Running)
    {
        if (impl->m_pendingStartInFullscreen && uiSettings.startGamesInFullscreen)
        {
            impl->m_pendingStartInFullscreen = false;
            impl->EnterFullscreen();
        }
        if (impl->m_pendingStartWithUiHidden && uiSettings.startGamesWithUiHidden)
        {
            impl->m_pendingStartWithUiHidden = false;
            impl->m_hideUi = true;
            impl->UpdateUIVisibility();
            impl->m_sciterUI.UpdateWindow(impl->m_rootElement.GetElementHwnd(true));
            SciterElement mainContents(impl->m_rootElement.GetElementByID("MainContents"));
            if (mainContents.IsValid())
            {
                mainContents.Update(true);
            }
            impl->LayoutRenderWindow();
        }
        impl->m_rootElement.PostEvent(EVENT_EMULATION_RUNNING);
        impl->ResetMenu();
        if (impl->m_shownFirstFrame)
        {
            impl->ShowPanel(Panel::Renderer);
        }
    }
    else if (state == EmulationState::Paused)
    {
        impl->ResetMenu();
        impl->ShowPanel(Panel::Pause);
        impl->UpdateEmulationStatusText();
    }
    else if (state == EmulationState::Stopped)
    {
        impl->m_rootElement.PostEvent(EVENT_EMULATION_STOPPED);
    }
}

void SciterMainWindow::GameFileChanged(const char * /*setting*/, void * userData)
{
    SciterMainWindow * impl = (SciterMainWindow *)userData;

    enum
    {
        maxRememberedFiles = 10
    };

    Stringlist & recentFiles = uiSettings.recentFiles;
    std::string gameFile = SettingsStore::GetInstance().GetString(NXCoreSetting::GameFile);
    for (Stringlist::const_iterator itr = recentFiles.begin(); itr != recentFiles.end(); itr++)
    {
        if (_stricmp(gameFile.c_str(), itr->c_str()) != 0)
        {
            continue;
        }
        recentFiles.erase(itr);
        break;
    }
    recentFiles.insert(recentFiles.begin(), gameFile);
    if (recentFiles.size() > maxRememberedFiles)
    {
        recentFiles.resize(maxRememberedFiles);
    }
    impl->ResetMenu();
    SaveUISetting();
}

void SciterMainWindow::GameNameChanged(const char * /*setting*/, void * userData)
{
    SciterMainWindow * impl = (SciterMainWindow *)userData;

    std::string gameName = SettingsStore::GetInstance().GetString(NXCoreSetting::GameName);
    if (gameName.length() > 0)
    {
        std::string caption;
        caption += gameName;
        caption += " | ";
        caption += impl->m_windowTitle;
        impl->SetCaption(caption);
    }
}

void SciterMainWindow::DisplayedFramesChanged(const char * /*setting*/, void * userData)
{
    SettingsStore & settings = SettingsStore::GetInstance();
    SciterMainWindow * impl = (SciterMainWindow *)userData;

    impl->m_shownFirstFrame = settings.GetBool(NXCoreSetting::DisplayedFrames);
    if (impl->m_shownFirstFrame)
    {
        impl->m_rootElement.PostEvent(EVENT_EMULATION_FIRST_FRAME);
    }
}

void SciterMainWindow::DiskCacheLoadChanged(const char * /*setting*/, void * userData)
{
    SciterMainWindow * impl = static_cast<SciterMainWindow *>(userData);
    SettingsStore & settings = SettingsStore::GetInstance();
    const int stage = settings.GetInt(NXCoreSetting::DiskCacheLoadStage);

    constexpr uint64_t intervalMs = 50;
    const uint64_t now = GetTickCount64();
    const bool neverPosted = (impl->m_lastDiskCacheStatusPostMs == 0);
    const bool stageChanged = (stage != impl->m_lastPostedDiskCacheStage);
    const uint64_t elapsed = neverPosted ? intervalMs : (now - impl->m_lastDiskCacheStatusPostMs);

    if (!neverPosted && !stageChanged && elapsed < intervalMs)
    {
        return;
    }

    impl->m_lastPostedDiskCacheStage = stage;
    impl->m_lastDiskCacheStatusPostMs = now;
    impl->m_rootElement.PostEvent(EVENT_DISK_CACHE_STATUS);
}

void SciterMainWindow::RefreshDiskCacheLoadingText()
{
    SettingsStore & settings = SettingsStore::GetInstance();
    const int stage = settings.GetInt(NXCoreSetting::DiskCacheLoadStage);
    const int current = settings.GetInt(NXCoreSetting::DiskCacheLoadCurrent);
    const int total = settings.GetInt(NXCoreSetting::DiskCacheLoadTotal);

    SciterElement fillEl(m_rootElement.GetElementByID("LoadingProgressFill"));

    std::string text;
    switch (stage)
    {
    case 0:
        text = GetGameTitleLoadingHtml(m_modules.Modules().Systemloader(), "Loading");
        UpdateLoadingProgressBar(fillEl, true, 0, false);
        break;
    case 1:
        text = stdstr_f(
            "<span class=\"loading-verb\">Loading shaders</span> <span class=\"loading-shader-count\">%d / %d</span>",
            current, total);
        if (total > 0)
        {
            const int pct = static_cast<int>((100.0 * static_cast<double>(current)) / static_cast<double>(total));
            UpdateLoadingProgressBar(fillEl, false, pct, true);
        }
        else
        {
            UpdateLoadingProgressBar(fillEl, true, 0, false);
        }
        break;
    case 2:
        text = GetGameTitleLoadingHtml(m_modules.Modules().Systemloader(), "Launching");
        UpdateLoadingProgressBar(fillEl, false, 100, false);
        break;
    default:
        UpdateLoadingProgressBar(fillEl, true, 0, false);
        return;
    }

    SciterElement status(m_rootElement.GetElementByID("loadingText"));
    if (status.IsValid())
    {
        status.SetHTML(reinterpret_cast<const uint8_t *>(text.c_str()), text.size());
    }
    m_sciterUI.UpdateWindow(m_rootElement.GetElementHwnd(true));
}

void SciterMainWindow::HotKeysChanged(const char * /*setting*/, void * userData)
{
    SciterMainWindow * impl = (SciterMainWindow *)userData;
    impl->ResetMenu();
}

void SciterMainWindow::PreventOSSleep()
{
#ifdef _WIN32
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
#elif defined(HAVE_SDL2)
    SDL_DisableScreenSaver();
#endif
}

void SciterMainWindow::AllowOSSleep()
{
#ifdef _WIN32
    SetThreadExecutionState(ES_CONTINUOUS);
#elif defined(HAVE_SDL2)
    SDL_EnableScreenSaver();
#endif
}

void SciterMainWindow::OnOpenFile()
{
    if (m_window == nullptr || !m_modules.IsValid())
    {
        return;
    }

    m_modules.Setup(*this);
    RegisterApplets();

    ISystemloader & loader = m_modules.Modules().Systemloader();
    loader.SelectAndLoad((void *)m_window->GetHandle());
    UpdateEmulationStatusText();
}

void SciterMainWindow::OnInstallFirmwareFromFile()
{
    if (m_window == nullptr || m_emulationRunning || !m_modules.IsValid() || m_firmwareInstallInProgress)
    {
        return;
    }

    Path file;
    const char * filter =
        "Firmware Package (*.dxci, *.zip)\0*.dxci;*.zip\0DXCI (*.dxci)\0*.dxci\0ZIP (*.zip)\0*.zip\0All files (*.*)\0*.*\0";
    if (!file.FileSelect((void *)m_window->GetHandle(), Path(Path::CURRENT_DIRECTORY), filter, true))
    {
        return;
    }

    BeginFirmwareInstall(file);
}

void SciterMainWindow::OnInstallFirmwareFromFolder()
{
    if (m_window == nullptr || m_emulationRunning || !m_modules.IsValid() || m_firmwareInstallInProgress)
    {
        return;
    }

    Path folder;
    folder.BrowseForDirectory((void *)m_window->GetHandle(), "Select folder containing firmware .dnca files");
    if (!folder.DirectoryExists())
    {
        return;
    }

    BeginFirmwareInstall((const char *)folder);
}

void SciterMainWindow::RefreshFirmwareInstallLoading()
{
    SettingsStore & settings = SettingsStore::GetInstance();
    const uint32_t current = static_cast<uint32_t>(settings.GetInt(NXLoaderSetting::FirmwareInstallCurrent));
    const uint32_t total = static_cast<uint32_t>(settings.GetInt(NXLoaderSetting::FirmwareInstallTotal));

    SciterElement fillEl(m_rootElement.GetElementByID("LoadingProgressFill"));
    SciterElement status(m_rootElement.GetElementByID("loadingText"));

    std::string detail;
    if (total > 0)
    {
        detail = stdstr_f("%u / %u files", current, total);
        if (fillEl.IsValid())
        {
            const int pct = static_cast<int>((100.0 * static_cast<double>(current)) / static_cast<double>(total));
            UpdateLoadingProgressBar(fillEl, false, pct, false);
        }
    }
    else if (fillEl.IsValid())
    {
        UpdateLoadingProgressBar(fillEl, true, 0, false);
    }

    if (status.IsValid())
    {
        const std::string text = stdstr_f("<span class=\"loading-verb\">Installing firmware</span> <span class=\"loading-game-name\">%s</span>", HtmlEscapeForHtmlContent(detail).c_str());
        status.SetHTML(reinterpret_cast<const uint8_t *>(text.c_str()), text.size());
    }
    m_sciterUI.UpdateWindow(m_rootElement.GetElementHwnd(true));
}

void SciterMainWindow::StartFirmwareInstallUi()
{
    if (m_firmwareInstallUiActive)
    {
        return;
    }

    m_firmwareInstallUiActive = true;

    SciterElement fillEl(m_rootElement.GetElementByID("LoadingProgressFill"));
    SciterElement status(m_rootElement.GetElementByID("loadingText"));
    if (fillEl.IsValid())
    {
        UpdateLoadingProgressBar(fillEl, true, 0, false);
    }
    if (status.IsValid())
    {
        const std::string text = "<span class=\"loading-verb\">Installing firmware</span>";
        status.SetHTML(reinterpret_cast<const uint8_t *>(text.c_str()), text.size());
    }
    LoadImageToElement(m_rootElement.GetElementByID("LoadingCornerLogo"), {});
    LoadImageToElement(m_rootElement.GetElementByID("LoadingCornerBanner"), {});
    LoadImageToElement(m_rootElement.GetElementByID("LoadingGameIcon"), {});
    SciterElement loadingMain(m_rootElement.FindFirst("#LoadingPanel .loading-main"));
    if (loadingMain.IsValid())
    {
        loadingMain.AddClassName("no-game-icon");
    }
    ShowPanel(Panel::Loading);
    m_sciterUI.UpdateWindow(m_rootElement.GetElementHwnd(true));

    if (!m_emulationRunning)
    {
        ResetMenu();
    }
    m_rootElement.SetTimer(25, (uint32_t *)TIMER_UPDATE_INSTALL_FIRMWARE);
    RefreshFirmwareInstallLoading();
}

void SciterMainWindow::StopFirmwareInstallUi()
{
    if (!m_firmwareInstallUiActive)
    {
        return;
    }

    m_firmwareInstallUiActive = false;
    m_rootElement.SetTimer(0, (uint32_t *)TIMER_UPDATE_INSTALL_FIRMWARE);

    SciterElement loadingMain(m_rootElement.FindFirst("#LoadingPanel .loading-main"));
    if (loadingMain.IsValid())
    {
        loadingMain.RemoveClassName("no-game-icon");
    }

    const EmulationState state = static_cast<EmulationState>(SettingsStore::GetInstance().GetInt(NXCoreSetting::EmulationState));
    if (state == EmulationState::Running && m_shownFirstFrame)
    {
        ShowPanel(Panel::Renderer);
    }
    else if (state == EmulationState::Paused)
    {
        ShowPanel(Panel::Pause);
    }
    else if (state == EmulationState::RomLoaded)
    {
        ShowPanel(Panel::Loading);
        RefreshDiskCacheLoadingText();
    }
    else
    {
        ShowPanel(Panel::RomBrowser);
    }

    UpdateEmulationStatusText();
    if (!m_emulationRunning)
    {
        ResetMenu();
    }
}

void SciterMainWindow::FinishFirmwareInstall()
{
    if (m_firmwareInstallThread.joinable())
    {
        m_firmwareInstallThread.join();
    }
    m_firmwareInstallInProgress = false;
    StopFirmwareInstallUi();
    UpdateEmulationStatusText();
    if (!m_emulationRunning)
    {
        ResetMenu();
    }
}

void SciterMainWindow::BeginFirmwareInstall(const char * utf8_path)
{
    if (utf8_path == nullptr || utf8_path[0] == '\0' || m_firmwareInstallInProgress || !m_modules.IsValid())
    {
        return;
    }

    m_firmwareInstallInProgress = true;
    m_rootElement.PostEvent(EVENT_FIRMWARE_INSTALL_ACTIVE);

    const std::string path = utf8_path;
    m_firmwareInstallThread = std::thread([this, path]() {
        m_modules.Modules().Systemloader().InstallFirmwarePackage(path.c_str());
        m_rootElement.PostEvent(EVENT_FIRMWARE_INSTALL_DONE);
    });
}

void SciterMainWindow::FirmwareInstallTotalChanged(const char * /*setting*/, void * userData)
{
    SciterMainWindow * impl = static_cast<SciterMainWindow *>(userData);
    const int32_t total = SettingsStore::GetInstance().GetInt(NXLoaderSetting::FirmwareInstallTotal);
    if (total > 0)
    {
        impl->m_rootElement.PostEvent(EVENT_FIRMWARE_INSTALL_ACTIVE);
    }
    else if (impl->m_firmwareInstallUiActive)
    {
        impl->m_rootElement.PostEvent(EVENT_FIRMWARE_INSTALL_FINISHED);
    }
}

bool SciterMainWindow::ConfirmCloseEmulator()
{
    if (!m_emulationRunning || !uiSettings.confirmBeforeStopping)
    {
        return true;
    }

    const char * msg = "Are you sure you want to exit the emulator?\n\nAny unsaved progress will be lost.";
    bool exitLocked = m_modules.IsValid() && m_modules.Modules().OperatingSystem().GetExitLocked();
    if (exitLocked)
    {
        msg = "The currently running application has requested NxEmu to not exit.\n\nWould you like to bypass this and close the emulator anyway?";
    }

    return g_notify->Query(msg, "NxEmu - Exit Emulator?") == NotificationResponse::Yes;
}

void SciterMainWindow::OnFileExit()
{
    if (!ConfirmCloseEmulator())
    {
        return;
    }
    m_sciterUI.Stop();
}

void SciterMainWindow::OnStopGame()
{
    if (!m_emulationRunning)
    {
        return;
    }
    if (uiSettings.confirmBeforeStopping)
    {
        const char * msg = "Are you sure you want to stop the emulation?\n\nAny unsaved progress will be lost.";
        bool exitLocked = m_modules.IsValid() && m_modules.Modules().OperatingSystem().GetExitLocked();
        if (exitLocked)
        {
            msg = "The currently running application has requested NxEmu to not exit.\n\nWould you like to bypass this and stop emulation anyway?";
        }

        if (g_notify->Query(msg, "NxEmu - Stop Emulation") == NotificationResponse::No)
        {
            return;
        }
    }
    AllowOSSleep();
    SettingsStore & settings = SettingsStore::GetInstance();
    settings.SetBool(NXCoreSetting::EmulationRunning, false);
}

void SciterMainWindow::OnPauseContinueGame()
{
    if (!m_emulationRunning || !m_modules.IsValid())
    {
        return;
    }
    IOperatingSystem & os = m_modules.Modules().OperatingSystem();
    if (os.IsEmulationPaused())
    {
        PreventOSSleep();
        os.SetEmulationPaused(false);
    }
    else
    {
        os.SetEmulationPaused(true);
        AllowOSSleep();
    }
    ResetMenu();
}

void SciterMainWindow::OnSystemConfig()
{
    ShowConfig(nullptr);
}

void SciterMainWindow::OnInputConfig()
{
    m_inputConfig.reset(new InputConfig(m_sciterUI, m_modules));
    m_inputConfig->Display((void *)m_window->GetHandle());
}

void SciterMainWindow::OnAbout()
{
    m_aboutDialog.reset(new AboutDialog(m_sciterUI));
    m_aboutDialog->Display((void *)m_window->GetHandle());
}

void SciterMainWindow::OnOpenDiscord()
{
    ShellOpen(kNxEmuDiscordUrl);
}

void SciterMainWindow::OnOpenAppDirectory()
{
    const std::string path = Common::FS::GetYuzuPathString(Common::FS::YuzuPath::YuzuDir);
    if (!path.empty())
    {
        ShellOpen(path.c_str(), m_window != nullptr ? (void *)m_window->GetHandle() : nullptr);
    }
}

void SciterMainWindow::OnOpenLogDirectory()
{
    const std::string path = Common::FS::GetYuzuPathString(Common::FS::YuzuPath::LogDir);
    if (!path.empty())
    {
        ShellOpen(path.c_str(), m_window != nullptr ? (void *)m_window->GetHandle() : nullptr);
    }
}

void SciterMainWindow::UpdateEmulationStatusText()
{
    SciterElement statusTextEl(m_rootElement.GetElementByID("StatusText"));
    if (!statusTextEl.IsValid())
    {
        return;
    }

    if (!m_modules.IsValid())
    {
        return;
    }

    IOperatingSystem & operatingSystem = m_modules.Modules().OperatingSystem();
    IVideo & video = m_modules.Modules().Video();
    ISystemloader & loader = m_modules.Modules().Systemloader();
    std::vector<std::string> parts;

    if (m_emulationRunning)
    {
        if (operatingSystem.IsEmulationPaused())
        {
            parts.push_back("Paused");
        }

        const int shaders_building = video.ShadersBuilding();

        if (shaders_building > 0)
        {
            parts.push_back(stdstr_f("Building: %d shader(s)", shaders_building));
        }

        if (m_resolutionUpFactor != 1.0)
        {
            parts.push_back(stdstr_f("Scale: %.0fx", m_resolutionUpFactor));
        }
        PerfStatsResults results = operatingSystem.GetAndResetPerfStats();
        if (!m_useMultiCore)
        {
            if (m_useSpeedLimit)
            {
                if (results.emulation_speed > 0.999 && results.emulation_speed < 1.01)
                {
                    results.emulation_speed = 100.0;
                }
                parts.push_back(stdstr_f("Speed: %.0f / %d", results.emulation_speed * 100.0, m_speedLimit));
            }
            else
            {
                parts.push_back(stdstr_f("Speed: %f", results.emulation_speed * 100.0));
            }
        }
        if (results.average_game_fps != 0)
        {
            parts.push_back(stdstr_f("%.0f FPS (%.2f ms)%s", std::round(results.average_game_fps), std::isnan(results.frametime) ? 0.0 : (results.frametime * 1000.0), m_useSpeedLimit ? "" : " Unlocked"));
        }
    }
    else
    {
        m_rootElement.SetTimer(0, (uint32_t *)TIMER_UPDATE_STATUS);
    }
    const std::string firmware_version = GetInstalledFirmwareDisplayVersion(loader);
    if (!firmware_version.empty())
    {
        parts.push_back("Firmware: " + firmware_version);
    }
    std::string status;
    for (size_t i = 0; i < parts.size(); i++)
    {
        if (i > 0) status += " | ";
        status += parts[i];
    }
    statusTextEl.SetText(status.c_str());
}

const MenuBarAccelerator * SciterMainWindow::HotkeyAccelerator(const char * name)
{
    if (name == nullptr)
    {
        return nullptr;
    }
    HotkeyMap::iterator itr = uiSettings.hotkeys.find(name);
    return itr != uiSettings.hotkeys.end() ? &itr->second : nullptr;
}

const char * SciterMainWindow::IsMenuBarAccelerator(uint32_t keyCode, uint32_t keyboardState)
{
    for (const HotkeyMap::value_type & entry : uiSettings.hotkeys)
    {
        if (AcceleratorMatchesKey(entry.second, keyCode, keyboardState))
        {
            return entry.first.c_str();
        }
    }
    return nullptr;
}

SciterMainWindow::GuiAction SciterMainWindow::HotkeyToGuiAction(const char * hotkeyId)
{
    if (hotkeyId == nullptr)
    {
        return GuiAction::Invalid;
    }
    if (strcmp(hotkeyId, Hotkey::LoadFile) == 0)
    {
        return GuiAction::LoadFile;
    }
    if (strcmp(hotkeyId, Hotkey::Exit) == 0)
    {
        return GuiAction::ExitApplication;
    }
    if (strcmp(hotkeyId, Hotkey::PauseContinue) == 0)
    {
        return GuiAction::PauseOrContinueEmulation;
    }
    if (strcmp(hotkeyId, Hotkey::ToggleDockedMode) == 0)
    {
        return GuiAction::ToggleDockedMode;
    }
    if (strcmp(hotkeyId, Hotkey::StopEmulation) == 0)
    {
        return GuiAction::StopEmulation;
    }
    if (strcmp(hotkeyId, Hotkey::Configure) == 0)
    {
        return GuiAction::OpenSystemConfiguration;
    }
    if (strcmp(hotkeyId, Hotkey::Controllers) == 0)
    {
        return GuiAction::OpenControllersDialog;
    }
    if (strcmp(hotkeyId, Hotkey::HideUi) == 0)
    {
        return GuiAction::ToggleHideUi;
    }
    return GuiAction::Invalid;
}

void SciterMainWindow::OnToggleDockedMode()
{
    SettingsStore & store = SettingsStore::GetInstance();
    const bool docked = store.GetInt(NXOsSetting::DockedMode) == static_cast<int32_t>(Settings::DockedMode::Docked);
    store.SetInt(NXOsSetting::DockedMode, static_cast<int32_t>(docked ? Settings::DockedMode::Handheld : Settings::DockedMode::Docked));
}

void SciterMainWindow::OnToggleStartGamesInFullscreen()
{
    if (m_emulationRunning)
    {
        return;
    }
    uiSettings.startGamesInFullscreen = !uiSettings.startGamesInFullscreen;
    SaveUISetting();
    ResetMenu();
}

void SciterMainWindow::OnToggleStartGamesWithUiHidden()
{
    if (m_emulationRunning)
    {
        return;
    }
    uiSettings.startGamesWithUiHidden = !uiSettings.startGamesWithUiHidden;
    SaveUISetting();
    ResetMenu();
}

void SciterMainWindow::OnRecetGame(uint32_t fileIndex)
{
    Stringlist & recentFiles = uiSettings.recentFiles;
    if (m_modules.IsValid() && fileIndex < recentFiles.size())
    {
        LoadGame(recentFiles[fileIndex].c_str());
    }
}

void SciterMainWindow::OnWindowDestroy(HWINDOW /*hWnd*/)
{
    m_WebBrowser.DetachWindow();
    m_sciterUI.Stop();
}

bool SciterMainWindow::OnWindowCloseRequest(HWINDOW /*hWnd*/)
{
    return ConfirmCloseEmulator();
}

void SciterMainWindow::OnMenuItem(int32_t id, SCITER_ELEMENT /*item*/)
{
    OnGuiAction(static_cast<GuiAction>(id));
}

void SciterMainWindow::OnGuiAction(GuiAction action)
{
    if (action >= GuiAction::RecentFileMenuFirst && action <= GuiAction::RecentFileMenuLast)
    {
        OnRecetGame(static_cast<uint32_t>(action) - static_cast<uint32_t>(GuiAction::RecentFileMenuFirst));
        return;
    }

    switch (action)
    {
    case GuiAction::LoadFile:
        OnOpenFile();
        break;
    case GuiAction::InstallFirmwareFromFile:
        OnInstallFirmwareFromFile();
        break;
    case GuiAction::InstallFirmwareFromFolder:
        OnInstallFirmwareFromFolder();
        break;
    case GuiAction::ExitApplication:
        OnFileExit();
        break;
    case GuiAction::PauseOrContinueEmulation:
        OnPauseContinueGame();
        break;
    case GuiAction::StopEmulation:
        OnStopGame();
        break;
    case GuiAction::OpenControllersDialog:
        OnInputConfig();
        break;
    case GuiAction::OpenSystemConfiguration:
        OnSystemConfig();
        break;
    case GuiAction::OpenAboutDialog:
        OnAbout();
        break;
    case GuiAction::OpenDiscord:
        OnOpenDiscord();
        break;
    case GuiAction::OpenAppDirectory:
        OnOpenAppDirectory();
        break;
    case GuiAction::OpenLogDirectory:
        OnOpenLogDirectory();
        break;
    case GuiAction::ToggleFullscreen:
        ToggleFullscreen();
        break;
    case GuiAction::ToggleStartGamesInFullscreen:
        OnToggleStartGamesInFullscreen();
        break;
    case GuiAction::ToggleStartGamesWithUiHidden:
        OnToggleStartGamesWithUiHidden();
        break;
    case GuiAction::ToggleHideUi:
        ToggleHideUi();
        break;
    case GuiAction::ToggleDockedMode:
        OnToggleDockedMode();
        break;
    case GuiAction::ResetWindowSize720p:
        ResetWindowSize(1280U, 720U);
        break;
    case GuiAction::ResetWindowSize900p:
        ResetWindowSize(1600U, 900U);
        break;
    case GuiAction::ResetWindowSize1080p:
        ResetWindowSize(1920U, 1080U);
        break;
    case GuiAction::Invalid:
    default:
        break;
    }
}

void * SciterMainWindow::RenderSurface() const
{
    return m_renderWindow;
}

float SciterMainWindow::PixelRatio() const
{
    HWND hwnd = nullptr;
    if (m_renderWindow != nullptr)
    {
        hwnd = (HWND)m_renderWindow;
    }
    else if (m_window != nullptr)
    {
        hwnd = (HWND)m_window->GetHandle();
    }

    typedef UINT(WINAPI * PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow pGetDpiForWindow = reinterpret_cast<PFN_GetDpiForWindow>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));

    if (hwnd != nullptr && pGetDpiForWindow != nullptr)
    {
        UINT dpi = pGetDpiForWindow(hwnd);
        if (dpi != 0)
        {
            return static_cast<float>(dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
        }
    }

    HDC hdc = ::GetDC(hwnd);
    if (hdc != nullptr)
    {
        int dpi = ::GetDeviceCaps(hdc, LOGPIXELSX);
        ::ReleaseDC(hwnd, hdc);
        if (dpi > 0)
        {
            return static_cast<float>(dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
        }
    }
    return 1.0f;
}

bool SciterMainWindow::OnKeyDown(SCITER_ELEMENT /*element*/, SCITER_ELEMENT /*item*/, SciterKeys keyCode, uint32_t keyboardState)
{
    const char * hotkeyId = IsMenuBarAccelerator((uint32_t)keyCode, keyboardState);
    if (hotkeyId != nullptr)
    {
        if (strcmp(hotkeyId, Hotkey::ExitFullscreen) == 0 && m_win32Fullscreen->active)
        {
            m_win32Fullscreen->pendingSwallowKeyUp = (uint32_t)keyCode;
            OnGuiAction(GuiAction::ToggleFullscreen);
            return true;
        }
        if (strcmp(hotkeyId, Hotkey::Fullscreen) == 0)
        {
            const bool allowFullscreen = m_emulationRunning || (m_win32Fullscreen != nullptr && m_win32Fullscreen->active);
            if (allowFullscreen)
            {
                m_win32Fullscreen->pendingSwallowKeyUp = (uint32_t)keyCode;
                OnGuiAction(GuiAction::ToggleFullscreen);
                return true;
            }
        }
        if (strcmp(hotkeyId, Hotkey::HideUi) == 0)
        {
            m_win32Fullscreen->pendingSwallowKeyUp = (uint32_t)keyCode;
            OnGuiAction(GuiAction::ToggleHideUi);
            return true;
        }
        const GuiAction fromKey = HotkeyToGuiAction(hotkeyId);
        if (fromKey != GuiAction::Invalid)
        {
            OnGuiAction(fromKey);
            return true;
        }
    }
    if (m_modules.IsValid())
    {
        IOperatingSystem & operatingSystem = m_modules.Modules().OperatingSystem();
        int keyIndex = SciterKeyToSwitchKey(keyCode);
        if (keyIndex != 0)
        {
            operatingSystem.KeyboardKeyPress(0, keyIndex, SciterKeyToVKCode(keyCode));
        }
    }
    return false;
}

bool SciterMainWindow::OnKeyUp(SCITER_ELEMENT /*element*/, SCITER_ELEMENT /*item*/, SciterKeys keyCode, uint32_t keyboardState)
{
    if (m_win32Fullscreen != nullptr && m_win32Fullscreen->pendingSwallowKeyUp != 0 && (uint32_t)keyCode == m_win32Fullscreen->pendingSwallowKeyUp)
    {
        m_win32Fullscreen->pendingSwallowKeyUp = 0;
        return true;
    }
    if (IsMenuBarAccelerator((uint32_t)keyCode, keyboardState) != nullptr)
    {
        return true;
    }
    if (m_modules.IsValid())
    {
        IOperatingSystem & operatingSystem = m_modules.Modules().OperatingSystem();
        int keyIndex = SciterKeyToSwitchKey(keyCode);
        if (keyIndex != 0)
        {
            operatingSystem.KeyboardKeyRelease(0, keyIndex, SciterKeyToVKCode(keyCode));
        }
    }
    return false;
}

bool SciterMainWindow::OnKeyChar(SCITER_ELEMENT /*element*/, SCITER_ELEMENT /*item*/, SciterKeys /*keyCode*/, uint32_t /*keyboardState*/)
{
    return false;
}

bool SciterMainWindow::OnSizeChanged(SCITER_ELEMENT elem)
{
    SciterElement rootElement(m_window->GetRootElement());
    if (elem == rootElement.GetElementByID("MainContents"))
    {
        LayoutRenderWindow();
    }
    return false;
}

void SciterMainWindow::LayoutRenderWindow()
{
    if (m_renderWindow == nullptr)
    {
        return;
    }
    SciterElement mainContents(m_rootElement.GetElementByID("MainContents"));
    if (!mainContents.IsValid())
    {
        return;
    }
    SciterElement::RECT rect = mainContents.GetLocation();
    uint32_t width = rect.right - rect.left;
    uint32_t height = rect.bottom - rect.top;
    MoveWindow((HWND)m_renderWindow, rect.left, rect.top, width, height, false);
    if (m_modules.IsValid())
    {
        IVideo & video = m_modules.Modules().Video();
        video.UpdateFramebufferLayout(width, height);
    }
}

void SciterMainWindow::UpdateLoadingScreenDetails()
{
    IRomInfoPtr info(m_modules.Modules().Systemloader().LoadedRomInfo());
    if (!info)
    {
        return;
    }

    std::vector<uint8_t> logoData, bannerData, iconData;
    uint32_t sz = 0;
    if (info->ReadLogo(nullptr, &sz) == LoaderResultStatus::Success && sz > 0)
    {
        logoData.resize(sz);
        if (info->ReadLogo(logoData.data(), &sz) != LoaderResultStatus::Success)
        {
            logoData.clear();
        }
    }

    sz = 0;
    if (info->ReadBanner(nullptr, &sz) == LoaderResultStatus::Success && sz > 0)
    {
        bannerData.resize(sz);
        if (info->ReadBanner(bannerData.data(), &sz) != LoaderResultStatus::Success)
        {
            bannerData.clear();
        }
    }

    sz = 0;
    if (info->ReadIcon(nullptr, &sz) == LoaderResultStatus::Success && sz > 0)
    {
        iconData.resize(sz);
        if (info->ReadIcon(iconData.data(), &sz) != LoaderResultStatus::Success)
        {
            iconData.clear();
        }
    }

    LoadImageToElement(m_rootElement.GetElementByID("LoadingCornerLogo"), logoData);
    LoadImageToElement(m_rootElement.GetElementByID("LoadingCornerBanner"), bannerData);
    LoadImageToElement(m_rootElement.GetElementByID("LoadingGameIcon"), iconData);

    SciterElement loadingMain(m_rootElement.FindFirst("#LoadingPanel .loading-main"));
    if (loadingMain.IsValid())
    {
        loadingMain.RemoveClassName("no-game-icon");
    }
}

void SciterMainWindow::UpdateUIVisibility()
{
    const bool hide = m_hideUi || (m_win32Fullscreen != nullptr && m_win32Fullscreen->active);

    std::array<SciterElement, 3> shellPanels = {{
        m_rootElement.FindFirst("header"),
        m_rootElement.GetElementByID("MainMenu"),
        m_rootElement.GetElementByID("StatusBar"),
    }};

    for (SciterElement & panel : shellPanels)
    {
        if (!panel.IsValid())
        {
            continue;
        }
        if (hide)
        {
            panel.AddClassName("nx-fullscreen-hide");
        }
        else
        {
            panel.RemoveClassName("nx-fullscreen-hide");
        }
    }
}

void SciterMainWindow::ToggleHideUi()
{
    if (!m_emulationRunning)
    {
        return;
    }
    m_hideUi = !m_hideUi;
    UpdateUIVisibility();
    m_sciterUI.UpdateWindow(m_rootElement.GetElementHwnd(true));
    SciterElement mainContents(m_rootElement.GetElementByID("MainContents"));
    if (mainContents.IsValid())
    {
        mainContents.Update(true);
    }
    LayoutRenderWindow();
    ResetMenu();
}

void SciterMainWindow::ToggleFullscreen()
{
    if (!m_win32Fullscreen)
    {
        return;
    }
    if (m_win32Fullscreen->active)
    {
        ExitFullscreen();
    }
    else if (m_emulationRunning)
    {
        EnterFullscreen();
    }
}

void SciterMainWindow::EnterFullscreen()
{
    if (m_window == nullptr || !m_win32Fullscreen || m_win32Fullscreen->active || !m_emulationRunning)
    {
        return;
    }
    HWND hwnd = (HWND)m_window->GetHandle();
    Win32FullscreenState & fs = *m_win32Fullscreen;
    fs.placement.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(hwnd, &fs.placement))
    {
        return;
    }
    fs.savedStyle = GetWindowLongPtr(hwnd, GWL_STYLE);
    fs.savedExStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);

    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(MONITORINFO)};
    if (!GetMonitorInfo(hMon, &mi))
    {
        return;
    }

    const int w = mi.rcMonitor.right - mi.rcMonitor.left;
    const int h = mi.rcMonitor.bottom - mi.rcMonitor.top;

    LONG_PTR style = fs.savedStyle;
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER);
    SetWindowLongPtr(hwnd, GWL_STYLE, style);

    SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    fs.active = true;
    UpdateUIVisibility();
    m_sciterUI.UpdateWindow(m_rootElement.GetElementHwnd(true));
    SciterElement mainContents(m_rootElement.GetElementByID("MainContents"));
    if (mainContents.IsValid())
    {
        mainContents.Update(true);
    }
    LayoutRenderWindow();
}

void SciterMainWindow::ExitFullscreen()
{
    if (m_window == nullptr || !m_win32Fullscreen || !m_win32Fullscreen->active)
    {
        return;
    }
    HWND hwnd = (HWND)m_window->GetHandle();
    Win32FullscreenState & fs = *m_win32Fullscreen;

    SetWindowLongPtr(hwnd, GWL_STYLE, fs.savedStyle);
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, fs.savedExStyle);
    SetWindowPlacement(hwnd, &fs.placement);

    fs.active = false;
    UpdateUIVisibility();
    m_sciterUI.UpdateWindow(m_rootElement.GetElementHwnd(true));
    SciterElement mainContents(m_rootElement.GetElementByID("MainContents"));
    if (mainContents.IsValid())
    {
        mainContents.Update(true);
    }
    LayoutRenderWindow();
}

void SciterMainWindow::ResetWindowSize(uint32_t nominal_width, uint32_t nominal_height)
{
    if (m_window == nullptr || nominal_width == 0 || nominal_height == 0)
    {
        return;
    }
    if (m_win32Fullscreen && m_win32Fullscreen->active)
    {
        ExitFullscreen();
    }
    HWND hwnd = (HWND)m_window->GetHandle();
    SciterElement mainContents(m_rootElement.GetElementByID("MainContents"));
    if (!mainContents.IsValid())
    {
        return;
    }
    SciterElement::RECT contentRect = mainContents.GetLocation();
    const int32_t curW = contentRect.right - contentRect.left;
    const int32_t curH = contentRect.bottom - contentRect.top;
    if (curW <= 0 || curH <= 0)
    {
        return;
    }

    const float window_aspect_ratio = (float)nominal_height / (float)nominal_width;
    float emulation_aspect_ratio = 720.0f / 1280.0f;
    switch ((AspectRatio)SettingsStore::GetInstance().GetInt(NXVideoSetting::AspectRatio))
    {
    case AspectRatio::R16_9:
        emulation_aspect_ratio = 720.0f / 1280.0f;
        break;
    case AspectRatio::R4_3:
        emulation_aspect_ratio = 3.0f / 4.0f;
        break;
    case AspectRatio::R21_9:
        emulation_aspect_ratio = 9.0f / 21.0f;
        break;
    case AspectRatio::R16_10:
        emulation_aspect_ratio = 10.0f / 16.0f;
        break;
    case AspectRatio::Stretch:
        emulation_aspect_ratio = window_aspect_ratio;
        break;
    default:
        break;
    }
    const uint32_t targetH = nominal_height;
    const uint32_t targetW = (uint32_t)(std::lround((double)nominal_height / (double)emulation_aspect_ratio));

    RECT wr{};
    if (!GetWindowRect(hwnd, &wr))
    {
        return;
    }
    const int outerW = wr.right - wr.left;
    const int outerH = wr.bottom - wr.top;
    const int deltaW = static_cast<int>(targetW) - curW;
    const int deltaH = static_cast<int>(targetH) - curH;

    SetWindowPos(hwnd, nullptr, 0, 0, outerW + deltaW, outerH + deltaH, SWP_NOMOVE | SWP_NOZORDER);
    m_sciterUI.UpdateWindow(m_rootElement.GetElementHwnd(true));
    SciterElement mc(m_rootElement.GetElementByID("MainContents"));
    if (mc.IsValid())
    {
        mc.Update(true);
    }
    LayoutRenderWindow();
}

bool SciterMainWindow::OnClick(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t /*reason*/)
{
    SciterElement rootElement(m_window->GetRootElement());
    if (source == rootElement.GetElementByID("dockedMode"))
    {
        OnToggleDockedMode();
    }
    else if (element == rootElement.GetElementByID("renderer"))
    {
        SettingsStore & settings = SettingsStore::GetInstance();
        RendererBackend graphicsAPI = (RendererBackend)settings.GetInt(NXVideoSetting::GraphicsAPI);
        if (graphicsAPI == RendererBackend::Vulkan)
        {
            graphicsAPI = RendererBackend::OpenGL;
        }
        else if (graphicsAPI == RendererBackend::OpenGL)
        {
            graphicsAPI = RendererBackend::Vulkan;
        }
        settings.SetInt(NXVideoSetting::GraphicsAPI, (int32_t)graphicsAPI);
        stdstr_f text("%s", RendererBackendLabel((RendererBackend)settings.GetInt(NXVideoSetting::GraphicsAPI)));
        SciterElement(source).SetHTML((const uint8_t *)text.c_str(), text.size());
        if (m_modules.IsValid())
        {
            m_modules.FlushSettings();
        }
    }
    else if (source == rootElement.GetElementByID("volume"))
    {
        SettingsStore & settings = SettingsStore::GetInstance();
        settings.SetBool(NXOsSetting::AudioMuted, !settings.GetBool(NXOsSetting::AudioMuted));
    }
    else if (element == rootElement.GetElementByID("volumePopupBtn"))
    {
        SciterElement volumePopup(rootElement.GetElementByID("VolumePopup"));
        if (volumePopup.IsValid())
        {
            const bool popupOpen = (volumePopup.GetState() & SciterElement::STATE_POPUP) != 0;
            if (!popupOpen)
            {
                m_sciterUI.PopupShow(rootElement.GetElementByID("VolumePopup"), rootElement.GetElementByID("volumeAnchor"), 8);
                SciterElement(rootElement.GetElementByID("volumePopupBtn")).AddClassName("open");
            }
            else
            {
                m_sciterUI.PopupHide(m_rootElement.GetElementByID("VolumePopup"));
                SciterElement(rootElement.GetElementByID("volumePopupBtn")).RemoveClassName("open");
            }
        }
    }
    return true;
}

bool SciterMainWindow::OnStateChange(SCITER_ELEMENT elem, uint32_t /*eventReason*/, void * /*data*/)
{
    SciterElement rootElement(m_window->GetRootElement());
    if (rootElement.GetElementByID("audioVolume") == elem)
    {
        SciterValue value = SciterElement(elem).GetValue();
        if (value.isInt())
        {
            SettingsStore & settings = SettingsStore::GetInstance();
            settings.SetBool(NXOsSetting::AudioMuted, false);
            settings.SetInt(NXOsSetting::AudioVolume, value.GetValueInt());
            if (m_modules.IsValid())
            {
                m_modules.FlushSettings();
            }
        }
    }
    return false;
}

void SciterMainWindow::SettingChanged(const char * setting, void * userData)
{
    SciterMainWindow * impl = (SciterMainWindow *)userData;
    if (strcmp(setting, NXOsSetting::AudioMuted) == 0 || strcmp(setting, NXOsSetting::AudioVolume) == 0)
    {
        impl->UpdateStatusWidgets();
    }
    else if (strcmp(setting, NXVideoSetting::ResolutionUpFactor) == 0)
    {
        impl->m_resolutionUpFactor = SettingsStore::GetInstance().GetFloat(NXVideoSetting::ResolutionUpFactor);
    }
    else if (strcmp(setting, NXOsSetting::UseMultiCore) == 0)
    {
        impl->m_useMultiCore = SettingsStore::GetInstance().GetBool(NXOsSetting::UseMultiCore);
    }
    else if (strcmp(setting, NXOsSetting::UseSpeedLimit) == 0)
    {
        impl->m_useSpeedLimit = SettingsStore::GetInstance().GetBool(NXOsSetting::UseSpeedLimit);
    }
    else if (strcmp(setting, NXOsSetting::SpeedLimit) == 0)
    {
        impl->m_speedLimit = SettingsStore::GetInstance().GetInt(NXOsSetting::SpeedLimit);
    }
    else if (strcmp(setting, NXOsSetting::DockedMode) == 0)
    {
        impl->UpdateStatusWidgets();
        impl->LayoutRenderWindow();
    }
}

bool SciterMainWindow::OnTimer(SCITER_ELEMENT /*element*/, uint32_t * timerId)
{
    if (timerId == (uint32_t *)TIMER_UPDATE_UI)
    {
        SciterElement mainContents(m_rootElement.GetElementByID("MainContents"));
        if (mainContents.IsValid())
        {
            mainContents.Update(false);
            m_sciterUI.UpdateWindow(mainContents.GetElementHwnd(true));
        }
        return false;
    }
    else if (timerId == (uint32_t *)TIMER_UPDATE_INPUT)
    {
        UpdateInputDrivers();
    }
    else if (timerId == (uint32_t *)TIMER_UPDATE_STATUS)
    {
        UpdateEmulationStatusText();
    }
    else if (timerId == (uint32_t *)TIMER_UPDATE_INSTALL_FIRMWARE)
    {
        if (m_firmwareInstallUiActive)
        {
            RefreshFirmwareInstallLoading();
        }
    }
    return true;
}

bool SciterMainWindow::OnEvent(SCITER_ELEMENT element, SCITER_ELEMENT /*source*/, uint32_t event_code, uint64_t /*reason*/)
{
    if (event_code == static_cast<uint32_t>(SciterBehaviorEvent::PopupDismissed) && m_window != nullptr)
    {
        SciterElement rootElement(m_window->GetRootElement());
        const SciterElement volumePopupRoot = rootElement.GetElementByID("VolumePopup");
        if (rootElement.IsValid() && volumePopupRoot.IsValid())
        {
            for (SciterElement walk(element); walk.IsValid(); walk = walk.GetParent())
            {
                if (walk == volumePopupRoot)
                {
                    SciterElement btn(rootElement.GetElementByID("volumePopupBtn"));
                    if (btn.IsValid())
                    {
                        btn.RemoveClassName("open");
                    }
                    break;
                }
            }
        }
        return false;
    }
    if (event_code == EVENT_EMULATION_LOADING)
    {
        ShowPanel(Panel::Loading);
    }
    else if (event_code == EVENT_EMULATION_RUNNING)
    {
        PreventOSSleep();
        m_rootElement.SetTimer(500, (uint32_t *)TIMER_UPDATE_STATUS);
    }
    else if (event_code == EVENT_EMULATION_STOPPED)
    {
        m_shownFirstFrame = false;
        ShowPanel(Panel::RomBrowser);
    }
    else if (event_code == EVENT_EMULATION_FIRST_FRAME)
    {
        SettingsStore & settings = SettingsStore::GetInstance();
        if (settings.GetBool(NXCoreSetting::DisplayedFrames))
        {
            ShowPanel(Panel::Renderer);
        }
    }
    else if (event_code == EVENT_DISK_CACHE_STATUS)
    {
        RefreshDiskCacheLoadingText();
    }
    else if (event_code == EVENT_FIRMWARE_INSTALL_ACTIVE)
    {
        StartFirmwareInstallUi();
    }
    else if (event_code == EVENT_FIRMWARE_INSTALL_FINISHED)
    {
        StopFirmwareInstallUi();
    }
    else if (event_code == EVENT_FIRMWARE_INSTALL_DONE)
    {
        FinishFirmwareInstall();
    }
    return false;
}
