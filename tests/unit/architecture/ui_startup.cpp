#define SDL_MAIN_HANDLED
#include <nxemu-core/app_init.h>
#include <nxemu-core/modules/system_modules.h>
#include <nxemu-core/settings/core_settings.h>
#include <nxemu-core/notification.h>
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/ui/views/startup_view.h>
#include <nxemu/ui/views/main_window.h>
#include <nxemu/ui/host/ui_notification.h>
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
    assert(argc == 6 || (argc == 7 && std::string(argv[6]) == "--preview"));
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
            assert(startup.Create(source / "src/nxemu/ui/assets"));
            assert(startup.Pump()); // Real RmlUI render before any other module can load.
            auto * logo = window.Context()->GetDocument(window.Context()->GetNumDocuments() - 1)->GetElementById("logo");
            Rml::Vector2f logoSize;
            float logoRatio = 0;
            assert(logo && logo->GetIntrinsicDimensions(logoSize, logoRatio));
            assert(logoSize.x > 0 && logoSize.y > 0 && logoRatio == 1.0f);
            // Optional visual inspection, without delaying the product or CTest.
            if (argc == 7)
                for (unsigned frame = 0; frame < 480 && !startup.Cancelled(); ++frame)
                {
                    window.PaceFrame();
                    startup.Pump();
                }
            assert(!(SDL_GetWindowFlags(window.Handle()) & SDL_WINDOW_HIDDEN));
            auto * splash = window.Context()->GetDocument(window.Context()->GetNumDocuments() - 1);
            // A newly shown document must not expose partially constructed UI.
            auto * building = window.Context()->LoadDocumentFromMemory(
                "<rml><head><style>body { width:100%; height:100%; background-color:#ff00ff; }</style></head><body>Building</body></rml>");
            assert(building);
            building->Show();
            assert(startup.Pump());
            for (Rml::Vector2f point : {Rml::Vector2f{10, 100}, Rml::Vector2f{320, 240}, Rml::Vector2f{630, 470}})
            {
                auto * hit = window.Context()->GetElementAtPoint(point);
                assert(hit && hit->GetOwnerDocument() == splash);
            }
            building->Close();
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
            assert(startup.Create(source / "src/nxemu/ui/assets"));
            assert(startup.Pump());
            auto * document = window.Context()->GetDocument(window.Context()->GetNumDocuments() - 1);
            document->GetElementById("close")->DispatchEvent("click", {});
            assert(startup.Cancelled() && !startup.Pump());
        }
        window.DetachVideo();
        modules.ShutDown();
        // Also exercise the actual main document and empty-catalogue handover.
        coreSettings.moduleLoader = std::filesystem::relative(argv[3], source).string();
        coreSettings.moduleCpu = std::filesystem::relative(argv[4], source).string();
        coreSettings.moduleOs = std::filesystem::relative(argv[5], source).string();
        window.SetTheme("dark");
        UiNotification uiNotification;
        {
            EmulatorSession session(window, uiNotification, window.Handle());
            assert(session.InitialiseDisplay());
            assert(window.AttachVideo(session.DisplayVideo()));
            {
                StartupView startup(window);
                assert(startup.Create(source / "src/nxemu/ui/assets") && startup.Pump());
                assert(session.Initialise());
                GameLibrary library;
                MainWindow mainWindow(window, session, library, source / "src/nxemu/ui/assets");
                assert(mainWindow.Create([&] { return startup.Pump(); }));
                unsigned ready = 0;
                for (unsigned frame = 0; frame < 240 && ready < 2; ++frame)
                {
                    window.PaceFrame();
                    const bool scanFinished = !library.Scanning();
                    mainWindow.Update();
                    assert(!mainWindow.QuitRequested() && startup.Pump());
                    ready = scanFinished && mainWindow.StartupReady() ? ready + 1 : 0;
                }
                assert(ready == 2);
                startup.Close();
                window.Context()->Update();
                assert(window.Render(mainWindow.GameArea()));
                // Open Controllers through the real MainWindow action: its
                // utility guest must survive updates and be released on close.
                const auto documentWith = [&](const char * id) -> Rml::ElementDocument * {
                    for (int i = 0; i < window.Context()->GetNumDocuments(); ++i)
                        if (auto * doc = window.Context()->GetDocument(i); doc->GetElementById(id)) return doc;
                    return nullptr;
                };
                const auto click = [](Rml::Element * element) {
                    assert(element);
                    element->DispatchEvent("click", {});
                };
                const auto openControllers = [&] {
                    assert(!session.HasGuest() && !session.Inputs().Available());
                    auto * main = documentWith("title-controllers");
                    assert(main);
                    click(main->GetElementById("title-controllers"));
                    mainWindow.Update();
                    auto * controllers = documentWith("players");
                    assert(controllers && !controllers->IsClassSet("offline"));
                    for (unsigned i = 0; i < 3; ++i) mainWindow.Update();
                    assert(session.HasGuest() && session.Inputs().Available());
                    assert(!session.Inputs().Devices().empty());
                    return controllers;
                };
                const auto closeControllers = [&](Rml::ElementDocument * controllers, bool save) {
                    click(controllers->QuerySelector(save ? "button[data-command='save']" : "button[data-command='cancel']"));
                    mainWindow.Update();
                    assert(!session.HasGuest() && !session.Inputs().Available());
                    window.Context()->Update(); // Drain closed documents before reopening.
                };
                const auto pad = [&]() -> IEmulatedController & {
                    auto & os = session.Modules().OperatingSystem();
                    auto & handheld = os.GetEmulatedController(NpadIdType::Handheld);
                    auto & player = os.GetEmulatedController(NpadIdType::Player1);
                    return handheld.IsConnected() || player.GetNpadStyleIndex() == NpadStyleIndex::Handheld ? handheld : player;
                };
                constexpr auto button = static_cast<uint32_t>(NativeButtonValues::A);
                auto * controllers = openControllers();
                const auto seed = InputParams::Key(SDLK_F10);
                pad().SetButtonParam(button, seed);
                pad().SaveCurrentConfig();
                session.FlushSettings();
                click(controllers->QuerySelector("[data-command='clear:12']"));
                mainWindow.Update();
                assert(InputParams::Take(pad().GetButtonParamPtr(button)).Unset());
                closeControllers(controllers, false);
                controllers = openControllers();
                assert(InputParams::Take(pad().GetButtonParamPtr(button)).Text() == seed.Text());
                click(controllers->QuerySelector("[data-command='clear:12']"));
                mainWindow.Update();
                closeControllers(controllers, true);
                controllers = openControllers();
                assert(InputParams::Take(pad().GetButtonParamPtr(button)).Unset());
                click(controllers->QuerySelector("[data-command='bind:12']"));
                mainWindow.Update();
                assert(controllers->GetElementById("bind-12")->IsClassSet("listening"));
                closeControllers(controllers, false); // Cancel while input polling is active.
            }
            window.DetachVideo();
        }
    }
    AppCleanup();
    SetNotification(nullptr);
    std::filesystem::remove_all(data);
    std::puts("PASS early UI: first frame, opaque overlay, failure, cancellation, close, empty library handover");
}
