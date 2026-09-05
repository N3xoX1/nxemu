// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Trimmed copy of video_core's Vulkan instance startup for the frontend crash
// sandbox. Enough for CheckVulkan (headless OpenLibrary + CreateInstance).

#include <array>
#include <cstdlib>
#include <cstring>
#include <future>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "yuzu_common/dynamic_library.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/polyfill_ranges.h"
#include "vulkan_probe.h"

#include <vulkan/vk_enum_string_helper.h>

namespace Vulkan {
namespace vk {
namespace {

template <typename T>
bool Proc(T& result, const InstanceDispatch& dld, const char* proc_name,
          VkInstance instance = nullptr) noexcept {
    result = reinterpret_cast<T>(dld.vkGetInstanceProcAddr(instance, proc_name));
    return result != nullptr;
}

} // namespace

const char* Exception::what() const noexcept {
    return string_VkResult(result);
}

bool Load(InstanceDispatch& dld) noexcept {
#define X(name) Proc(dld.name, dld, #name)
    return X(vkCreateInstance) && X(vkEnumerateInstanceExtensionProperties) &&
           X(vkEnumerateInstanceLayerProperties);
#undef X
}

bool Load(VkInstance instance, InstanceDispatch& dld) noexcept {
#define X(name) Proc(dld.name, dld, #name, instance)
    X(vkGetPhysicalDeviceFeatures2);
    X(vkGetPhysicalDeviceProperties2);
    X(vkGetPhysicalDeviceToolProperties);

    return X(vkCreateDevice) && X(vkDestroyDevice) && X(vkEnumerateDeviceExtensionProperties) &&
           X(vkEnumeratePhysicalDevices) && X(vkGetDeviceProcAddr) &&
           X(vkGetPhysicalDeviceFormatProperties) && X(vkGetPhysicalDeviceMemoryProperties) &&
           X(vkGetPhysicalDeviceMemoryProperties2) && X(vkGetPhysicalDeviceProperties) &&
           X(vkGetPhysicalDeviceQueueFamilyProperties);
#undef X
}

void Instance::Release() noexcept {
    if (handle) {
        dld->vkDestroyInstance(handle, nullptr);
        handle = nullptr;
    }
}

Instance Instance::Create(u32 version, Span<const char*> layers, Span<const char*> extensions,
                          InstanceDispatch& dispatch) {
    (void)version;
#ifdef __APPLE__
    constexpr VkFlags ci_flags{VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR};
#else
    constexpr VkFlags ci_flags{};
#endif

    const VkApplicationInfo application_info{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
        .pApplicationName = "yuzu Emulator",
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName = "yuzu Emulator",
        .engineVersion = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = ci_flags,
        .pApplicationInfo = &application_info,
        .enabledLayerCount = layers.size(),
        .ppEnabledLayerNames = layers.data(),
        .enabledExtensionCount = extensions.size(),
        .ppEnabledExtensionNames = extensions.data(),
    };
    VkInstance instance;
    Check(dispatch.vkCreateInstance(&ci, nullptr, &instance));
    if (!Proc(dispatch.vkDestroyInstance, dispatch, "vkDestroyInstance", instance)) {
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    return Instance(instance, dispatch);
}

u32 AvailableVersion(const InstanceDispatch& dld) noexcept {
    PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion;
    if (!Proc(vkEnumerateInstanceVersion, dld, "vkEnumerateInstanceVersion")) {
        return VK_API_VERSION_1_0;
    }
    u32 version;
    if (const VkResult result = vkEnumerateInstanceVersion(&version); result != VK_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "vkEnumerateInstanceVersion returned {}, assuming Vulkan 1.1",
                  string_VkResult(result));
        return VK_API_VERSION_1_1;
    }
    return version;
}

std::optional<std::vector<VkExtensionProperties>> EnumerateInstanceExtensionProperties(
    const InstanceDispatch& dld) {
    u32 num;
    if (dld.vkEnumerateInstanceExtensionProperties(nullptr, &num, nullptr) != VK_SUCCESS) {
        return std::nullopt;
    }
    std::vector<VkExtensionProperties> properties(num);
    if (dld.vkEnumerateInstanceExtensionProperties(nullptr, &num, properties.data()) !=
        VK_SUCCESS) {
        return std::nullopt;
    }
    return properties;
}

std::optional<std::vector<VkLayerProperties>> EnumerateInstanceLayerProperties(
    const InstanceDispatch& dld) {
    u32 num;
    if (dld.vkEnumerateInstanceLayerProperties(&num, nullptr) != VK_SUCCESS) {
        return std::nullopt;
    }
    std::vector<VkLayerProperties> properties(num);
    if (dld.vkEnumerateInstanceLayerProperties(&num, properties.data()) != VK_SUCCESS) {
        return std::nullopt;
    }
    return properties;
}

} // namespace vk

namespace {

[[nodiscard]] bool AreExtensionsSupported(const vk::InstanceDispatch& dld,
                                          std::span<const char* const> extensions) {
    const std::optional properties = vk::EnumerateInstanceExtensionProperties(dld);
    if (!properties) {
        LOG_ERROR(Render_Vulkan, "Failed to query extension properties");
        return false;
    }
    for (const char* extension : extensions) {
        const auto it = std::ranges::find_if(*properties, [extension](const auto& prop) {
            return std::strcmp(extension, prop.extensionName) == 0;
        });
        if (it == properties->end()) {
            LOG_ERROR(Render_Vulkan, "Required instance extension {} is not available", extension);
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::vector<const char*> RequiredExtensions(const vk::InstanceDispatch& dld,
                                                          WindowSystemType window_type,
                                                          bool enable_validation) {
    std::vector<const char*> extensions;
    extensions.reserve(6);
    switch (window_type) {
    case WindowSystemType::Headless:
        break;
    }
    if (window_type != WindowSystemType::Headless) {
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    }
#ifdef __APPLE__
    if (AreExtensionsSupported(dld, std::array{VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME})) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }
#endif
    if (enable_validation &&
        AreExtensionsSupported(dld, std::array{VK_EXT_DEBUG_UTILS_EXTENSION_NAME})) {
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

} // namespace

std::shared_ptr<Common::DynamicLibrary> OpenLibrary() {
    LOG_DEBUG(Render_Vulkan, "Looking for a Vulkan library");
    auto library = std::make_shared<Common::DynamicLibrary>();
#ifdef __APPLE__
    const auto libvulkan_filename =
        Common::FS::GetBundleDirectory() / "Contents/Frameworks/libvulkan.1.dylib";
    const auto libmoltenvk_filename =
        Common::FS::GetBundleDirectory() / "Contents/Frameworks/libMoltenVK.dylib";
    const char* library_paths[] = {std::getenv("LIBVULKAN_PATH"), libvulkan_filename.c_str(),
                                   libmoltenvk_filename.c_str()};
    for (const auto& library_path : library_paths) {
        if (library_path && library->Open(library_path)) {
            break;
        }
    }
#else
    std::string filename = Common::DynamicLibrary::GetVersionedFilename("vulkan", 1);
    LOG_DEBUG(Render_Vulkan, "Trying Vulkan library: {}", filename);
    if (!library->Open(filename.c_str())) {
        filename = Common::DynamicLibrary::GetVersionedFilename("vulkan");
        LOG_DEBUG(Render_Vulkan, "Trying Vulkan library (second attempt): {}", filename);
        void(library->Open(filename.c_str()));
    }
#endif
    return library;
}

vk::Instance CreateInstance(const Common::DynamicLibrary& library, vk::InstanceDispatch& dld,
                            u32 required_version, WindowSystemType window_type,
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
    const std::vector<const char*> extensions =
        RequiredExtensions(dld, window_type, enable_validation);
    if (!AreExtensionsSupported(dld, extensions)) {
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
    vk::Instance instance =
        std::async([&] {
            return vk::Instance::Create(available_version, layers, extensions, dld);
        }).get();
    if (!vk::Load(*instance, dld)) {
        LOG_ERROR(Render_Vulkan, "Failed to load Vulkan instance function pointers");
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    return instance;
}

} // namespace Vulkan
