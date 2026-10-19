// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstdlib>
#include <cstring>
#include <future>
#include <optional>
#include <span>
#include <vector>

#include "yuzu_common/common_types.h"
#include "yuzu_common/dynamic_library.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/polyfill_ranges.h"
#include "frontend/emu_window.h"
#include "yuzu_video_core/vulkan_common/vulkan_instance.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
namespace {

[[nodiscard]] bool AreExtensionsSupported(const std::vector<VkExtensionProperties>& properties,
                                          std::span<const char* const> extensions) {
    for (const char* extension : extensions) {
        const auto it = std::ranges::find_if(properties, [extension](const auto& prop) {
            return std::strcmp(extension, prop.extensionName) == 0;
        });
        if (it == properties.end()) {
            LOG_ERROR(Render_Vulkan, "Required instance extension {} is not available", extension);
            return false;
        }
    }
    return true;
}

#ifdef __APPLE__
struct MoltenVKSetting {
    const char* name;
    VkLayerSettingTypeEXT type;
    u32 value;
};

// MoltenVK configuration nxemu runs best with. An environment variable of the same name wins.
constexpr std::array MOLTENVK_SETTINGS{
    // Encode Metal commands on MoltenVK's own thread instead of blocking each submit
    MoltenVKSetting{"MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", VK_LAYER_SETTING_TYPE_BOOL32_EXT,
                    VK_FALSE},
    MoltenVKSetting{"MVK_CONFIG_RESUME_LOST_DEVICE", VK_LAYER_SETTING_TYPE_BOOL32_EXT, VK_TRUE},
};

[[nodiscard]] std::vector<VkLayerSettingEXT> MoltenVKLayerSettings() {
    std::vector<VkLayerSettingEXT> settings;
    for (const MoltenVKSetting& setting : MOLTENVK_SETTINGS) {
        if (std::getenv(setting.name) != nullptr) {
            continue;
        }
        settings.push_back({
            .pLayerName = "MoltenVK",
            .pSettingName = setting.name,
            .type = setting.type,
            .valueCount = 1,
            .pValues = &setting.value,
        });
    }
    return settings;
}
#endif

[[nodiscard]] std::vector<const char*> RequiredExtensions(
    const std::vector<VkExtensionProperties>& properties,
    Core::Frontend::WindowSystemType window_type, bool enable_validation) {
    std::vector<const char*> extensions;
    extensions.reserve(6);
    switch (window_type) {
    case Core::Frontend::WindowSystemType::Headless:
        break;
#ifdef _WIN32
    case Core::Frontend::WindowSystemType::Windows:
        extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
        break;
#elif defined(__APPLE__)
    case Core::Frontend::WindowSystemType::Cocoa:
        extensions.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
        break;
#elif defined(__ANDROID__)
    case Core::Frontend::WindowSystemType::Android:
        extensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
        break;
#else
    case Core::Frontend::WindowSystemType::X11:
        extensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
        break;
    case Core::Frontend::WindowSystemType::Wayland:
        extensions.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
        break;
#endif
    default:
        LOG_ERROR(Render_Vulkan, "Presentation not supported on this platform");
        break;
    }
    if (window_type != Core::Frontend::WindowSystemType::Headless) {
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    }
#ifdef __APPLE__
    if (AreExtensionsSupported(properties, std::array{VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME})) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }
    if (AreExtensionsSupported(dld, std::array{VK_EXT_LAYER_SETTINGS_EXTENSION_NAME})) {
        extensions.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
    }
#endif
    if (enable_validation &&
        AreExtensionsSupported(properties, std::array{VK_EXT_DEBUG_UTILS_EXTENSION_NAME})) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    return extensions;
}

[[nodiscard]] std::vector<const char*> Layers(bool enable_validation) {
    std::vector<const char*> layers;
    if (enable_validation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }
    return layers;
}

void RemoveUnavailableLayers(const vk::InstanceDispatch& dld, std::vector<const char*>& layers) {
    const std::optional layer_properties = vk::EnumerateInstanceLayerProperties(dld);
    if (!layer_properties) {
        LOG_ERROR(Render_Vulkan, "Failed to query layer properties, disabling layers");
        layers.clear();
    }
    std::erase_if(layers, [&layer_properties](const char* layer) {
        const auto comp = [layer](const VkLayerProperties& layer_property) {
            return std::strcmp(layer, layer_property.layerName) == 0;
        };
        const auto it = std::ranges::find_if(*layer_properties, comp);
        if (it == layer_properties->end()) {
            LOG_ERROR(Render_Vulkan, "Layer {} not available, removing it", layer);
            return true;
        }
        return false;
    });
}
} // Anonymous namespace

vk::Instance CreateInstance(const Common::DynamicLibrary& library, vk::InstanceDispatch& dld,
                            u32 required_version, Core::Frontend::WindowSystemType window_type,
                            bool enable_validation) {
    if (!library.IsOpen()) {
        LOG_ERROR(Render_Vulkan, "Vulkan library not available");
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    if (!library.GetSymbol("vkGetInstanceProcAddr", &dld.vkGetInstanceProcAddr)) {
        LOG_ERROR(Render_Vulkan, "vkGetInstanceProcAddr not present in Vulkan");
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    if (!vk::Load(dld)) {
        LOG_ERROR(Render_Vulkan, "Failed to load Vulkan function pointers");
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    const auto properties = vk::EnumerateInstanceExtensionProperties(dld);
    if (!properties) {
        throw vk::Exception(VK_ERROR_EXTENSION_NOT_PRESENT);
    }
    const std::vector<const char*> extensions =
        RequiredExtensions(*properties, window_type, enable_validation);
    if (!AreExtensionsSupported(*properties, extensions)) {
        throw vk::Exception(VK_ERROR_EXTENSION_NOT_PRESENT);
    }
    std::vector<const char*> layers = Layers(enable_validation);
    RemoveUnavailableLayers(dld, layers);

    const u32 available_version = vk::AvailableVersion(dld);
    if (available_version < required_version) {
        LOG_ERROR(Render_Vulkan, "Vulkan {}.{} is not supported, {}.{} is required",
                  VK_VERSION_MAJOR(available_version), VK_VERSION_MINOR(available_version),
                  VK_VERSION_MAJOR(required_version), VK_VERSION_MINOR(required_version));
        throw vk::Exception(VK_ERROR_INCOMPATIBLE_DRIVER);
    }
    const void* next = nullptr;
#ifdef __APPLE__
    const std::vector<VkLayerSettingEXT> layer_settings = MoltenVKLayerSettings();
    const VkLayerSettingsCreateInfoEXT layer_settings_ci{
        .sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
        .pNext = nullptr,
        .settingCount = static_cast<u32>(layer_settings.size()),
        .pSettings = layer_settings.data(),
    };
    if (std::ranges::find_if(extensions, [](const char* extension) {
            return std::strcmp(extension, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME) == 0;
        }) != extensions.end()) {
        next = &layer_settings_ci;
    }
#endif
    vk::Instance instance =
        std::async([&] {
            return vk::Instance::Create(available_version, layers, extensions, dld, next);
        }).get();
    if (!vk::Load(*instance, dld)) {
        LOG_ERROR(Render_Vulkan, "Failed to load Vulkan instance function pointers");
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    return instance;
}

} // namespace Vulkan
