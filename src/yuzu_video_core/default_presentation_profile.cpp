// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <exception>
#include <string_view>

#if !defined(ANDROID) && !defined(__ANDROID__)
#include <filesystem>
#include <fstream>
#include <string>
#endif

#if defined(__APPLE__)
#include <dlfcn.h>
#include <sys/sysctl.h>
#endif

#include "yuzu_common/logging/log.h"
#include "frontend/emu_window.h"
#include "yuzu_video_core/default_presentation_profile.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"
#include "yuzu_video_core/vulkan_common/vulkan_instance.h"
#include "yuzu_video_core/vulkan_common/vulkan_library.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCore {
namespace {

#if !defined(ANDROID) && !defined(__ANDROID__)
bool IsPortableHost() {
#if defined(_WIN32)
    // The ACPI platform role avoids false positives from desktop UPS units.
    // Values match POWER_PLATFORM_ROLE in <winnt.h>.
    constexpr ULONG kPlatformRoleV2 = 2;
    constexpr int kPlatformRoleMobile = 2;
    constexpr int kPlatformRoleSlate = 8;
    using DetermineRoleExFn = int(WINAPI*)(ULONG);
    using DetermineRoleFn = int(WINAPI*)();
    if (HMODULE powrprof = LoadLibraryA("powrprof.dll"); powrprof != nullptr) {
        int role = 0;
        if (const auto fn_ex = reinterpret_cast<DetermineRoleExFn>(
                GetProcAddress(powrprof, "PowerDeterminePlatformRoleEx"));
            fn_ex != nullptr) {
            role = fn_ex(kPlatformRoleV2);
        } else if (const auto fn = reinterpret_cast<DetermineRoleFn>(
                       GetProcAddress(powrprof, "PowerDeterminePlatformRole"));
                   fn != nullptr) {
            role = fn();
        }
        FreeLibrary(powrprof);
        if (role != 0) {
            return role == kPlatformRoleMobile || role == kPlatformRoleSlate;
        }
    }
    SYSTEM_POWER_STATUS status{};
    if (GetSystemPowerStatus(&status)) {
        // 128 = no system battery, 255 = unknown status.
        return status.BatteryFlag != 128 && status.BatteryFlag != 255;
    }
    return false;
#elif defined(__APPLE__)
    char model[64]{};
    size_t size = sizeof(model) - 1;
    if (sysctlbyname("hw.model", model, &size, nullptr, 0) == 0 &&
        std::strncmp(model, "MacBook", 7) == 0) {
        return true;
    }
    // Apple Silicon MacBooks use numeric model IDs, so look for an internal battery in IOKit.
    void* iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
    if (iokit == nullptr) {
        return false;
    }
    using MatchingFn = void* (*)(const char*);
    using GetServiceFn = uint32_t (*)(uint32_t, void*);
    using ReleaseFn = int (*)(uint32_t);
    const auto matching = reinterpret_cast<MatchingFn>(dlsym(iokit, "IOServiceMatching"));
    const auto get_service =
        reinterpret_cast<GetServiceFn>(dlsym(iokit, "IOServiceGetMatchingService"));
    const auto release = reinterpret_cast<ReleaseFn>(dlsym(iokit, "IOObjectRelease"));
    bool has_internal_battery = false;
    if (matching != nullptr && get_service != nullptr && release != nullptr) {
        if (void* dict = matching("AppleSmartBattery"); dict != nullptr) {
            const uint32_t service = get_service(0, dict);
            if (service != 0) {
                has_internal_battery = true;
                release(service);
            }
        }
    }
    dlclose(iokit);
    return has_internal_battery;
#else
    // SMBIOS chassis type values (DMTF DSP0134): Portable (8), Laptop (9), Notebook (10),
    // Hand Held (11), Sub Notebook (14), Tablet (30), Convertible (31), Detachable (32).
    if (std::ifstream chassis("/sys/class/dmi/id/chassis_type"); chassis.is_open()) {
        int type = 0;
        if (chassis >> type) {
            switch (type) {
            case 8:
            case 9:
            case 10:
            case 11:
            case 14:
            case 30:
            case 31:
            case 32:
                return true;
            default:
                return false;
            }
        }
    }
    // Non-DMI Linux fallback (e.g. ARM devices): look for a system battery.
    std::error_code ec;
    const std::filesystem::path power_root("/sys/class/power_supply");
    if (!std::filesystem::is_directory(power_root, ec)) {
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(power_root, ec)) {
        std::ifstream type_file(entry.path() / "type");
        std::string type;
        if (!std::getline(type_file, type) || type != "Battery") {
            continue;
        }
        std::ifstream scope_file(entry.path() / "scope");
        std::string scope;
        if (std::getline(scope_file, scope) && scope == "Device") {
            continue; // Skip wireless peripheral batteries.
        }
        return true;
    }
    return false;
#endif
}
#endif

} // namespace

ScalingFilter DetectDefaultPresentationProfile(int32_t vulkan_device_index) {
    DefaultPresentationProfileInput input{};
#if defined(ANDROID) || defined(__ANDROID__)
    (void)vulkan_device_index;
    input.is_mobile = true;
    return DefaultPresentationProfile(input);
#else
    input.is_portable = IsPortableHost();
    if (input.is_portable) {
        LOG_INFO(Render_Vulkan, "Default presentation profile: Performance (portable host)");
        return DefaultPresentationProfile(input);
    }
    try {
        const auto library = Vulkan::OpenLibrary();
        if (!library || !library->IsOpen()) {
            return DefaultPresentationProfile(input);
        }
        Vulkan::vk::InstanceDispatch dld{};
        const auto instance = Vulkan::CreateInstance(
            *library, dld, VK_API_VERSION_1_1, Core::Frontend::WindowSystemType::Headless, false);
        const auto devices = instance.EnumeratePhysicalDevices();
        if (devices.empty()) {
            return DefaultPresentationProfile(input);
        }
        const uint64_t index =
            vulkan_device_index >= 0 && static_cast<uint64_t>(vulkan_device_index) < devices.size()
                ? static_cast<uint64_t>(vulkan_device_index)
                : 0;
        const Vulkan::vk::PhysicalDevice physical{devices[index], dld};
        const VkPhysicalDeviceProperties props = physical.GetProperties();

        switch (props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            input.gpu_kind = DefaultProfileGpuKind::Discrete;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            input.gpu_kind = DefaultProfileGpuKind::Integrated;
            break;
        default:
            input.gpu_kind = DefaultProfileGpuKind::Other;
            break;
        }

        input.supports_vulkan_1_2 = props.apiVersion >= VK_API_VERSION_1_2;

        const std::string_view device_name{props.deviceName};
        if (device_name.find("Microsoft") != std::string_view::npos ||
            device_name.find("llvmpipe") != std::string_view::npos ||
            device_name.find("SwiftShader") != std::string_view::npos) {
            input.is_software_or_emulated = true;
        }

        if (input.supports_vulkan_1_2) {
            VkPhysicalDeviceDriverProperties driver_props{};
            driver_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
            VkPhysicalDeviceProperties2 props2{};
            props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            props2.pNext = &driver_props;
            physical.GetProperties2(props2);
            const VkDriverId driver_id = driver_props.driverID;
            switch (driver_id) {
            case VK_DRIVER_ID_MESA_LLVMPIPE:
            case VK_DRIVER_ID_GOOGLE_SWIFTSHADER:
            case VK_DRIVER_ID_MESA_DOZEN:
            case VK_DRIVER_ID_MESA_VENUS:
                input.is_software_or_emulated = true;
                break;
            default:
                break;
            }

            VkPhysicalDeviceShaderFloat16Int8Features float16_features{};
            float16_features.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
            VkPhysicalDeviceFeatures2 features2{};
            features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features2.pNext = &float16_features;
            physical.GetFeatures2(features2);
            // Use the same FP16 decision as the renderer, which turns FP16 off on some drivers.
            const Vulkan::NvidiaArchitecture nvidia_arch =
                driver_id == VK_DRIVER_ID_NVIDIA_PROPRIETARY
                    ? Vulkan::DetectNvidiaArchitecture(physical)
                    : Vulkan::NvidiaArchitecture::Arch_KeplerOrOlder;
            input.supports_fast_fp16 =
                float16_features.shaderFloat16 == VK_TRUE &&
                features2.features.shaderInt16 == VK_TRUE &&
                !Vulkan::HasBrokenFloat16Math(driver_id, nvidia_arch);
        }

        const auto mem_props = physical.GetMemoryProperties().memoryProperties;
        uint64_t device_local_bytes = 0;
        for (uint32_t i = 0; i < mem_props.memoryHeapCount; ++i) {
            if ((mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
                device_local_bytes += mem_props.memoryHeaps[i].size;
            }
        }
        input.device_local_memory_bytes = device_local_bytes;

        const auto color_fmt =
            physical.GetFormatProperties(VK_FORMAT_R16G16B16A16_SFLOAT).optimalTilingFeatures;
        const auto integer_fmt =
            physical.GetFormatProperties(VK_FORMAT_R32_UINT).optimalTilingFeatures;
        const auto edge_fmt =
            physical.GetFormatProperties(VK_FORMAT_R8_UINT).optimalTilingFeatures;
        constexpr VkFormatFeatureFlags kColorRequired =
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        constexpr VkFormatFeatureFlags kIntegerRequired =
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT;
        input.supports_cmaa2_formats =
            (color_fmt & kColorRequired) == kColorRequired &&
            (integer_fmt & kIntegerRequired) == kIntegerRequired &&
            (edge_fmt & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;

        const ScalingFilter selected = DefaultPresentationProfile(input);
        LOG_INFO(Render_Vulkan,
                 "Default presentation profile: {} (gpu='{}', discrete={}, vram={} MiB, fp16={})",
                 selected == ScalingFilter::AutoQuality ? "Balanced" : "Performance", device_name,
                 input.gpu_kind == DefaultProfileGpuKind::Discrete,
                 device_local_bytes / (1024ULL * 1024ULL), input.supports_fast_fp16);
        return selected;
    } catch (const std::exception& e) {
        LOG_WARNING(Render_Vulkan,
                    "Failed to probe GPU for default presentation profile, using Performance: {}",
                    e.what());
        return ScalingFilter::AutoPerformance;
    }
#endif
}

} // namespace VideoCore
