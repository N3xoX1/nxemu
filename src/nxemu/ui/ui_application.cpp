#include "ui_application.h"
#include "settings/ui_settings.h"
#include "ui/emulation/emulator_session.h"
#include "ui/host/ui_notification.h"
#include "ui/host/ui_pack.h"
#include "ui/host/ui_window.h"
#include "ui/host/ui_theme.h"
#include "ui/library/game_library.h"
#include "ui/library/interface_language.h"
#include "ui/views/main_window.h"
#include "ui/views/startup_view.h"
#include "ui/views/message_dialog.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <yuzu_common/logging/log.h>
#include <iostream>
#include <nxemu-core/app_init.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/version.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu-video/video_settings_identifiers.h>
#include <nxemu-module-spec/video.h>
#include <string>
#include <yuzu_common/fs/path_util.h>
#include <yuzu_common/scope_exit.h>

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
    SDL_threadID thread = SDL_ThreadID();
    bool busy = false;
};

int SDLCALL DrawWhileResizing(void * userdata, SDL_Event * event)
{
    LiveResize & resize = *static_cast<LiveResize *>(userdata);
    if (SDL_ThreadID() != resize.thread || resize.busy ||
        event->type != SDL_WINDOWEVENT || event->window.event != SDL_WINDOWEVENT_SIZE_CHANGED ||
        event->window.windowID != SDL_GetWindowID(resize.window.Handle()))
    {
        return 0;
    }
    resize.busy = true;
    resize.mainWindow.HandleEvent(*event);
    resize.mainWindow.Update();
    resize.window.PaceFrame();
    resize.window.Render(resize.mainWindow.GameArea());
    resize.busy = false;
    return 0;
}

int Run(UiNotification & notification, const std::string & gamePath, std::chrono::steady_clock::time_point started)
{
    const std::filesystem::path assetDir = FindAssetDirectory();
    if (assetDir.empty())
    {
        std::cerr << "NXEmu: the interface files are missing\n";
        return 1;
    }

    UiWindow window;
    const std::string title = std::string("NXEmu ") + VER_FILE_VERSION_STR;
    if (!window.Create(title.c_str(), kInitialWidth, kInitialHeight, assetDir))
    {
        std::cerr << "NXEmu: " << window.Error() << '\n';
        return 1;
    }
    window.SetTheme(uiSettings.interfaceTheme);

    EmulatorSession session(window, notification, window.Handle());
    if (!session.InitialiseDisplay())
    {
        notification.DisplayError("Required modules failed to load.", "NXEmu");
        return 1;
    }
    // RmlUI renders through Vulkan only. An older Sciter configuration may
    // still select OpenGL/Null; override it for this frontend before the
    // shared display is created, without changing other frontends.
    auto & settings = SettingsStore::GetInstance();
    if (settings.GetInt(NXVideoSetting::GraphicsAPI) != static_cast<int32_t>(RendererBackend::Vulkan))
        settings.SetInt(NXVideoSetting::GraphicsAPI, static_cast<int32_t>(RendererBackend::Vulkan));
    if (!window.AttachVideo(session.DisplayVideo()))
    {
        notification.DisplayError(window.Error().c_str(), "NXEmu");
        return 1;
    }
    // Run owns the interface binding.  Detach after MainWindow and all its
    // RmlUI resources have been destroyed, but before EmulatorSession
    // destroys the host video module, including on early returns.
    SCOPE_EXIT { window.DetachVideo(); };
    StartupView startup(window);
    if (!startup.Create(assetDir) || !startup.Pump())
        return startup.Cancelled() ? 0 : 1;
    LOG_INFO(Frontend, "Frontend startup: first frame submitted in {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());
    if (!startup.Pump("Preparing your library...")) return startup.Cancelled() ? 0 : 1;
    if (!session.Initialise())
    {
        notification.DisplayError("Required modules failed to load.", "NXEmu");
        return 1;
    }
    if (!startup.Pump("Preparing the interface...")) return startup.Cancelled() ? 0 : 1;
    if (!window.LoadFallbackFonts([&] { return startup.Pump(); })) return startup.Cancelled() ? 0 : 1;
    InterfaceLanguage::AdoptSystemLanguage(session);
    InterfaceLanguage::Use(InterfaceLanguage::Code(), assetDir);
    if (!startup.Pump("Preparing your library...")) return startup.Cancelled() ? 0 : 1;
    GameLibrary library;
    MainWindow mainWindow(window, session, library, assetDir);
    if (!mainWindow.Create([&] { return startup.Pump(); }))
    {
        if (startup.Cancelled()) return 0;
        std::cerr << "NXEmu: unable to load the main window document\n";
        return 1;
    }
    // Keep presenting the opaque loading document while the catalogue, layout
    // and first viewport's icons are prepared underneath it. Two ready passes
    // allow layout changes from Context::Update to feed back into virtualization.
    unsigned readyFrames = 0;
    while (readyFrames < 2)
    {
        window.PaceFrame();
        // Sample before Update drains the worker queue, including scan failure.
        const bool scanFinished = !library.Scanning();
        mainWindow.Update();
        if (mainWindow.QuitRequested()) return 1;
        if (!startup.Pump("Preparing your library...")) return startup.Cancelled() ? 0 : 1;
        readyFrames = scanFinished && mainWindow.StartupReady() ? readyFrames + 1 : 0;
    }
    startup.Close();
    window.Context()->Update();
    if (!window.Render(mainWindow.GameArea())) return 1;
    LOG_INFO(Frontend, "Frontend startup: library ready in {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());

    if (!gamePath.empty())
    {
        mainWindow.OpenGame(gamePath);
    }

    LiveResize liveResize{window, mainWindow};
    notification.SetPresenter([&](bool question, const std::string & title, const std::string & text) {
        const bool wasBusy = liveResize.busy;
        liveResize.busy = true;
        mainWindow.RevealMouseCursor();
        MessageDialog dialog(window.Context(), assetDir, question, title, text);
        dialog.Show();
        SDL_Event deferred{};
        bool quitting = false;
        while (dialog.IsVisible())
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE))
                {
                    deferred = event;
                    quitting = true;
                    dialog.Dismiss();
                    break;
                }
                window.HandleEvent(event);
            }
            dialog.Update();
            window.SyncNativeState();
            window.Context()->Update();
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
    // These callbacks borrow stack-owned MainWindow/LiveResize state.
    // Unregister them on every exit path, including exceptions.
    SCOPE_EXIT { notification.SetPresenter({}); };
    SDL_AddEventWatch(&DrawWhileResizing, &liveResize);
    SCOPE_EXIT { SDL_DelEventWatch(&DrawWhileResizing, &liveResize); };

    uint64_t lastChange = SDL_GetTicks64();
    uint64_t lastRender = 0;
    while (!mainWindow.QuitRequested())
    {
        // Input polling keeps its normal cadence while a game runs, even
        // when the interface image can be reused for several game frames.
        window.PaceFrame();
        SDL_Event event;
        bool changed = false;
        bool pending = !session.IsRunning() && SDL_GetTicks64() - lastChange >= kStillAfterMs ? SDL_WaitEventTimeout(&event, kStillFrameMs) != 0 : SDL_PollEvent(&event) != 0;
        for (; pending; pending = SDL_PollEvent(&event) != 0)
        {
            liveResize.busy = true;
            changed = true;
            if (event.type == SDL_QUIT)
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
        const uint64_t now = SDL_GetTicks64();
        if (changed || !mainWindow.Still())
        {
            lastChange = now;
        }
        if (now - lastChange < kStillAfterMs || now - lastRender >= kStillFrameMs || KeepGraphicsBusy(window, session))
        {
            window.Render(mainWindow.GameArea());
            lastRender = SDL_GetTicks64();
        }
        liveResize.busy = false;
    }
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

int RunUiApplication(int argc, char ** argv, std::chrono::steady_clock::time_point started)
{
    UiNotification notification;
    SetNotification(&notification);

    int result = 1;
    if (argc > 0 && uiSettings.performVulkanCheck && !probeGraphics(argv[0]))
    {
        notification.DisplayError("Vulkan could not be initialised. Update the graphics driver and try again.", "NXEmu");
    }
    else
    {
        result = Run(notification, argc > 1 ? argv[1] : "", started);
    }
    SetNotification(nullptr);
    return result;
}
