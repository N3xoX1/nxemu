#include "settings/ui_settings.h"
#include "startup_checks.h"
#include "yuzu_common/dynamic_library.h"
#include "yuzu_common/fs/path_util.h"
#include "vulkan/vulkan_core.h"

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#include <vulkan/vulkan_win32.h>
#endif

constexpr char IS_CHILD_ENV_VAR[] = "NXEMU_IS_CHILD";
constexpr char STARTUP_CHECK_ENV_VAR[] = "NXEMU_DO_STARTUP_CHECKS";
constexpr char ENV_VAR_ENABLED_TEXT[] = "ON";

namespace 
{
    HMODULE OpenVulkanLibrary()
    {
        HMODULE library = LoadLibrary(L"vulkan-1.dll");
        if (library == nullptr)
        {
            library = LoadLibrary(L"vulkan.dll");
        }

        if (library == nullptr)
        {
            throw std::runtime_error("Vulkan library not available");
        }
        return library;
    }

    VkInstance CreateVulkanInstance(HMODULE library, u32 required_version, bool enable_validation) 
    {
        if (library == nullptr)
        {
            throw std::runtime_error("Vulkan library not available");
        }

        PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(library, "vkGetInstanceProcAddr");
        if (vkGetInstanceProcAddr == nullptr)
        {
            throw std::runtime_error("vkGetInstanceProcAddr not found");
        }

        PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
        PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
        PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");

        if (vkCreateInstance == nullptr)
        {
            throw std::runtime_error("Failed to get vkCreateInstance");
        }

        std::vector<const char*> required_exts = {
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
            VK_KHR_SURFACE_EXTENSION_NAME,
        };

        if (enable_validation) 
        {
            required_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        if (vkEnumerateInstanceExtensionProperties != nullptr)
        {
            u32 extensionCount = 0;
            vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);

            std::vector<VkExtensionProperties> available_extensions(extensionCount);
            vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, available_extensions.data());

            for (const char * required_ext : required_exts)
            {
                bool found = false;
                for (const VkExtensionProperties & available_ext : available_extensions)
                {
                    if (strcmp(required_ext, available_ext.extensionName) == 0) 
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    throw std::runtime_error(std::string("Required extension not available: ") + required_ext);
                }
            }
        }

        std::vector<const char*> layers;
        if (enable_validation) 
        {
            layers.push_back("VK_LAYER_KHRONOS_validation");
        }

        u32 available_version = VK_API_VERSION_1_0;
        if (vkEnumerateInstanceVersion) 
        {
            vkEnumerateInstanceVersion(&available_version);
        }

        if (available_version < required_version) 
        {
            throw std::runtime_error("Incompatible Vulkan driver version");
        }

        VkApplicationInfo app_info = {};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "My Application";
        app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app_info.pEngineName = "Engine";
        app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        app_info.apiVersion = available_version;

        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;
        create_info.enabledExtensionCount = static_cast<uint32_t>(required_exts.size());
        create_info.ppEnabledExtensionNames = required_exts.data();
        create_info.enabledLayerCount = static_cast<uint32_t>(layers.size());
        create_info.ppEnabledLayerNames = layers.data();

        VkInstance instance;
        VkResult result = vkCreateInstance(&create_info, nullptr, &instance);
        if (result != VK_SUCCESS) 
        {
            throw std::runtime_error("Failed to create Vulkan instance");
        }

        return instance;
    }

    std::vector<VkPhysicalDevice> EnumeratePhysicalDevices(VkInstance instance, PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr) 
    {
        PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)vkGetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices");
        if (!vkEnumeratePhysicalDevices) 
        {
            throw std::runtime_error("Failed to load vkEnumeratePhysicalDevices");
        }

        u32 num;
        VkResult result = vkEnumeratePhysicalDevices(instance, &num, nullptr);
        if (result != VK_SUCCESS) 
        {
            throw std::runtime_error("Failed to enumerate physical devices");
        }

        std::vector<VkPhysicalDevice> physical_devices(num);
        result = vkEnumeratePhysicalDevices(instance, &num, physical_devices.data());
        if (result != VK_SUCCESS) 
        {
            throw std::runtime_error("Failed to get physical devices");
        }

        if (physical_devices.size() > 1) 
        {
            PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties");

            if (vkGetPhysicalDeviceProperties) 
            {
                std::stable_sort(physical_devices.begin(), physical_devices.end(),
                    [vkGetPhysicalDeviceProperties](VkPhysicalDevice lhs, VkPhysicalDevice rhs) {
                        VkPhysicalDeviceProperties lhs_props, rhs_props;
                        vkGetPhysicalDeviceProperties(lhs, &lhs_props);
                        vkGetPhysicalDeviceProperties(rhs, &rhs_props);

                        // Prefer discrete GPU over integrated
                        bool lhs_discrete = (lhs_props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
                        bool rhs_discrete = (rhs_props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);

                        if (lhs_discrete != rhs_discrete) 
                        {
                            return lhs_discrete; // Discrete GPU wins
                        }

                        // If both same type, sort by name (like original)
                        return std::string_view{ lhs_props.deviceName } > std::string_view{ rhs_props.deviceName };
                    });
            }
        }
        return physical_devices;
    }

    constexpr bool CheckBrokenCompute(VkDriverId driver_id, u32 driver_version) 
    {
        if (driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS) 
        {
            const u32 major = VK_API_VERSION_MAJOR(driver_version);
            const u32 minor = VK_API_VERSION_MINOR(driver_version);
            const u32 patch = VK_API_VERSION_PATCH(driver_version);
            if (major == 0 && minor == 405 && patch < 286) 
            {
                fprintf(stderr, "Warning: Intel proprietary drivers 0.405.0 until 0.405.286 have broken compute\n");
                return true;
            }
        }
        return false;
    }
};

bool CheckVulkan() 
{
    // Just start the Vulkan loader, this will crash if something is wrong
    bool created = false;
    HMODULE library = nullptr;
    try 
    {
        library = OpenVulkanLibrary();
        VkInstance instance = CreateVulkanInstance(library, VK_API_VERSION_1_1, false);
        if (instance != nullptr)
        {
            created = true;
            PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(GetProcAddress(library, "vkGetInstanceProcAddr"));
            PFN_vkDestroyInstance vkDestroyInstance = (PFN_vkDestroyInstance)vkGetInstanceProcAddr(instance, "vkDestroyInstance");
            if (vkDestroyInstance != nullptr)
            {
                vkDestroyInstance(instance, nullptr);
            }
        }
    }
    catch (const std::runtime_error & e)
    {
       fprintf(stderr, "Failed to initialize Vulkan: %s\n", e.what());
    }
    if (library != nullptr)
    {
        FreeLibrary(library);
    }
    return created;
}

#ifdef _WIN32
bool SpawnChild(const char* arg0, PROCESS_INFORMATION* pi, int flags) {
    STARTUPINFOA startup_info;

    std::memset(&startup_info, '\0', sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);

    char p_name[255];
    std::strncpy(p_name, arg0, 254);
    p_name[254] = '\0';

    const bool process_created = CreateProcessA(nullptr,       // lpApplicationName
        p_name,        // lpCommandLine
        nullptr,       // lpProcessAttributes
        nullptr,       // lpThreadAttributes
        false,         // bInheritHandles
        flags,         // dwCreationFlags
        nullptr,       // lpEnvironment
        nullptr,       // lpCurrentDirectory
        &startup_info, // lpStartupInfo
        pi             // lpProcessInformation
    );
    if (!process_created) 
    {
        fprintf(stderr, "CreateProcessA failed with error %d\n", GetLastError());
        return false;
    }

    return true;
}
#endif

VulkanCheckResult RunVulkanProbeIfChild()
{
#ifdef _WIN32
    char variable_contents[8];
    const DWORD startup_check_var = GetEnvironmentVariableA(STARTUP_CHECK_ENV_VAR, variable_contents, 8);
    if (startup_check_var > 0 && std::strncmp(variable_contents, ENV_VAR_ENABLED_TEXT, 8) == 0)
    {
        return CheckVulkan() ? EXIT_VULKAN_AVAILABLE : EXIT_VULKAN_NOT_AVAILABLE;
    }

    char is_child_s[8];
    const DWORD is_child_len = GetEnvironmentVariableA(IS_CHILD_ENV_VAR, is_child_s, 8);
    if (is_child_len > 0 && std::strncmp(is_child_s, ENV_VAR_ENABLED_TEXT, 8) == 0)
    {
        return VULKAN_CHECK_DONE;
    }
    else if (!SetEnvironmentVariableA(IS_CHILD_ENV_VAR, ENV_VAR_ENABLED_TEXT))
    {
        return EXIT_VULKAN_NOT_AVAILABLE;
    }
#elif defined(NXEMU_UNIX)
    const char* startup_check_var = getenv(STARTUP_CHECK_ENV_VAR);
    if (startup_check_var != nullptr &&
        std::strncmp(startup_check_var, ENV_VAR_ENABLED_TEXT, 8) == 0) {
        return CheckVulkan() ? EXIT_VULKAN_AVAILABLE : EXIT_VULKAN_NOT_AVAILABLE;
    }
#endif
    return VULKAN_CHECK_DONE;
}

VulkanCheckResult StartupVulkanChecks()
{
    const VulkanCheckResult childResult = RunVulkanProbeIfChild();
    if (childResult != VULKAN_CHECK_DONE)
    {
        return childResult;
    }

#ifdef _WIN32
    // Set the startup variable for child processes
    const bool env_var_set = SetEnvironmentVariableA(STARTUP_CHECK_ENV_VAR, ENV_VAR_ENABLED_TEXT);
    if (!env_var_set) {
        fprintf(stderr, "SetEnvironmentVariableA failed to set %s with error %d\n", STARTUP_CHECK_ENV_VAR, GetLastError());
        return VULKAN_CHECK_DONE;
    }

    // Spawn child process that performs Vulkan check
    PROCESS_INFORMATION process_info;
    memset(&process_info, '\0', sizeof(process_info));

    if (!SpawnChild(__argv[0], &process_info, 0))
    {
        return VULKAN_CHECK_DONE;
    }

    // Wait until the process exits and get exit code from it
    WaitForSingleObject(process_info.hProcess, INFINITE);
    DWORD exit_code = STILL_ACTIVE;
    const int err = GetExitCodeProcess(process_info.hProcess, &exit_code);
    if (err == 0) 
    {
        fprintf(stderr, "GetExitCodeProcess failed with error %d\n", GetLastError());
    }

    // Vulkan is broken if the child crashed (return value is not zero)
    uiSettings.hasBrokenVulkan = (exit_code != 0);

    if (CloseHandle(process_info.hProcess) == 0) 
    {
        fprintf(stderr, "CloseHandle failed with error %d\n", GetLastError());
    }
    if (CloseHandle(process_info.hThread) == 0) 
    {
        fprintf(stderr, "CloseHandle failed with error %d\n", GetLastError());
    }

    if (!SetEnvironmentVariableA(STARTUP_CHECK_ENV_VAR, nullptr)) 
    {
        fprintf(stderr, "SetEnvironmentVariableA failed to clear %s with error %d\n",STARTUP_CHECK_ENV_VAR, GetLastError());
    }
#endif
    return VULKAN_CHECK_DONE;
}

VkDeviceRecord::VkDeviceRecord(const std::string & name_, const std::vector<uint32_t> & vsync_modes_, bool has_broken_compute_) :
    name{ name_ }, 
    vsync_support{ vsync_modes_ }, 
    has_broken_compute{ has_broken_compute_ } 
{
}

VkDeviceRecord::~VkDeviceRecord()
{
}

void PopulateVulkanRecords(std::vector<VkDeviceRecord> & records, void * renderSurface)
{
    HMODULE library = nullptr;
    try
    {
        library = OpenVulkanLibrary();
        VkInstance instance = CreateVulkanInstance(library, VK_API_VERSION_1_1, false);

        PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(GetProcAddress(library, "vkGetInstanceProcAddr"));

        std::vector<VkPhysicalDevice> physical_devices = EnumeratePhysicalDevices(instance, vkGetInstanceProcAddr);
        PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR");
        PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR = (PFN_vkDestroySurfaceKHR)vkGetInstanceProcAddr(instance, "vkDestroySurfaceKHR");

        if (!vkCreateWin32SurfaceKHR) 
        {
            throw std::runtime_error("Failed to load vkCreateWin32SurfaceKHR");
        }

        VkWin32SurfaceCreateInfoKHR surface_create_info = {};
        surface_create_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        surface_create_info.hinstance = GetModuleHandle(nullptr);
        surface_create_info.hwnd = (HWND)renderSurface;

        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkResult result = vkCreateWin32SurfaceKHR(instance, &surface_create_info, nullptr, &surface);
        if (result != VK_SUCCESS) 
        {
            throw std::runtime_error("Failed to create surface");
        }

        PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties");
        PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2");
        PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");

        records.clear();
        records.reserve(physical_devices.size());

        for (const VkPhysicalDevice device : physical_devices) 
        {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(device, &properties);
            std::string name = properties.deviceName;

            std::vector<uint32_t> present_modes;
            if (vkGetPhysicalDeviceSurfacePresentModesKHR) 
            {
                uint32_t present_mode_count = 0;
                vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &present_mode_count, nullptr);

                if (present_mode_count > 0)
                {
                    present_modes.resize(present_mode_count);
                    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &present_mode_count, (VkPresentModeKHR *)present_modes.data());
                }
            }

            VkPhysicalDeviceDriverProperties driver_properties = {};
            driver_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
            driver_properties.pNext = nullptr;

            VkPhysicalDeviceProperties2 properties2 = {};
            properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties2.pNext = &driver_properties;

            if (vkGetPhysicalDeviceProperties2) 
            {
                vkGetPhysicalDeviceProperties2(device, &properties2);
            }
            bool has_broken_compute = CheckBrokenCompute(driver_properties.driverID, properties2.properties.driverVersion);
            records.push_back(VkDeviceRecord(name, present_modes, has_broken_compute));
        }
        if (surface != VK_NULL_HANDLE && vkDestroySurfaceKHR) 
        {
            vkDestroySurfaceKHR(instance, surface, nullptr);
        }
    }
    catch (const std::runtime_error& e)
    {
        fprintf(stderr, "Failed to initialize Vulkan: %s\n", e.what());
    }
    if (library != nullptr)
    {
        FreeLibrary(library);
    }
}