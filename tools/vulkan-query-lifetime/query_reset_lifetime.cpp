// SPDX-License-Identifier: GPL-2.0-or-later
// Host-resetting a pool does not order a reset between recorded query uses.
// The validation callback rejects the invalid submission before driver dispatch.
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

static unsigned errors{}, pending_resets{};
static VKAPI_ATTR VkBool32 VKAPI_CALL Message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void*) {
    if (!(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT))
        return VK_FALSE;
    ++errors;
    if (data->pMessageIdName && std::strstr(data->pMessageIdName, "00807"))
        ++pending_resets;
    std::printf("validation: %s\n", data->pMessageIdName ? data->pMessageIdName : data->pMessage);
    return VK_TRUE;
}
static void Check(VkResult result) {
    if (result != VK_SUCCESS)
        throw std::runtime_error("Vulkan failure " + std::to_string(result));
}

int main() try {
    const char* layer = "VK_LAYER_KHRONOS_validation";
    const char* extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
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
    ici.enabledExtensionCount = 1;
    ici.ppEnabledExtensionNames = &extension;
    VkInstance instance;
    Check(vkCreateInstance(&ici, nullptr, &instance));
    auto create_debug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    auto destroy_debug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    VkDebugUtilsMessengerEXT messenger;
    Check(create_debug(instance, &debug, nullptr, &messenger));
    uint32_t count{};
    Check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    VkPhysicalDevice physical{};
    uint32_t family{};
    for (auto candidate : devices) {
        VkPhysicalDeviceHostQueryResetFeatures reset{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES};
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        reset.pNext = &timeline;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &reset;
        vkGetPhysicalDeviceFeatures2(candidate, &features);
        if (!reset.hostQueryReset || !timeline.timelineSemaphore)
            continue;
        uint32_t n;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, families.data());
        for (uint32_t i = 0; i < n; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                families[i].timestampValidBits) {
                physical = candidate;
                family = i;
                break;
            }
        }
        if (physical)
            break;
    }
    if (!physical)
        throw std::runtime_error("No suitable Vulkan 1.2 device");
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
    VkPhysicalDeviceHostQueryResetFeatures reset{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES};
    reset.hostQueryReset = VK_TRUE;
    reset.pNext = &timeline;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &reset;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice device;
    Check(vkCreateDevice(physical, &dci, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkQueryPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    pool_ci.queryType = VK_QUERY_TYPE_OCCLUSION;
    pool_ci.queryCount = 1;
    VkQueryPool pool;
    Check(vkCreateQueryPool(device, &pool_ci, nullptr, &pool));
    vkResetQueryPool(device, pool, 0, 1);
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    VkRenderPassCreateInfo rp_ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp_ci.subpassCount = 1;
    rp_ci.pSubpasses = &subpass;
    VkRenderPass renderpass;
    Check(vkCreateRenderPass(device, &rp_ci, nullptr, &renderpass));
    VkFramebufferCreateInfo fb_ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb_ci.renderPass = renderpass;
    fb_ci.width = 1;
    fb_ci.height = 1;
    fb_ci.layers = 1;
    VkFramebuffer framebuffer;
    Check(vkCreateFramebuffer(device, &fb_ci, nullptr, &framebuffer));
    VkCommandPoolCreateInfo cmd_ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cmd_ci.queueFamilyIndex = family;
    VkCommandPool cmd_pool;
    Check(vkCreateCommandPool(device, &cmd_ci, nullptr, &cmd_pool));
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = cmd_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    Check(vkAllocateCommandBuffers(device, &alloc, &cmd));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    Check(vkBeginCommandBuffer(cmd, &begin));
    VkRenderPassBeginInfo rp_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp_begin.renderPass = renderpass;
    rp_begin.framebuffer = framebuffer;
    rp_begin.renderArea.extent = {1, 1};
    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBeginQuery(cmd, pool, 0, 0);
    vkCmdEndQuery(cmd, pool, 0);
    vkCmdEndRenderPass(cmd);
    // CPU recycling can also happen before submission. A host reset cannot
    // order a reset between the two uses already recorded in this command buffer.
    vkResetQueryPool(device, pool, 0, 1);
    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    const unsigned before = errors;
    vkCmdBeginQuery(cmd, pool, 0, 0);
    if (errors == before)
        vkCmdEndQuery(cmd, pool, 0);
    vkCmdEndRenderPass(cmd);
    Check(vkEndCommandBuffer(cmd));
    VkSubmitInfo invalid_submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    invalid_submit.commandBufferCount = 1;
    invalid_submit.pCommandBuffers = &cmd;
    const VkResult invalid_result = vkQueueSubmit(queue, 1, &invalid_submit, VK_NULL_HANDLE);
    const bool rejected =
        errors > before && pending_resets == 1 && invalid_result == VK_ERROR_VALIDATION_FAILED_EXT;
    if (invalid_result == VK_SUCCESS)
        Check(vkQueueWaitIdle(queue));
    const unsigned valid_before = errors;
    // Do not submit the intentionally invalid recording, even when the callback
    // rejected the second BeginQuery. Start the positive case with a clean buffer.
    Check(vkResetCommandPool(device, cmd_pool, 0));
    vkResetQueryPool(device, pool, 0, 1);
    Check(vkBeginCommandBuffer(cmd, &begin));
    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBeginQuery(cmd, pool, 0, 0);
    vkCmdEndQuery(cmd, pool, 0);
    vkCmdEndRenderPass(cmd);
    Check(vkEndCommandBuffer(cmd));
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sem_ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sem_ci.pNext = &type;
    VkSemaphore gate;
    Check(vkCreateSemaphore(device, &sem_ci, nullptr, &gate));
    VkFenceCreateInfo fence_ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence done;
    Check(vkCreateFence(device, &fence_ci, nullptr, &done));
    const uint64_t wait_value = 1;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo tsi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    tsi.waitSemaphoreValueCount = 1;
    tsi.pWaitSemaphoreValues = &wait_value;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.pNext = &tsi;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &gate;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    Check(vkQueueSubmit(queue, 1, &submit, done));
    if (vkGetFenceStatus(device, done) != VK_NOT_READY)
        throw std::runtime_error("Gate was not pending");
    VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    signal.semaphore = gate;
    signal.value = 1;
    Check(vkSignalSemaphore(device, &signal));
    Check(vkWaitForFences(device, 1, &done, VK_TRUE, 5000000000));
    // A completed producer query can still be read by a later GPU copy. Host
    // result availability alone must not allow recycling its query pool.
    VkBufferCreateInfo copy_buffer_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    copy_buffer_ci.size = sizeof(uint64_t);
    copy_buffer_ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer copy_buffer;
    Check(vkCreateBuffer(device, &copy_buffer_ci, nullptr, &copy_buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, copy_buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    uint32_t memory_type = 0;
    while (!(requirements.memoryTypeBits & (1u << memory_type)))
        ++memory_type;
    VkMemoryAllocateInfo memory_ci{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memory_ci.allocationSize = requirements.size;
    memory_ci.memoryTypeIndex = memory_type;
    VkDeviceMemory copy_memory;
    Check(vkAllocateMemory(device, &memory_ci, nullptr, &copy_memory));
    Check(vkBindBufferMemory(device, copy_buffer, copy_memory, 0));
    uint64_t host_result{};
    Check(vkGetQueryPoolResults(device, pool, 0, 1, sizeof(host_result), &host_result,
                                sizeof(host_result),
                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    Check(vkResetCommandPool(device, cmd_pool, 0));
    Check(vkBeginCommandBuffer(cmd, &begin));
    vkCmdCopyQueryPoolResults(cmd, pool, 0, 1, copy_buffer, 0, sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    Check(vkEndCommandBuffer(cmd));
    Check(vkResetFences(device, 1, &done));
    const uint64_t copy_wait_value = 2;
    tsi.pWaitSemaphoreValues = &copy_wait_value;
    Check(vkQueueSubmit(queue, 1, &submit, done));
    if (vkGetFenceStatus(device, done) != VK_NOT_READY)
        throw std::runtime_error("Copy gate was not pending");
    // Host results are already available, but the copy is still pending. This
    // is exactly the interval in which recycling must be deferred. Do not try
    // an invalid reset here: validation does not reliably intercept this case.
    const bool copy_reset_deferred = vkGetFenceStatus(device, done) == VK_NOT_READY;
    signal.value = 2;
    Check(vkSignalSemaphore(device, &signal));
    Check(vkWaitForFences(device, 1, &done, VK_TRUE, 5000000000));
    vkResetQueryPool(device, pool, 0, 1);
    // Re-record and execute the same query only after the earlier submission
    // completes; this is the lifetime rule enforced by the bank reuse guard.
    Check(vkResetCommandPool(device, cmd_pool, 0));
    Check(vkBeginCommandBuffer(cmd, &begin));
    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBeginQuery(cmd, pool, 0, 0);
    vkCmdEndQuery(cmd, pool, 0);
    vkCmdEndRenderPass(cmd);
    Check(vkEndCommandBuffer(cmd));
    Check(vkResetFences(device, 1, &done));
    submit.pNext = nullptr;
    submit.waitSemaphoreCount = 0;
    Check(vkQueueSubmit(queue, 1, &submit, done));
    Check(vkWaitForFences(device, 1, &done, VK_TRUE, 5000000000));
    vkDestroyBuffer(device, copy_buffer, nullptr);
    vkFreeMemory(device, copy_memory, nullptr);
    vkDestroyFence(device, done, nullptr);
    vkDestroySemaphore(device, gate, nullptr);
    vkDestroyCommandPool(device, cmd_pool, nullptr);
    vkDestroyFramebuffer(device, framebuffer, nullptr);
    vkDestroyRenderPass(device, renderpass, nullptr);
    vkDestroyQueryPool(device, pool, nullptr);
    vkDestroyDevice(device, nullptr);
    destroy_debug(instance, messenger, nullptr);
    vkDestroyInstance(instance, nullptr);
    const bool valid = errors == valid_before;
    std::printf(
        "Unsubmitted query reuse after host reset rejected: %s\nReset after GPU completion: %s\n",
        rejected ? "PASS" : "FAIL", valid ? "PASS" : "FAIL");
    std::printf("Completed producer with pending GPU copy requires deferred reset: %s\n",
                copy_reset_deferred ? "PASS" : "FAIL");
    return rejected && valid && copy_reset_deferred ? 0 : 1;
} catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 2;
}
