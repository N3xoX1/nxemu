#include <Windows.h>
#include <DbgHelp.h>
#include <Psapi.h>
#include <nxemu-core/app_init.h>
#include <nxemu-core/modules/system_modules.h>
#include <nxemu-core/modules/module_base.h>
#include <nxemu-core/notification.h>
#include <nxemu-core/settings/core_settings.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-module-spec/system_loader.h>
#include <nxemu-module-spec/video.h>
#include <nxemu-video/video_settings_identifiers.h>
#define VK_NO_PROTOTYPES
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <utility>
#include <vulkan/vulkan.h>

LONG WINAPI CrashTrace(EXCEPTION_POINTERS *exception)
{
    const auto process = GetCurrentProcess();
    SymInitialize(process, nullptr, TRUE);
    auto context = *exception->ContextRecord;
#if defined(_M_X64) || defined(__x86_64__)
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    std::fprintf(stderr, "Exception %08lx at %p\n", exception->ExceptionRecord->ExceptionCode,
                 exception->ExceptionRecord->ExceptionAddress);
    for (int i = 0;
         i < 32 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context,
                               nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr);
         ++i)
    {
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
        auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
            std::fprintf(stderr, "%s + 0x%llx\n", symbol->Name, displacement);
        else
            std::fprintf(stderr, "0x%llx\n", frame.AddrPC.Offset);
    }
#endif
    return EXCEPTION_EXECUTE_HANDLER;
}

std::size_t PrivateBytes()
{
    PROCESS_MEMORY_COUNTERS_EX memory{};
    assert(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),
        sizeof(memory)));
    return memory.PrivateUsage;
}

class Window final : public IRenderWindow
{
  public:
    explicit Window(bool vulkan)
    {
        if (vulkan)
        {
            // No WS_VISIBLE: integration tests never show an interactive window.
            m_window = CreateWindowExW(0, L"STATIC", L"NXEmu Vulkan lifecycle test", WS_OVERLAPPEDWINDOW, 0,
                                       0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            assert(m_window);
        }
    }
    ~Window()
    {
        if (m_window)
            DestroyWindow(m_window);
    }
    void *RenderSurface() const override
    {
        return m_window;
    }
    void *DisplayConnection() const override
    {
        return nullptr;
    }
    RenderWindowSystem WindowSystem() const override
    {
        return m_window ? RenderWindowSystem::Windows : RenderWindowSystem::Default;
    }
    float PixelRatio() const override
    {
        return 1.0f;
    }

  private:
    HWND m_window = nullptr;
};

void DrawInterface(IVideo &video, const VideoInterfaceDevice &device)
{
    const auto getInstanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(device.getInstanceProcAddr);
    const auto getDeviceProc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        getInstanceProc(static_cast<VkInstance>(device.instance), "vkGetDeviceProcAddr"));
    const auto clear = reinterpret_cast<PFN_vkCmdClearColorImage>(
        getDeviceProc(static_cast<VkDevice>(device.device), "vkCmdClearColorImage"));
    const auto barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(
        getDeviceProc(static_cast<VkDevice>(device.device), "vkCmdPipelineBarrier"));
    assert(clear && barrier);
    VideoInterfaceFrame frame{};
    assert(video.BeginInterfaceFrame(320, 240, &frame));
    const auto image = reinterpret_cast<VkImage>(frame.image);
    const auto command = static_cast<VkCommandBuffer>(frame.commandBuffer);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkClearColorValue color{{0.1f, 0.2f, 0.3f, 1.0f}};
    clear(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    VkImageMemoryBarrier transition{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    transition.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    transition.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    transition.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    transition.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    transition.srcQueueFamilyIndex = transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    transition.image = image;
    transition.subresourceRange = range;
    barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
            nullptr, 1, &transition);
    video.EndInterfaceFrame(nullptr);
    video.WaitInterfaceIdle();
}

class Notification final : public INotification
{
  public:
    void DisplayError(const char *message, const char *) const override
    {
        std::fprintf(stderr, "%s\n", message);
    }
    NotificationResponse Query(const char *, const char *) const override
    {
        return NotificationResponse::No;
    }
    void BreakPoint(const char *file, uint32_t line) override
    {
        std::fprintf(stderr, "Unexpected breakpoint %s:%u\n", file, line);
        std::abort();
    }
};

int main(int argc, char **argv)
{
    if (argc != 6 && argc != 7)
        return 1;
    SetUnhandledExceptionFilter(CrashTrace);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool vulkan = argc == 7;
    class VersionAccess : public ModuleBase
    {
    public:
        using ModuleBase::ValidVersion;
    };
    MODULE_INFO info{};
    for (const auto & version : {std::pair{MODULE_TYPE_VIDEO, 0x0127},
             std::pair{MODULE_TYPE_LOADER, 0x0137}, std::pair{MODULE_TYPE_OPERATING_SYSTEM, 0x0127}})
    {
        info.type = version.first;
        info.version = version.second;
        assert(!VersionAccess::ValidVersion(info));
        ++info.version;
        assert(VersionAccess::ValidVersion(info));
    }
    const std::filesystem::path repo(argv[1]);
    const auto data = std::filesystem::temp_directory_path() /
                      ("nxemu-architecture-test-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(data);
    std::printf("data_directory,%s\n", data.string().c_str());
    Notification notification;
    SetNotification(&notification);
    assert(AppInit(repo.string().c_str(), data.string().c_str()));
    coreSettings.moduleDir = repo.string().c_str();
    coreSettings.moduleLoader = std::filesystem::relative(argv[2], repo).string();
    coreSettings.moduleCpu = std::filesystem::relative(argv[3], repo).string();
    coreSettings.moduleVideo = std::filesystem::relative(argv[4], repo).string();
    coreSettings.moduleOs = std::filesystem::relative(argv[5], repo).string();
    Window window(vulkan);
    {
        SystemModules modules;
        const auto before = std::chrono::steady_clock::now();
        assert(modules.SetupFrontend(window));
        const auto hostReady = std::chrono::steady_clock::now();
        const auto hostBytes = PrivateBytes();
        std::size_t guestBytes = 0;
        assert(modules.FrontendValid() && !modules.IsValid());
        auto *const display = &modules.DisplayVideo();
        auto *const loader = &modules.Loader();
        VideoInterfaceDevice device{};
        if (vulkan)
        {
            // This test intentionally cancels before loading an application.
            // A per-title shader cache requires a loaded process/program ID.
            SettingsStore::GetInstance().SetBool(NXVideoSetting::UseDiskPipelineCache, false);
            assert(display->AttachInterface(&device));
            const auto getProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(device.getInstanceProcAddr);
            const auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                getProc(static_cast<VkInstance>(device.instance), "vkGetPhysicalDeviceProperties"));
            VkPhysicalDeviceProperties properties{};
            getProperties(static_cast<VkPhysicalDevice>(device.physicalDevice), &properties);
            std::printf("vulkan_device,%s\n", properties.deviceName);
            std::puts("Vulkan interface attached");
            DrawInterface(*display, device);
        }
        double guestMs = 0;
        for (int i = 0; i < 8; ++i)
        {
            const auto start = std::chrono::steady_clock::now();
            assert(modules.StartSession());
            assert(modules.IsValid());
            if (i == 0) guestBytes = PrivateBytes();
            guestMs +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (vulkan)
            {
                std::printf("Starting Vulkan guest %d\n", i);
                SettingsStore::GetInstance().SetBool(NXCoreSetting::EmulationRunning, true);
                std::puts("Guest startup dispatched");
                assert(display->GuestGpuCreated() && display->WaitForGuestGpu());
                std::puts("Guest GPU ready");
                DrawInterface(*display, device);
                std::puts("Guest interface drawn");
            }
            modules.StopSession();
            if (vulkan)
                std::puts("Guest released");
            modules.StopSession(); // Idempotent, including an unstarted guest.
            assert(!modules.IsValid() && modules.FrontendValid());
            assert(&modules.DisplayVideo() == display && &modules.Loader() == loader);
            if (vulkan)
            {
                VideoInterfaceDevice retained{};
                assert(display->AttachInterface(&retained));
                assert(retained.device == device.device && retained.instance == device.instance);
                DrawInterface(*display, retained);
            }
            modules.FlushSettings();
        }
        std::printf("frontend_ms,%.3f\nguest_setup_mean_ms,%.3f\n",
                    std::chrono::duration<double, std::milli>(hostReady - before).count(), guestMs / 8);
        std::printf("frontend_private_bytes,%zu\nfirst_guest_private_bytes,%zu\n", hostBytes, guestBytes);
        modules.ShutDown();
        modules.ShutDown();
        assert(!modules.FrontendValid());
        // A failed setup must release partially loaded DLLs and callbacks.
        const auto cpu = coreSettings.moduleCpu;
        coreSettings.moduleCpu = "missing-architecture-test-cpu.dll";
        assert(!modules.SetupFrontend(window));
        assert(!modules.FrontendValid());
        coreSettings.moduleCpu = cpu;
        assert(modules.SetupFrontend(window));
        assert(modules.StartSession());
    }
    AppCleanup();
    SetNotification(nullptr);
    // data is an explicitly created unique directory under the OS temp dir.
    std::filesystem::remove_all(data);
    std::puts("PASS real plugin lifecycle: 8 guest cycles, persistent host objects, partial setup recovery");
}
