#include "startup_checks.h"
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

static int RunApplication(int argc, char ** argv)
{
#ifdef __APPLE__
    RaiseOpenFileLimit();
#endif
    // The process may be the child that probes Vulkan for another instance.
    bool isChild = false;
    if (CheckEnvVars(&isChild))
    {
        return 0;
    }
    return RunUiApplication(argc, argv, [](const char * executable) {
        bool brokenVulkan = false;
        StartupChecks(executable, &brokenVulkan, true);
        return !brokenVulkan;
    });
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
