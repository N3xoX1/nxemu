#include "ui_application.h"
#include "settings/ui_settings.h"
#include "ui/emulation/emulator_session.h"
#include "ui/host/ui_notification.h"
#include "ui/host/ui_window.h"
#include "ui/views/main_window.h"
#include <common/path.h>
#include <iostream>
#include <nxemu-core/app_init.h>
#include <nxemu-core/version.h>
#include <string>
#include <yuzu_common/fs/path_util.h>

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
    const std::filesystem::path candidates[] = {
        exeDir / "ui",
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

int Run(UiNotification & notification, const std::string & /*gamePath*/)
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

    EmulatorSession session(window, notification, window.Handle());
    if (!session.Initialise())
    {
        notification.DisplayError("Required modules failed to load.", "NXEmu");
        return 1;
    }
    if (!window.AttachVideo(session.Modules().Video()))
    {
        notification.DisplayError(window.Error().c_str(), "NXEmu");
        return 1;
    }
    MainWindow mainWindow(window, session, assetDir);
    if (!mainWindow.Create())
    {
        std::cerr << "NXEmu: unable to load the main window document\n";
        return 1;
    }

    LiveResize liveResize{window, mainWindow};
    SDL_AddEventWatch(&DrawWhileResizing, &liveResize);

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
    SDL_DelEventWatch(&DrawWhileResizing, &liveResize);
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
    const std::string appDir = Common::FS::GetYuzuPathString(Common::FS::YuzuPath::YuzuDir);
    if (!AppInit(&notification, Path(Path::MODULE_DIRECTORY), appDir.c_str()))
    {
        return 1;
    }

    int result = 1;
    if (argc > 0 && uiSettings.performVulkanCheck && !probeGraphics(argv[0]))
    {
        notification.DisplayError("Vulkan could not be initialised. Update the graphics driver and try again.", "NXEmu");
    }
    else
    {
        result = Run(notification, argc > 1 ? argv[1] : "");
    }
    AppCleanup();
    return result;
}
