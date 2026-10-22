// SPDX-License-Identifier: GPL-2.0-or-later
// The validation callback rejects the invalid acquire before driver dispatch.
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include <windows.h>

static unsigned errors{}, pending_errors{};
static VKAPI_ATTR VkBool32 VKAPI_CALL Message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void*) {
    if (!(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT))
        return VK_FALSE;
    ++errors;
    if (data->pMessageIdName && std::strstr(data->pMessageIdName, "01779"))
        ++pending_errors;
    std::printf("validation: %s\n", data->pMessageIdName ? data->pMessageIdName : data->pMessage);
    return VK_TRUE;
}
static void Check(VkResult result) {
    if (result != VK_SUCCESS)
        throw std::runtime_error("Vulkan result " + std::to_string(result));
}

int main() try {
    const HINSTANCE module = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = module;
    wc.lpszClassName = L"NXAcquireLifetimeTest";
    if (!RegisterClassW(&wc))
        throw std::runtime_error("RegisterClass failed");
    HWND window = CreateWindowW(wc.lpszClassName, L"Vulkan acquire test", WS_OVERLAPPEDWINDOW, 0, 0,
                                256, 256, nullptr, nullptr, module, nullptr);
    if (!window)
        throw std::runtime_error("CreateWindow failed");
    const char* layer = "VK_LAYER_KHRONOS_validation";
    const std::array extensions{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
                                VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkDebugUtilsMessengerCreateInfoEXT debug{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    debug.pfnUserCallback = Message;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pNext = &debug;
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = &layer;
    ici.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ici.ppEnabledExtensionNames = extensions.data();
    VkInstance instance;
    Check(vkCreateInstance(&ici, nullptr, &instance));
    auto create_debug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    auto destroy_debug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    VkDebugUtilsMessengerEXT messenger;
    Check(create_debug(instance, &debug, nullptr, &messenger));
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = module;
    sci.hwnd = window;
    VkSurfaceKHR surface;
    Check(vkCreateWin32SurfaceKHR(instance, &sci, nullptr, &surface));
    uint32_t count{};
    Check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    VkPhysicalDevice physical{};
    uint32_t family{};
    for (auto candidate : devices) {
        uint32_t families;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
        std::vector<VkQueueFamilyProperties> props(families);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, props.data());
        for (uint32_t i = 0; i < families; ++i) {
            VkBool32 supported;
            Check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &supported));
            if (supported && (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                physical = candidate;
                family = i;
                break;
            }
        }
        if (physical)
            break;
    }
    if (!physical)
        throw std::runtime_error("No graphics/present device");
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    std::printf("Device: %s\n", props.deviceName);
    const float priority = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    timeline.timelineSemaphore = VK_TRUE;
    const char* swapchain_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &timeline;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = &swapchain_extension;
    VkDevice device;
    Check(vkCreateDevice(physical, &dci, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkSurfaceCapabilitiesKHR caps;
    Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
    Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(count);
    Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()));
    VkSwapchainCreateInfoKHR swap_info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swap_info.surface = surface;
    swap_info.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && swap_info.minImageCount > caps.maxImageCount)
        swap_info.minImageCount = caps.maxImageCount;
    swap_info.imageFormat = formats[0].format;
    swap_info.imageColorSpace = formats[0].colorSpace;
    swap_info.imageExtent = caps.currentExtent;
    swap_info.imageArrayLayers = 1;
    swap_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swap_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swap_info.preTransform = caps.currentTransform;
    swap_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swap_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swap_info.clipped = VK_TRUE;
    VkSwapchainKHR swapchain;
    Check(vkCreateSwapchainKHR(device, &swap_info, nullptr, &swapchain));
    VkSemaphoreCreateInfo sem_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acquired, gate;
    Check(vkCreateSemaphore(device, &sem_info, nullptr, &acquired));
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    sem_info.pNext = &type;
    Check(vkCreateSemaphore(device, &sem_info, nullptr, &gate));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence done;
    Check(vkCreateFence(device, &fci, nullptr, &done));
    uint32_t image;
    Check(vkAcquireNextImageKHR(device, swapchain, 1000000000, acquired, VK_NULL_HANDLE, &image));
    const std::array waits{acquired, gate};
    const std::array<uint64_t, 2> values{0, 1};
    const std::array<VkPipelineStageFlags, 2> stages{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    VkTimelineSemaphoreSubmitInfo tsi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    tsi.waitSemaphoreValueCount = 2;
    tsi.pWaitSemaphoreValues = values.data();
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.pNext = &tsi;
    submit.waitSemaphoreCount = 2;
    submit.pWaitSemaphores = waits.data();
    submit.pWaitDstStageMask = stages.data();
    Check(vkQueueSubmit(queue, 1, &submit, done));
    // Keep the wait pending deterministically, rather than relying on GPU load.
    const auto before = errors;
    const VkResult rejected =
        vkAcquireNextImageKHR(device, swapchain, 0, acquired, VK_NULL_HANDLE, &image);
    const bool detected =
        errors > before && pending_errors && rejected == VK_ERROR_VALIDATION_FAILED_EXT;
    VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    signal.semaphore = gate;
    signal.value = 1;
    Check(vkSignalSemaphore(device, &signal));
    Check(vkWaitForFences(device, 1, &done, VK_TRUE, 5000000000));
    const auto valid_start = errors;
    Check(vkAcquireNextImageKHR(device, swapchain, 1000000000, acquired, VK_NULL_HANDLE, &image));
    // Consume the second acquisition before destroying its semaphore.
    submit.pNext = nullptr;
    submit.waitSemaphoreCount = 1;
    Check(vkResetFences(device, 1, &done));
    Check(vkQueueSubmit(queue, 1, &submit, done));
    Check(vkWaitForFences(device, 1, &done, VK_TRUE, 5000000000));
    Check(vkDeviceWaitIdle(device));
    vkDestroyFence(device, done, nullptr);
    vkDestroySemaphore(device, gate, nullptr);
    vkDestroySemaphore(device, acquired, nullptr);
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    destroy_debug(instance, messenger, nullptr);
    vkDestroyInstance(instance, nullptr);
    DestroyWindow(window);
    const bool valid = errors == valid_start;
    std::printf("Pending-wait reuse rejected: %s\nReuse after consuming-submit fence: %s\n",
                detected ? "PASS" : "FAIL", valid ? "PASS" : "FAIL");
    return detected && valid ? 0 : 1;
} catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 2;
}
