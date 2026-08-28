#include "settings/ui_settings.h"
#include "startup_checks.h"
#include "user_interface/notification.h"
#include "user_interface/sciter_main_window.h"
#include <common/path.h>
#include <common/std_string.h>
#include <memory>
#include <nxemu-core/app_init.h>
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

static int RunApplication() 
{
    const VulkanCheckResult probe = RunVulkanProbeIfChild();
    if (probe != VULKAN_CHECK_DONE)
    {
        return probe == EXIT_VULKAN_AVAILABLE ? 0 : 1;
    }

#ifdef WIN32
    EnablePerMonitorDpiAwareness();
#endif
    bool res = AppInit(&Notification::GetInstance(), Path(Path::MODULE_DIRECTORY), Common::FS::GetYuzuPathString(Common::FS::YuzuPath::YuzuDir).c_str());

    if (res && uiSettings.performVulkanCheck)
    {
        VulkanCheckResult result = StartupVulkanChecks();
        if (result != VULKAN_CHECK_DONE)
        {
            return result == EXIT_VULKAN_AVAILABLE ? 0 : 1;
        }
    }

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
    return RunApplication();
}
#else
int main(int /*argc*/, char * /*argv*/[])
{
    return RunApplication();
}
#endif
