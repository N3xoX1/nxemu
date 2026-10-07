#include "settings/ui_settings.h"
#include "startup_checks.h"
#include "user_interface/notification.h"
#include "user_interface/sciter_main_window.h"
#include <common/path.h>
#include <common/std_string.h>
#include <filesystem>
#include <memory>
#include <nxemu-core/app_init.h>
#ifdef __APPLE__
#include <nxemu-core/settings/core_settings.h>
#include <sys/resource.h>
#endif
#include <nxemu-core/version.h>
#include <yuzu_common/fs/path_util.h>
#include <sciter_ui.h>
#include <widgets/list_box.h>
#include <widgets/combo_box.h>
#include <widgets/menubar.h>
#include <widgets/page_nav.h>
#include <widgets/tooltip_host.h>
#include "user_interface/widgets/rom_browser.h"

#ifdef WIN32
#include <windows.h>
#endif

void RegisterWidgets(ISciterUI & sciterUI)
{
    Register_WidgetListBox(sciterUI);
    Register_WidgetComboBox(sciterUI);
    Register_WidgetMenuBar(sciterUI);
    Register_WidgetPageNav(sciterUI);
    Register_WidgetToolTipHost(sciterUI);
    Register_WidgetRomBrowser(sciterUI);
}

#ifdef WIN32
extern "C"
{
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;         // NVIDIA
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;   // AMD
}

static void EnablePerMonitorDpiAwareness()
{
    typedef BOOL (WINAPI * PFN_SetProcessDpiAwarenessContext)(HANDLE);
    typedef HRESULT (WINAPI * PFN_SetProcessDpiAwareness)(int);
    typedef BOOL (WINAPI * PFN_SetProcessDPIAware)(void);

    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr)
    {
        PFN_SetProcessDpiAwarenessContext setCtx = reinterpret_cast<PFN_SetProcessDpiAwarenessContext>(
            ::GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setCtx != nullptr)
        {
            if (setCtx(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4))))
            {
                return;
            }
            if (setCtx(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-3))))
            {
                return;
            }
        }
    }

    HMODULE shcore = ::LoadLibraryW(L"shcore.dll");
    if (shcore != nullptr)
    {
        PFN_SetProcessDpiAwareness setAware = reinterpret_cast<PFN_SetProcessDpiAwareness>(
            ::GetProcAddress(shcore, "SetProcessDpiAwareness"));
        if (setAware != nullptr && SUCCEEDED(setAware(2)))
        {
            return;
        }
    }

    if (user32 != nullptr)
    {
        PFN_SetProcessDPIAware setLegacy = reinterpret_cast<PFN_SetProcessDPIAware>(
            ::GetProcAddress(user32, "SetProcessDPIAware"));
        if (setLegacy != nullptr)
        {
            setLegacy();
        }
    }
}
#endif

#ifdef __APPLE__
// Apps started from Finder inherit a soft limit of 256 open files, which a
// firmware install (one open file per NCA) exceeds.
static void RaiseOpenFileLimit()
{
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0)
    {
        return;
    }
    const rlim_t wanted = 10240; // OPEN_MAX, the most macOS accepts here
    const rlim_t target = limit.rlim_max == RLIM_INFINITY || limit.rlim_max > wanted ? wanted : limit.rlim_max;
    if (limit.rlim_cur < target)
    {
        limit.rlim_cur = target;
        setrlimit(RLIMIT_NOFILE, &limit);
    }
}
#endif

static int RunApplication(const char * arg0) 
{
#ifdef __APPLE__
    RaiseOpenFileLimit();
#endif
    bool has_broken_vulkan = false;
    bool is_child = false;
    if (CheckEnvVars(&is_child)) {
        return 0;
    }

#ifdef WIN32
    EnablePerMonitorDpiAwareness();
#endif
    bool res = AppInit(&Notification::GetInstance(), Path(Path::MODULE_DIRECTORY), Common::FS::GetYuzuPathString(Common::FS::YuzuPath::YuzuDir).c_str());

#ifdef __APPLE__
    const auto exe_dir = Common::FS::GetExeDirectory();
    const bool is_bundle = exe_dir.filename() == "MacOS" && exe_dir.parent_path().filename() == "Contents";
    const bool has_local_runtime = std::filesystem::is_directory(exe_dir / "modules") &&
                                   std::filesystem::is_directory(exe_dir / "lang");
    if (res && (is_bundle || has_local_runtime)) {
        // A bundle must load its matching modules/resources even when reusing a
        // development config. Keep the stored path values unchanged for raw runs.
        const auto module_dir = is_bundle ? exe_dir.parent_path() / "Frameworks/modules" : exe_dir / "modules";
        const auto language_dir = is_bundle ? exe_dir.parent_path() / "Resources/lang" : exe_dir / "lang";
        coreSettings.moduleDir = Path(module_dir.string(), "");
        uiSettings.languageDir = Path(language_dir.string(), "");
    }
#endif

    if (res && StartupChecks(arg0, &has_broken_vulkan, uiSettings.performVulkanCheck)) {
        return 0;
    }
    uiSettings.hasBrokenVulkan = has_broken_vulkan;

    ISciterUI * sciterUI = nullptr;
    if (res && !SciterUIInit(uiSettings.languageDir, uiSettings.languageBase.c_str(), uiSettings.languageCurrent.c_str(), uiSettings.sciterConsole, sciterUI))
    {
        res = false;
    }
    if (res)
    {
        RegisterWidgets(*sciterUI);
        SciterMainWindow window(*sciterUI, stdstr_f("NXEmu %s", VER_FILE_VERSION_STR).c_str());
        window.Show();
        sciterUI->Run();
    }
    if (sciterUI != nullptr)
    {
        sciterUI->Shutdown();
    }
    AppCleanup();
    Notification::CleanUp();
    return res ? 0 : 1;
}

#ifdef _WIN32
int WINAPI WinMain(_In_ HINSTANCE /*hInstance*/, _In_opt_ HINSTANCE /*hPrevInstance*/, _In_ LPSTR /*lpszArgs*/, _In_ int /*nWinMode*/)
{
    char arg0[MAX_PATH]{};
    GetModuleFileNameA(nullptr, arg0, MAX_PATH);
    return RunApplication(arg0);
}
#else
int main(int argc, char * argv[])
{
    return RunApplication((argc > 0 && argv[0] != nullptr) ? argv[0] : "nxemu");
}
#endif
