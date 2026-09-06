// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "yuzu_common/common_types.h"
#include "yuzu_common/dynamic_library.h"

#define VK_NO_PROTOTYPES
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__APPLE__)
#define VK_USE_PLATFORM_METAL_EXT
#elif defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR
#else
#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#include <vulkan/vulkan.h>
#undef CreateEvent
#undef CreateSemaphore
#undef Always
#undef False
#undef None
#undef True

namespace Vulkan {

// Probe-only copy of video_core's instance startup. CheckVulkan uses Headless,
// matching CreateInstance's default in vulkan_instance.h.
enum class WindowSystemType {
    Headless,
};

namespace vk {

template <typename T>
class Span {
public:
    constexpr Span() noexcept = default;
    constexpr Span(std::nullptr_t) noexcept {}
    constexpr Span(const T& value) noexcept : ptr{&value}, num{1} {}
    template <typename Range>
    constexpr Span(const Range& range) : ptr{std::data(range)}, num{std::size(range)} {}
    constexpr Span(const T* ptr_, std::size_t num_) noexcept : ptr{ptr_}, num{num_} {}

    constexpr const T* data() const noexcept {
        return ptr;
    }
    constexpr u32 size() const noexcept {
        return static_cast<u32>(num);
    }

private:
    const T* ptr = nullptr;
    std::size_t num = 0;
};

class Exception final : public std::exception {
public:
    explicit Exception(VkResult result_) : result{result_} {}
    const char* what() const noexcept override;
    VkResult GetResult() const noexcept {
        return result;
    }

private:
    VkResult result;
};

inline void Check(VkResult result) {
    if (result != VK_SUCCESS) {
        throw Exception(result);
    }
}

struct InstanceDispatch {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr{};
    PFN_vkCreateInstance vkCreateInstance{};
    PFN_vkDestroyInstance vkDestroyInstance{};
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties{};
    PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties{};
    PFN_vkCreateDevice vkCreateDevice{};
    PFN_vkDestroyDevice vkDestroyDevice{};
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties{};
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices{};
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr{};
    PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2{};
    PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties{};
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties{};
    PFN_vkGetPhysicalDeviceMemoryProperties2 vkGetPhysicalDeviceMemoryProperties2{};
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties{};
    PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2{};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties{};
    PFN_vkGetPhysicalDeviceToolProperties vkGetPhysicalDeviceToolProperties{};
};

bool Load(InstanceDispatch& dld) noexcept;
bool Load(VkInstance instance, InstanceDispatch& dld) noexcept;
u32 AvailableVersion(const InstanceDispatch& dld) noexcept;
std::optional<std::vector<VkExtensionProperties>> EnumerateInstanceExtensionProperties(
    const InstanceDispatch& dld);
std::optional<std::vector<VkLayerProperties>> EnumerateInstanceLayerProperties(
    const InstanceDispatch& dld);

class Instance {
public:
    Instance() noexcept = default;
    explicit Instance(VkInstance handle_, const InstanceDispatch& dld_) noexcept
        : handle{handle_}, dld{&dld_} {}
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
    Instance(Instance&& rhs) noexcept
        : handle{std::exchange(rhs.handle, nullptr)}, dld{rhs.dld} {}
    Instance& operator=(Instance&& rhs) noexcept {
        Release();
        handle = std::exchange(rhs.handle, nullptr);
        dld = rhs.dld;
        return *this;
    }
    ~Instance() noexcept {
        Release();
    }

    static Instance Create(u32 version, Span<const char*> layers, Span<const char*> extensions,
                           InstanceDispatch& dispatch);

    VkInstance operator*() const noexcept {
        return handle;
    }

private:
    void Release() noexcept;
    VkInstance handle = nullptr;
    const InstanceDispatch* dld = nullptr;
};

} // namespace vk

std::shared_ptr<Common::DynamicLibrary> OpenLibrary();

[[nodiscard]] vk::Instance CreateInstance(
    const Common::DynamicLibrary& library, vk::InstanceDispatch& dld, u32 required_version,
    WindowSystemType window_type = WindowSystemType::Headless, bool enable_validation = false);

} // namespace Vulkan
