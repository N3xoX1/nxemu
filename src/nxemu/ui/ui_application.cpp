#include <nxemu-core/settings/game_settings.h>
#include "ui_application.h"
#include <common/path.h>
#include <nxemu/ui/host/ui_pack.h>
#include <filesystem>
#include <iostream>
#include <nxemu-core/app_init.h>
#include <nxemu-core/settings/core_settings.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/version.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu/ui/emulation/emulator_session.h>
#include <nxemu/ui/host/system_language.h>
#include <nxemu/ui/host/ui_notification.h>
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/ui/i18n/translation.h>
#include <nxemu/ui/library/game_library.h>
#include <nxemu/ui/library/ui_preferences.h>
#include <nxemu/ui/views/main_window.h>
#include <nxemu/ui/views/message_dialog.h>
#include <RmlUi/Core/Context.h>
#include <string>
#include <yuzu_common/fs/fs_util.h>
#include <yuzu_common/fs/path_util.h>

namespace
{
// The console speaks the language of the computer until the user chooses
// another: once, and not over a language already chosen.
void AdoptSystemLanguage(EmulatorSession & session)
{
    if (UiPreferences::GetBool(UiPreferences::ConsoleLanguageSet, false))
    {
        return;
    }
    SettingsStore & settings = SettingsStore::GetInstance();
    Language language{};
    if (settings.GetInt(NXOsSetting::LanguageIndex) == settings.GetDefaultInt(NXOsSetting::LanguageIndex) && SystemConsoleLanguage(language))
    {
        settings.SetInt(NXOsSetting::LanguageIndex, static_cast<int32_t>(language));
        session.FlushSettings();
    }
    UiPreferences::SetBool(UiPreferences::ConsoleLanguageSet, true);
}

constexpr int kInitialWidth = 1280;
constexpr int kInitialHeight = 720;
// A window that stays the same is not drawn at the display's rate: once it
// has been still this long, it is only drawn again at the slow interval, or
// as soon as an event arrives. The slow interval is how late something that
// comes from a background task, like a game's details, may show.
constexpr uint64_t kStillAfterMs = 250;
constexpr int kStillFrameMs = 100;

// In full screen macOS lowers the graphics processor's frequency under the
// bursts of work a game sends it. An interface drawn with every frame keeps
// the frequency up, which a game running unlimited gains more from than the
// drawing costs.
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

// Documents, fonts and images ship beside the executable, or in the bundle's
// resources on macOS. A build run from its tree falls back to the sources.
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
    for (const std::filesystem::path & candidate : {
             exeDir / "ui",
             exeDir.parent_path() / "Resources" / "ui",
             std::filesystem::path(NXEMU_UI_SOURCE_ASSETS),
         })
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate / "main_window.rml", error))
        {
            return candidate;
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

// While the user drags a window edge the system runs its own event loop and
// Run()'s does not turn. SDL still reports each new size to event watchers,
// so the interface is laid out and drawn from here in the meantime.
int SDLCALL DrawWhileResizing(void * userdata, SDL_Event * event)
{
    LiveResize & resize = *static_cast<LiveResize *>(userdata);
    // Event watchers can run on the thread that pushed the event. RmlUi and
    // its command buffer belong to the UI thread, and cannot be reentered
    // by a size change generated while an update is already in progress.
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

int Run(UiNotification & notification, const std::string & gamePath)
{
    const std::filesystem::path assetDir = FindAssetDirectory();
    if (assetDir.empty())
    {
        notification.DisplayError("The interface files are missing. Reinstall NXEmu with its ui folder.", "NXEmu");
        return 1;
    }

    std::string overrideError;
    const auto userOverrides = std::filesystem::path{Common::FS::ToU8String(SettingsStore::GetInstance().GetConfigFile())}.parent_path() / "game_overrides.user.json";
    if (!GameSettings::GetInstance().Initialize(assetDir / "game_overrides.json", userOverrides, overrideError))
    {
        std::cerr << "NXEmu: " << overrideError << '\n';
    }

    UiWindow window;
    const std::string title = std::string("NXEmu ") + VER_FILE_VERSION_STR;
    if (!window.Create(title.c_str(), kInitialWidth, kInitialHeight, assetDir))
    {
        notification.DisplayError(window.Error().c_str(), "NXEmu");
        return 1;
    }

    // Destroyed in reverse order: the view first, then the modules that draw
    // it, and the window last.
    EmulatorSession session(window, notification, window.Handle());
    if (!session.Initialise())
    {
        notification.DisplayError(Tr("Required modules failed to load.").c_str(), "NXEmu");
        return 1;
    }
    AdoptSystemLanguage(session);
    Translation::UseLanguage(UiPreferences::InterfaceLanguageCode(), assetDir / "lang",
        std::filesystem::path(NXEMU_UI_SOURCE_ASSETS).parent_path() / "lang");
    if (!window.AttachVideo(session.Modules().Video()))
    {
        notification.DisplayError(window.Error().c_str(), "NXEmu");
        return 1;
    }
    GameLibrary library;
    MainWindow mainWindow(window, session, library, assetDir);
    if (!mainWindow.Create())
    {
        notification.DisplayError(Tr("Unable to load the interface. Check that NXEmu's ui folder is complete.").c_str(), "NXEmu");
        return 1;
    }
    if (!gamePath.empty())
    {
        mainWindow.OpenGame(gamePath);
    }

    LiveResize liveResize{window, mainWindow};
    notification.SetPresenter([&](bool question, const std::string & title, const std::string & text) {
        // Module queries are synchronous. Process only modal input and drawing
        // here: reentering MainWindow::Update could destroy the asking module.
        const bool wasBusy = liveResize.busy;
        liveResize.busy = true;
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
        liveResize.busy = wasBusy;
        if (quitting) SDL_PushEvent(&deferred);
        return dialog.Response();
    });
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
    notification.SetPresenter({});
    return 0;
}
} // namespace

int RunUiApplication(int argc, char ** argv, bool (*probeGraphics)(const char * executable))
{
    UiNotification notification;
    const std::string appDir = Common::FS::GetYuzuPathString(Common::FS::YuzuPath::YuzuDir);
    if (!AppInit(Path(Path::MODULE_DIRECTORY), appDir.c_str()))
    {
        AppCleanup();
        return 1;
    }
    SetNotification(&notification);
    Translation::Clear();
    UiPreferences::ApplyStatusOverrides();
    const std::string interfaceLanguage = UiPreferences::InterfaceLanguageChoice();
    if (!interfaceLanguage.empty())
    {
        Translation::UseLanguage(interfaceLanguage, FindAssetDirectory() / "lang",
            std::filesystem::path(NXEMU_UI_SOURCE_ASSETS).parent_path() / "lang");
    }
    // A packaged build loads the modules shipped with it, whatever the
    // settings say; otherwise the configured directory is used.
    const std::filesystem::path exeDir = Common::FS::GetExeDirectory();
    bool packaged = false;
    for (const std::filesystem::path & candidate : {exeDir.parent_path() / "Frameworks" / "modules", exeDir / "modules"})
    {
        std::error_code error;
        if (!packaged && std::filesystem::is_directory(candidate, error))
        {
            coreSettings.moduleDir = Path(candidate.string(), "");
            packaged = true;
        }
    }
#if defined(NXEMU_UI_BUILD_MODULE_DIR)
    if (!packaged)
    {
        coreSettings.moduleDir = Path(NXEMU_UI_BUILD_MODULE_DIR, "");
    }
#endif

    int result = 1;
    if (argc > 0 && UiPreferences::GetBool(UiPreferences::PerformVulkanCheck, true) && !probeGraphics(argv[0]))
    {
        notification.DisplayError(Tr("Vulkan could not be initialised. Update the graphics driver and try again.").c_str(), "NXEmu");
    }
    else
    {
        result = Run(notification, argc > 1 ? argv[1] : "");
    }
    SetNotification(nullptr);
    AppCleanup();
    return result;
}
