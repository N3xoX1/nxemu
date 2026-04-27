#include "sciter_main_window.h"
#include "settings/input_config.h"
#include "settings/system_config.h"
#include "settings/ui_settings.h"
#include "user_interface/key_mappings.h"
#include <common/std_string.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/version.h>
#include <nxemu-module-spec/system_loader.h>
#include <nxemu-module-spec/video.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu-video/video_settings_identifiers.h>
#include <nxemu/settings/ui_identifiers.h>
#include <sciter_element.h>
#include <widgets/menubar.h>
#include <yuzu_common/settings.h>
#include <yuzu_common/settings_input.h>

#include <Windows.h>
#include <cmath>

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

/** Matches Layout::EmulationAspectRatio (yuzu_video_core) for window-size reset; inlined to avoid a link dep. */
static float EmulationAspectRatioForWindowReset(float window_aspect_ratio)
{
    using Settings::AspectRatio;
    const auto aspect = static_cast<AspectRatio>(Settings::values.aspect_ratio.GetValue());
    switch (aspect)
    {
    case AspectRatio::R16_9:
        return 720.0f / 1280.0f;
    case AspectRatio::R4_3:
        return 3.0f / 4.0f;
    case AspectRatio::R21_9:
        return 9.0f / 21.0f;
    case AspectRatio::R16_10:
        return 10.0f / 16.0f;
    case AspectRatio::Stretch:
        return window_aspect_ratio;
    default:
        return 720.0f / 1280.0f;
    }
}
} // namespace

SciterMainWindow::SciterMainWindow(ISciterUI & sciterUI, const char * windowTitle) :
    m_sciterUI(sciterUI),
    m_window(nullptr),
    m_renderWindow(nullptr),
    m_windowTitle(windowTitle),
    m_emulationRunning(false),
    m_win32Fullscreen(std::make_unique<Win32FullscreenState>())
{
    SettingsStore & settings = SettingsStore::GetInstance();
    settings.RegisterCallback(NXCoreSetting::EmulationRunning, SciterMainWindow::EmulationRunning, this);
    settings.RegisterCallback(NXCoreSetting::EmulationState, SciterMainWindow::EmulationStateChanged, this);
    settings.RegisterCallback(NXCoreSetting::GameFile, SciterMainWindow::GameFileChanged, this);
    settings.RegisterCallback(NXCoreSetting::GameName, SciterMainWindow::GameNameChanged, this);
    settings.RegisterCallback(NXCoreSetting::DisplayedFrames, SciterMainWindow::DisplayedFramesChanged, this);
    settings.RegisterCallback(NXCoreSetting::DiskCacheLoadTick, SciterMainWindow::DiskCacheLoadChanged, this);
    settings.RegisterCallback(NXOsSetting::AudioMuted, SciterMainWindow::SettingChanged, this);
    settings.RegisterCallback(NXOsSetting::AudioVolume, SciterMainWindow::SettingChanged, this);
    settings.RegisterCallback(NXOsSetting::ResolutionUpFactor, SciterMainWindow::SettingChanged, this);
    settings.RegisterCallback(NXOsSetting::SpeedLimit, SciterMainWindow::SettingChanged, this);
    settings.RegisterCallback(NXOsSetting::UseMultiCore, SciterMainWindow::SettingChanged, this);
    settings.RegisterCallback(NXOsSetting::UseSpeedLimit, SciterMainWindow::SettingChanged, this);
    settings.RegisterCallback(NXUISetting::Hotkeys, SciterMainWindow::HotKeysChanged, this);

    m_useMultiCore = settings.GetBool(NXOsSetting::UseMultiCore);
    m_useSpeedLimit = settings.GetBool(NXOsSetting::UseSpeedLimit);
    m_speedLimit = settings.GetBool(NXOsSetting::SpeedLimit);
}

SciterMainWindow::~SciterMainWindow()
{
    SettingsStore & settings = SettingsStore::GetInstance();
    settings.UnregisterCallback(NXCoreSetting::EmulationRunning, SciterMainWindow::EmulationRunning, this);
    settings.UnregisterCallback(NXCoreSetting::EmulationState, SciterMainWindow::EmulationStateChanged, this);
    settings.UnregisterCallback(NXCoreSetting::GameFile, SciterMainWindow::GameFileChanged, this);
    settings.UnregisterCallback(NXCoreSetting::GameName, SciterMainWindow::GameNameChanged, this);
    settings.UnregisterCallback(NXCoreSetting::DisplayedFrames, SciterMainWindow::DisplayedFramesChanged, this);
    settings.UnregisterCallback(NXCoreSetting::DiskCacheLoadTick, SciterMainWindow::DiskCacheLoadChanged, this);
    settings.UnregisterCallback(NXOsSetting::AudioVolume, SciterMainWindow::SettingChanged, this);
    settings.UnregisterCallback(NXOsSetting::ResolutionUpFactor, SciterMainWindow::SettingChanged, this);
    settings.UnregisterCallback(NXOsSetting::SpeedLimit, SciterMainWindow::SettingChanged, this);
    settings.UnregisterCallback(NXOsSetting::UseMultiCore, SciterMainWindow::SettingChanged, this);
    settings.UnregisterCallback(NXOsSetting::UseSpeedLimit, SciterMainWindow::SettingChanged, this);
    settings.UnregisterCallback(NXUISetting::Hotkeys, SciterMainWindow::HotKeysChanged, this);

    m_systemConfig.reset(nullptr);
    m_inputConfig.reset(nullptr);

    settings.SetBool(NXCoreSetting::ShuttingDown, true);
    if (m_romBrowser)
    {
        m_romBrowser->ClearItems();
    }
    m_modules.ShutDown();
}

void SciterMainWindow::ResetMenu()
{
    if (m_menuBar == nullptr)
    {
        return;
    }

    MenuBarItemList mainTitleMenu;
    MenuBarItemList fileMenu;
    fileMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::LoadFile), "&Load File...", nullptr, HotkeyAccelerator(Hotkey::LoadFile)));

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
        fileMenu.emplace_back(MenuBarItem::SUB_MENU, "&Recent File", &RecentFileMenu);
    }

    fileMenu.push_back(MenuBarItem(MenuBarItem::SPLITER));
    fileMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::ExitApplication), "E&xit", nullptr, HotkeyAccelerator(Hotkey::Exit)));
    mainTitleMenu.push_back(MenuBarItem(MenuBarItem::SUB_MENU, "&File", &fileMenu));

    MenuBarItemList systemMenu;
    if (m_emulationRunning)
    {
        bool paused = false;
        if (m_modules.IsValid())
        {
            paused = m_modules.Modules().OperatingSystem().IsEmulationPaused();
        }
        systemMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::PauseOrContinueEmulation),paused ? "Continue" : "Pause",nullptr, HotkeyAccelerator(Hotkey::PauseContinue)));
        systemMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::StopEmulation), "&Stop"));
        mainTitleMenu.push_back(MenuBarItem(MenuBarItem::SUB_MENU, "&System", &systemMenu));
    }

    MenuBarItemList viewMenu;
    viewMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::ToggleFullscreen),"&Fullscreen",nullptr,HotkeyAccelerator(Hotkey::Fullscreen)));
    MenuBarItemList resetWindowSizeMenu;
    resetWindowSizeMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::ResetWindowSize720p),"Reset Window Size to 720p"));
    resetWindowSizeMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::ResetWindowSize900p),"Reset Window Size to 900p"));
    resetWindowSizeMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::ResetWindowSize1080p),"Reset Window Size to 1080p"));
    viewMenu.push_back(MenuBarItem(MenuBarItem::SUB_MENU, "Reset Window Size", &resetWindowSizeMenu));
    mainTitleMenu.push_back(MenuBarItem(MenuBarItem::SUB_MENU, "&View", &viewMenu));

    MenuBarItemList optionsMenu;
    optionsMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::OpenControllersDialog), "&Controllers..."));
    optionsMenu.push_back(MenuBarItem(static_cast<int32_t>(GuiAction::OpenSystemConfiguration), "Confi&gure..."));
    mainTitleMenu.push_back(MenuBarItem(MenuBarItem::SUB_MENU, "&Options", &optionsMenu));

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
    UpdateStatusbar();

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

    SciterElement romBrowserPanel = m_rootElement.GetElementByID("RomBrowserPanel");
    if (romBrowserPanel.IsValid())
    {
        romBrowserPanel.SetStyleAttribute("display", "block");
    }

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

    ISystemloader & loader = m_modules.Modules().Systemloader();
    loader.LoadRom(path);
}

void SciterMainWindow::UpdateStatusbar()
{
    SettingsStore & settings = SettingsStore::GetInstance();
    SciterElement renderer(m_rootElement.GetElementByID("renderer"));
    if (renderer.IsValid())
    {
        stdstr_f text("%s", Settings::CanonicalizeEnum((Settings::RendererBackend)settings.GetInt(NXVideoSetting::GraphicsAPI)).c_str());
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

    UpdatePausePanel();
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
}

void SciterMainWindow::ApplyEmulationLoadingUi()
{
    SciterElement romBrowserPanel(m_rootElement.GetElementByID("RomBrowserPanel"));
    if (romBrowserPanel.IsValid())
    {
        romBrowserPanel.SetStyleAttribute("display", "none");
    }
    SciterElement loadingPanel(m_rootElement.GetElementByID("LoadingPanel"));
    if (loadingPanel.IsValid())
    {
        loadingPanel.SetStyleAttribute("display", "block");
    }
    m_sciterUI.UpdateWindow(m_rootElement.GetElementHwnd(true));
}

void SciterMainWindow::EmulationStateChanged(const char * /*setting*/, void * userData)
{
    SciterMainWindow * impl = (SciterMainWindow *)userData;
    EmulationState state = (EmulationState)SettingsStore::GetInstance().GetInt(NXCoreSetting::EmulationState);

    if (state == EmulationState::LoadingRom || state == EmulationState::Starting)
    {
        impl->ApplyEmulationLoadingUi();
    }
    else if (state == EmulationState::Running)
    {
        impl->m_rootElement.PostEvent(EVENT_EMULATION_RUNNING);
        impl->ResetMenu();
    }
    else if (state == EmulationState::Paused)
    {
        impl->ResetMenu();
    }
    else if (state == EmulationState::Stopped)
    {
        impl->m_rootElement.PostEvent(EVENT_EMULATION_STOPPED);
    }
    impl->UpdatePausePanel();
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
    SciterMainWindow * impl = (SciterMainWindow *)userData;
    impl->m_rootElement.PostEvent(EVENT_EMULATION_FIRST_FRAME);
}

void SciterMainWindow::DiskCacheLoadChanged(const char * /*setting*/, void * userData)
{
    SciterMainWindow * impl = static_cast<SciterMainWindow *>(userData);
    impl->m_rootElement.PostEvent(EVENT_DISK_CACHE_STATUS);
}

void SciterMainWindow::RefreshDiskCacheLoadingText()
{
    SettingsStore & settings = SettingsStore::GetInstance();
    const int stage = settings.GetInt(NXCoreSetting::DiskCacheLoadStage);
    const int current = settings.GetInt(NXCoreSetting::DiskCacheLoadCurrent);
    const int total = settings.GetInt(NXCoreSetting::DiskCacheLoadTotal);

    std::string text;
    switch (stage)
    {
    case 0:
        text = "Loading...";
        break;
    case 1:
        text = stdstr_f("Loading shaders %d / %d", current, total);
        break;
    case 2:
        text = "Launching...";
        break;
    default:
        return;
    }

    SciterElement status(m_rootElement.GetElementByID("LoadingStatusText"));
    if (status.IsValid())
    {
        status.SetText(text.c_str());
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
    if (m_modules.IsValid())
    {
        m_modules.Setup(*this);

        ISystemloader & loader = m_modules.Modules().Systemloader();
        loader.SelectAndLoad((void *)m_window->GetHandle());
    }
}

void SciterMainWindow::OnFileExit()
{
    m_sciterUI.Stop();
}

void SciterMainWindow::OnStopGame()
{
    if (!m_emulationRunning)
    {
        return;
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

void SciterMainWindow::UpdateStatusBar()
{
    SciterElement statusTextEl(m_rootElement.GetElementByID("StatusText"));
    if (!statusTextEl.IsValid())
    {
        return;
    }
    if (!m_emulationRunning)
    {
        m_rootElement.SetTimer(0, (uint32_t *)TIMER_UPDATE_STATUS);
        statusTextEl.SetText("");
        return;
    }

    if (!m_modules.IsValid())
    {
        return;
    }
    IOperatingSystem & operatingSystem = m_modules.Modules().OperatingSystem();
    IVideo & video = m_modules.Modules().Video();
    std::vector<std::string> parts;

    if (operatingSystem.IsEmulationPaused())
    {
        parts.push_back("Paused");
    }

    const int shaders_building = video.ShadersBuilding();

    if (shaders_building > 0)
    {
        parts.push_back(stdstr_f("Building: %d shader(s)", shaders_building));
    }

    PerfStatsResults results = operatingSystem.GetAndResetPerfStats();

    if (m_resolutionUpFactor != 1.0)
    {
        parts.push_back(stdstr_f("Scale: %.0fx", m_resolutionUpFactor));
    }
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
    parts.push_back(stdstr_f("%.0f FPS (%.2f ms)%s", std::round(results.average_game_fps), std::isnan(results.frametime) ? 0.0 : (results.frametime * 1000.0), m_useSpeedLimit ? "" : " Unlocked"));
    
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
    return GuiAction::Invalid;
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
    m_sciterUI.Stop();
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
    case GuiAction::ToggleFullscreen:
        ToggleFullscreen();
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
    return 1.0;
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
            m_win32Fullscreen->pendingSwallowKeyUp = (uint32_t)keyCode;
            OnGuiAction(GuiAction::ToggleFullscreen);
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
#ifdef _WIN32
    if (m_win32Fullscreen != nullptr && m_win32Fullscreen->pendingSwallowKeyUp != 0 &&
        (uint32_t)keyCode == m_win32Fullscreen->pendingSwallowKeyUp)
    {
        m_win32Fullscreen->pendingSwallowKeyUp = 0;
        return true;
    }
#endif
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
    UpdatePausePanel();
}

void SciterMainWindow::UpdatePausePanel()
{
    SciterElement panel(m_rootElement.GetElementByID("PausePanel"));
    if (!panel.IsValid())
    {
        return;
    }

    const bool paused =
        m_emulationRunning && m_modules.IsValid() && m_modules.Modules().OperatingSystem().IsEmulationPaused();

    panel.SetStyleAttribute("display", paused ? "block" : "none");

    if (m_renderWindow == nullptr || !m_emulationRunning)
    {
        return;
    }

    if (paused)
    {
        ShowWindow((HWND)m_renderWindow, SW_HIDE);
    }
    else
    {
        SettingsStore & settings = SettingsStore::GetInstance();
        const bool showRender = settings.GetBool(NXCoreSetting::DisplayedFrames);
        ShowWindow((HWND)m_renderWindow, showRender ? SW_SHOW : SW_HIDE);
    }
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
    else
    {
        EnterFullscreen();
    }
}

void SciterMainWindow::EnterFullscreen()
{
    if (m_window == nullptr || !m_win32Fullscreen || m_win32Fullscreen->active)
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

    SciterElement hdr(m_rootElement.FindFirst("header"));
    if (hdr.IsValid())
    {
        hdr.AddClassName("nx-fullscreen-hide");
    }
    SciterElement mm(m_rootElement.GetElementByID("MainMenu"));
    if (mm.IsValid())
    {
        mm.AddClassName("nx-fullscreen-hide");
    }
    SciterElement sb(m_rootElement.GetElementByID("StatusBar"));
    if (sb.IsValid())
    {
        sb.AddClassName("nx-fullscreen-hide");
    }

    fs.active = true;
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

    SciterElement hdr(m_rootElement.FindFirst("header"));
    if (hdr.IsValid())
    {
        hdr.RemoveClassName("nx-fullscreen-hide");
    }
    SciterElement mm(m_rootElement.GetElementByID("MainMenu"));
    if (mm.IsValid())
    {
        mm.RemoveClassName("nx-fullscreen-hide");
    }
    SciterElement statusEl(m_rootElement.GetElementByID("StatusBar"));
    if (statusEl.IsValid())
    {
        statusEl.RemoveClassName("nx-fullscreen-hide");
    }

    SetWindowLongPtr(hwnd, GWL_STYLE, fs.savedStyle);
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, fs.savedExStyle);
    SetWindowPlacement(hwnd, &fs.placement);

    fs.active = false;
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

    const float window_aspect_ratio =
        static_cast<float>(nominal_height) / static_cast<float>(nominal_width);
    const float emulation_aspect_ratio = EmulationAspectRatioForWindowReset(window_aspect_ratio);
    const uint32_t targetH = nominal_height;
    const uint32_t targetW =
        static_cast<uint32_t>(std::lround(static_cast<double>(nominal_height) / static_cast<double>(emulation_aspect_ratio)));

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
    if (element == rootElement.GetElementByID("renderer"))
    {
        SettingsStore & settings = SettingsStore::GetInstance();
        Settings::RendererBackend graphicsAPI = (Settings::RendererBackend)settings.GetInt(NXVideoSetting::GraphicsAPI);
        if (graphicsAPI == Settings::RendererBackend::Vulkan)
        {
            graphicsAPI = Settings::RendererBackend::OpenGL;
        }
        else if (graphicsAPI == Settings::RendererBackend::OpenGL)
        {
            graphicsAPI = Settings::RendererBackend::Vulkan;
        }
        settings.SetInt(NXVideoSetting::GraphicsAPI, (int32_t)graphicsAPI);
        stdstr_f text("%s", Settings::CanonicalizeEnum((Settings::RendererBackend)settings.GetInt(NXVideoSetting::GraphicsAPI)).c_str());
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
        impl->UpdateStatusbar();
    }
    else if (strcmp(setting, NXOsSetting::ResolutionUpFactor) == 0)
    {
        impl->m_resolutionUpFactor = SettingsStore::GetInstance().GetFloat(NXOsSetting::ResolutionUpFactor);
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
        UpdateStatusBar();
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
        ApplyEmulationLoadingUi();
    }
    else if (event_code == EVENT_EMULATION_RUNNING)
    {
        PreventOSSleep();
        m_rootElement.SetTimer(500, (uint32_t *)TIMER_UPDATE_STATUS);
    }
    else if (event_code == EVENT_EMULATION_STOPPED)
    {
        ShowWindow((HWND)m_renderWindow, SW_HIDE);
        SciterElement LoadingPanel(m_rootElement.GetElementByID("LoadingPanel"));
        if (LoadingPanel.IsValid())
        {
            LoadingPanel.SetStyleAttribute("display", "none");
        }

        SciterElement romBrowserPanel(m_rootElement.GetElementByID("RomBrowserPanel"));
        if (romBrowserPanel.IsValid())
        {
            romBrowserPanel.SetStyleAttribute("display", "block");
        }
        UpdatePausePanel();
    }
    else if (event_code == EVENT_EMULATION_FIRST_FRAME)
    {
        SettingsStore & settings = SettingsStore::GetInstance();
        if (settings.GetBool(NXCoreSetting::DisplayedFrames))
        {
            ShowWindow((HWND)m_renderWindow, SW_SHOW);
            SciterElement LoadingPanel(m_rootElement.GetElementByID("LoadingPanel"));
            if (LoadingPanel.IsValid())
            {
                LoadingPanel.SetStyleAttribute("display", "none");
            }
        }
        UpdatePausePanel();
    }
    else if (event_code == EVENT_DISK_CACHE_STATUS)
    {
        RefreshDiskCacheLoadingText();
    }
    return false;
}
