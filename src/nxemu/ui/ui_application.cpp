#include "ui_application.h"
#include "settings/ui_settings.h"
#include "ui/emulation/emulator_session.h"
#include "ui/host/ui_dialog_window.h"
#include "ui/host/ui_notification.h"
#include "ui/host/ui_pack.h"
#include "ui/host/ui_window.h"
#include "ui/i18n/translation.h"
#include "ui/library/game_library.h"
#include "ui/library/interface_language.h"
#include "ui/views/main_window.h"
#include "ui/views/message_dialog.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <iostream>
#include <cstdlib>
#include <nxemu-core/app_init.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/version.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu-video/video_settings_identifiers.h>
#include <string>
#include <yuzu_common/fs/path_util.h>
#include <yuzu_common/logging/log.h>

namespace
{
constexpr int kInitialWidth = 1280;
constexpr int kInitialHeight = 720;
constexpr uint64_t kStillAfterMs = 250;
constexpr int kStillFrameMs = 100;

bool KeepGraphicsBusy(const UiWindow & window, EmulatorSession & session)
{
#if defined(__APPLE__)
    return session.IsRunning() && !session.IsPaused() && window.IsFullscreen() &&
           !SettingsStore::GetInstance().GetBool(NXOsSetting::UseSpeedLimit);
#else
    (void)window;
    (void)session;
    return false;
#endif
}

std::filesystem::path FindAssetDirectory()
{
    const std::filesystem::path exeDir = Common::FS::GetExeDirectory();
    for (const std::filesystem::path & packDir : {exeDir, exeDir.parent_path() / "Resources"})
    {
        std::error_code packError;
        if (std::filesystem::is_regular_file(packDir / "nxemu.lang", packError) &&
            UiPack::Open(packDir / "nxemu.lang", packDir / "ui"))
        {
            return packDir / "ui";
        }
    }
    const std::filesystem::path candidates[] = {
        exeDir / "ui",
        // macOS application bundle: Contents/MacOS/../Resources/ui
        exeDir.parent_path() / "Resources" / "ui",
        exeDir.parent_path().parent_path().parent_path() / "src" / "nxemu" / "ui" / "assets",
    };

    for (const std::filesystem::path & dir : candidates)
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(dir / "main_window.rml", error))
        {
            return dir;
        }
    }
    return {};
}

struct LiveResize
{
    UiWindow & window;
    MainWindow & mainWindow;
    SDL_ThreadID thread = SDL_GetCurrentThreadID();
    bool busy = false;
};

bool SDLCALL DrawWhileResizing(void * userdata, SDL_Event * event)
{
    LiveResize & resize = *static_cast<LiveResize *>(userdata);
    UiDialogWindow * dialogWindow = resize.window.DialogWindow();
    if (SDL_GetCurrentThreadID() != resize.thread || resize.busy || event->type != SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
        (event->window.windowID != SDL_GetWindowID(resize.window.Handle()) && (dialogWindow == nullptr || !dialogWindow->Owns(*event))))
    {
        return true;
    }
    resize.busy = true;
    resize.mainWindow.HandleEvent(*event);
    resize.mainWindow.Update();
    resize.window.PaceFrame();
    resize.window.Render(resize.mainWindow.GameArea());
    resize.busy = false;
    return true;
}

int Run(UiNotification & notification, const std::string & gamePath)
{
    const std::filesystem::path assetDir = FindAssetDirectory();
    if (assetDir.empty())
    {
        std::cerr << "NXEmu: the interface files are missing\n";
        return 1;
    }

    UiWindow window;
    const std::string title = std::string("NXEmu ") + VER_FILE_VERSION_STR;
    if (!window.Create(title.c_str(), kInitialWidth, kInitialHeight, assetDir, SettingsStore::GetInstance().GetInt(NXVideoSetting::VulkanDevice)))
    {
        std::cerr << "NXEmu: " << window.Error() << '\n';
        notification.DisplayError(window.Error().c_str(), "NXEmu");
        return 1;
    }
    LOG_INFO(Frontend, "Startup: window created");
    window.SetTheme(uiSettings.interfaceTheme);
    InterfaceLanguage::AdoptSystemLanguage();
    InterfaceLanguage::Use(InterfaceLanguage::Code(), assetDir);

    Rml::ElementDocument * startup = Translation::LoadDocument(*window.Context(), assetDir / "startup.rml");
    if (startup != nullptr)
    {
        startup->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        window.Context()->Update();
        window.Render(nullptr);
        LOG_INFO(Frontend, "Startup: startup screen shown");
    }

    EmulatorSession session(window, notification, window.Handle());
    if (!session.Initialise())
    {
        notification.DisplayError("Required modules failed to load.", "NXEmu");
        return 1;
    }
    LOG_INFO(Frontend, "Startup: modules ready");
    if (startup != nullptr)
    {
        startup->Close();
    }
    const auto closesMainWindow = [&window](const SDL_Event & event) {
        return event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window.Handle());
    };
    GameLibrary library;
    MainWindow mainWindow(window, session, library, assetDir);
    if (!mainWindow.Create())
    {
        std::cerr << "NXEmu: unable to load the main window document\n";
        return 1;
    }
    LOG_INFO(Frontend, "Startup: main window created");

    if (!gamePath.empty())
    {
        mainWindow.OpenGame(gamePath);
    }

    LiveResize liveResize{window, mainWindow};
    notification.SetPresenter([&](bool question, const std::string & title, const std::string & text) {
        const bool wasBusy = liveResize.busy;
        liveResize.busy = true;
        mainWindow.RevealMouseCursor();
        // The main window takes no input while the dialog window is open.
        MessageDialog dialog(window.FrontContext(), assetDir, question, title, text);
        UiDialogWindow * dialogWindow = window.DialogWindow();
        dialog.Show();
        SDL_Event deferred{};
        bool quitting = false;
        while (dialog.IsVisible())
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_EVENT_QUIT || closesMainWindow(event))
                {
                    deferred = event;
                    quitting = true;
                    dialog.Dismiss();
                    break;
                }
                if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                {
                    dialog.Dismiss();
                    continue;
                }
                window.HandleEvent(event);
            }
            dialog.Update();
            window.SyncNativeState();
            window.Context()->Update();
            if (dialogWindow != nullptr)
            {
                dialogWindow->Update();
            }
            window.PaceFrame();
            if (!window.Render(mainWindow.GameArea()) && !question)
            {
                // The dialog cannot be seen, such as after the GPU stopped responding.
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), text.c_str(), window.Handle());
                dialog.Dismiss();
            }
        }
        mainWindow.RevealMouseCursor();
        liveResize.busy = wasBusy;
        if (quitting)
        {
            SDL_PushEvent(&deferred);
        }
        return dialog.Response();
    });
    SDL_AddEventWatch(&DrawWhileResizing, &liveResize);

    if (!gamePath.empty())
    {
        mainWindow.OpenGame(gamePath);
    }

    uint64_t lastChange = SDL_GetTicks();
    uint64_t lastRender = 0;
    while (!mainWindow.QuitRequested())
    {
        // Input polling keeps its normal cadence while a game runs, even
        // when the interface image can be reused for several game frames.
        window.PaceFrame();
        SDL_Event event;
        bool changed = false;
        bool pending = !session.IsRunning() && SDL_GetTicks() - lastChange >= kStillAfterMs ? SDL_WaitEventTimeout(&event, kStillFrameMs) != 0 : SDL_PollEvent(&event) != 0;
        for (; pending; pending = SDL_PollEvent(&event) != 0)
        {
            liveResize.busy = true;
            changed = true;
            if (event.type == SDL_EVENT_QUIT || closesMainWindow(event))
            {
                mainWindow.RequestQuit();
            }
            else
            {
                mainWindow.HandleEvent(event);
            }
            liveResize.busy = false;
        }
        liveResize.busy = true;
        window.SyncNativeState();
        mainWindow.Update();
        const uint64_t now = SDL_GetTicks();
        if (changed || !mainWindow.Still())
        {
            lastChange = now;
        }
        if (now - lastChange < kStillAfterMs || now - lastRender >= kStillFrameMs || KeepGraphicsBusy(window, session))
        {
            if (window.Render(mainWindow.GameArea()) && lastRender == 0)
            {
                LOG_INFO(Frontend, "Startup: first frame shown");
            }
            lastRender = SDL_GetTicks();
        }
        liveResize.busy = false;
    }
    SDL_RemoveEventWatch(&DrawWhileResizing, &liveResize);
    notification.SetPresenter({});
    return 0;
}
} // namespace

bool StartupChecks(const char * arg0, bool * has_broken_vulkan, bool perform_vulkan_check);

bool probeGraphics(const char * executable)
{
    bool brokenVulkan = false;
    StartupChecks(executable, &brokenVulkan, true);
    return !brokenVulkan;
}

int RunUiApplication(int argc, char ** argv)
{
    UiNotification notification;
    SetNotification(&notification);

    std::string gamePath;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        const char * environment = nullptr;
        if (argument == "--nxshader-import") environment = "NXEMU_IMPORT_NXSHADER";
        else if (argument == "--nxshader-game-root") environment = "NXEMU_SHADER_GAME_ROOT";
        else if (argument == "--nxshader-record") environment = "NXEMU_NXSHADER_RECORD";
        if (environment)
        {
            if (++i >= argc)
            {
                notification.DisplayError("Missing value for NXShader argument.", "NXEmu");
                SetNotification(nullptr);
                return 1;
            }
#ifdef _WIN32
            _putenv_s(environment, argv[i]);
#else
            setenv(environment, argv[i], 1);
#endif
        }
        else if (!argument.empty() && argument[0] != '-' && gamePath.empty()) gamePath = argument;
        else
        {
            notification.DisplayError("Unknown command-line argument.", "NXEmu");
            SetNotification(nullptr);
            return 1;
        }
    }

    int result = 1;
    LOG_INFO(Frontend, "Startup: checking Vulkan");
    if (argc > 0 && uiSettings.performVulkanCheck && !probeGraphics(argv[0]))
    {
        notification.DisplayError("Vulkan could not be initialised. Update the graphics driver and try again.", "NXEmu");
    }
    else
    {
        LOG_INFO(Frontend, "Startup: Vulkan checked");
        result = Run(notification, gamePath);
    }
    SetNotification(nullptr);
    return result;
}
