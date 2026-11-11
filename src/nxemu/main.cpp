#include "startup_checks.h"
#include "settings/ui_settings.h"
#include <common/path.h>
#include <nxemu-core/app_init.h>
#include <nxemu-core/settings/core_settings.h>
#include <filesystem>
#include <yuzu_common/fs/path_util.h>
#include <nxemu/ui/ui_application.h>

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __APPLE__
#include <sys/resource.h>
#endif

#ifdef _WIN32
extern "C"
{
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;       // NVIDIA
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1; // AMD
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

#ifdef _WIN32
// SDL tags each window with a "SDL_WindowData" property, whose global atom is
// deleted again when the last window closes. Windows hands a freed atom slot
// to the free list of whichever CPU freed it, so every start and exit moves a
// slot between CPUs; after enough runs some CPUs have none left and SetProp,
// and with it window creation, fails on them until the next logon. Holding a
// reference that is never released keeps the atom alive across runs.
static void PinSdlWindowAtom()
{
    GlobalAddAtomW(L"SDL_WindowData");
}
#endif

static int RunApplication(int argc, char ** argv)
{
#ifdef _WIN32
    PinSdlWindowAtom();
#endif
#ifdef __APPLE__
    RaiseOpenFileLimit();
#endif
    // The process may be the child that probes Vulkan for another instance.
    bool isChild = false;
    if (CheckEnvVars(&isChild))
    {
        return 0;
    }
    if (!AppInit(Path(Path::MODULE_DIRECTORY), Common::FS::GetYuzuPathString(Common::FS::YuzuPath::YuzuDir).c_str()))
    {
        AppCleanup();
        return 1;
    }
    SetupUISetting();
    const auto exeDir = Common::FS::GetExeDirectory();
    const bool bundled = exeDir.filename() == "MacOS" && exeDir.parent_path().filename() == "Contents";
    const auto modules = bundled ? exeDir.parent_path() / "Frameworks/modules" : exeDir / "modules";
    if (std::filesystem::is_directory(modules))
    {
        coreSettings.moduleDir = Path(modules.string(), "");
    }
    const int result = RunUiApplication(argc, argv);
    AppCleanup();
    return result;
}

#ifdef _WIN32
int WINAPI WinMain(_In_ HINSTANCE /*hInstance*/, _In_opt_ HINSTANCE /*hPrevInstance*/, _In_ LPSTR /*lpszArgs*/, _In_ int /*nWinMode*/)
{
    return RunApplication(__argc, __argv);
}
#else
int main(int argc, char * argv[])
{
    return RunApplication(argc, argv);
}
#endif
