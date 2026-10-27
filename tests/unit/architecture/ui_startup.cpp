#define SDL_MAIN_HANDLED
#include <nxemu-core/app_init.h>
#include <nxemu-core/modules/system_modules.h>
#include <nxemu-core/settings/core_settings.h>
#include <nxemu-core/notification.h>
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/ui/views/startup_view.h>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <cstdio>

class Notification final : public INotification
{
public:
    void DisplayError(const char * message, const char *) const override { std::fprintf(stderr, "%s\n", message); }
    NotificationResponse Query(const char *, const char *) const override { return NotificationResponse::No; }
    void BreakPoint(const char *, uint32_t) override { std::abort(); }
};

int main(int argc, char ** argv)
{
    assert(argc == 3);
    const std::filesystem::path source(argv[1]);
    const auto data = std::filesystem::temp_directory_path() /
        ("nxemu-ui-startup-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(data);
    Notification notification;
    SetNotification(&notification);
    assert(AppInit(source.string().c_str(), data.string().c_str()));
    coreSettings.moduleDir = source.string().c_str();
    coreSettings.moduleVideo = std::filesystem::relative(argv[2], source).string();
    coreSettings.moduleCpu = "missing-startup-test-cpu.dll";
    coreSettings.moduleOs = "missing-startup-test-os.dll";
    coreSettings.moduleLoader = "missing-startup-test-loader.dll";
    {
        UiWindow window;
        assert(window.Create("NXEmu startup integration test", 640, 480, source / "src/nxemu/ui/assets"));
        assert(SDL_GetWindowFlags(window.Handle()) & SDL_WINDOW_HIDDEN);
        window.SetTheme("light");
        SystemModules modules;
        assert(modules.SetupDisplay(window));
        assert(!modules.FrontendValid() && !modules.IsValid());
        assert(window.AttachVideo(modules.DisplayVideo()));
        {
            StartupView startup(window);
            assert(startup.Create());
            assert(startup.Pump()); // Real RmlUI render before any other module can load.
            assert(!(SDL_GetWindowFlags(window.Handle()) & SDL_WINDOW_HIDDEN));
            assert(!modules.CompleteFrontendSetup());
            assert(modules.DisplayReady());
            assert(startup.Pump("Services unavailable")); // Failure does not invalidate the binding.
            // A close request during deferred font setup cancels cleanly.
            SDL_Event quit{};
            quit.type = SDL_QUIT;
            assert(SDL_PushEvent(&quit) == 1);
            assert(!window.LoadFallbackFonts([&] { return startup.Pump(); }));
            assert(startup.Cancelled());
        }
        {
            StartupView startup(window);
            assert(startup.Create());
            assert(startup.Pump());
            auto * document = window.Context()->GetDocument(window.Context()->GetNumDocuments() - 1);
            document->GetElementById("close")->DispatchEvent("click", {});
            assert(startup.Cancelled() && !startup.Pump());
        }
        window.DetachVideo();
        modules.ShutDown();
    }
    AppCleanup();
    SetNotification(nullptr);
    std::filesystem::remove_all(data);
    std::puts("PASS early UI: render before services, setup failure, deferred font cancellation, close button");
}
