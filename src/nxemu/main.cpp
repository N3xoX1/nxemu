#include "settings/ui_settings.h"
#include "startup_checks.h"
#include "ui/ui_application.h"
#include <common/path.h>
#include <filesystem>
#include <memory>
#include <nxemu-core/app_init.h>
#ifdef __APPLE__
#include <nxemu-core/settings/core_settings.h>
#include <sys/resource.h>
#endif
#include <yuzu_common/fs/path_util.h>

#ifdef WIN32
#include <windows.h>
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

static int RunApplication(int argc, char ** argv)
{
#ifdef __APPLE__
    RaiseOpenFileLimit();
#endif
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

#ifdef __APPLE__
    const auto exe_dir = Common::FS::GetExeDirectory();
    const bool is_bundle = exe_dir.filename() == "MacOS" && exe_dir.parent_path().filename() == "Contents";
    const bool has_local_runtime = std::filesystem::is_directory(exe_dir / "modules");
    if (is_bundle || has_local_runtime) {
        // A bundle must load its matching modules even when reusing a
        // development config. Keep the stored path values unchanged for raw runs.
        const auto module_dir = is_bundle ? exe_dir.parent_path() / "Frameworks/modules" : exe_dir / "modules";
        coreSettings.moduleDir = Path(module_dir.string(), "");
    }
#endif

    int result = RunUiApplication(argc, argv);
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
