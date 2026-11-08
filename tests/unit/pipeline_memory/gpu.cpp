// SPDX-License-Identifier: GPL-2.0-or-later
// Native Vulkan test; embedded SPIR-V: #version 450, layout(local_size_x=1) in; void main() {}
#include "nxemu-video/video_settings.h"
#include "yuzu_common/dynamic_library.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "yuzu_video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "yuzu_video_core/renderer_vulkan/vk_state_tracker.h"
#include "yuzu_video_core/renderer_vulkan/vk_update_descriptor.h"
#include "yuzu_video_core/vulkan_common/vulkan_instance.h"
#include "yuzu_video_core/vulkan_common/vulkan_memory_allocator.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <new>

IModuleSettings* g_settings{};
IModuleNotification* g_notify{};

void Require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
std::atomic<unsigned> validation_errors{};
VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++validation_errors;
        std::fprintf(stderr, "VALIDATION: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

PFN_vkDestroyShaderModule original_destroy;
PFN_vkCreateComputePipelines original_create;
VkPipeline created_pipeline{};
std::atomic<unsigned> destroyed{};
int failure{};
VKAPI_ATTR void VKAPI_CALL DestroyModule(VkDevice device, VkShaderModule module,
                                       const VkAllocationCallbacks* allocator) {
    ++destroyed;
    original_destroy(device, module, allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL CreatePipeline(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkComputePipelineCreateInfo* ci, const VkAllocationCallbacks* allocator, VkPipeline* out) {
    if (failure == 1) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (failure == 2) throw std::bad_alloc{};
    const auto result = original_create(device, cache, count, ci, allocator, out);
    if (result == VK_SUCCESS) created_pipeline = *out;
    return result;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s /path/to/Vulkan/library\n", argv[0]);
        return 2;
    }
    std::fill(std::begin(Common::Log::g_classLevel), std::end(Common::Log::g_classLevel),
              LogLevel::Warning);
    Common::DynamicLibrary library{argv[1]};
    Require(library.IsOpen(), "Vulkan library opens");
    Vulkan::vk::InstanceDispatch dispatch;
#ifdef __APPLE__
    constexpr auto platform = Core::Frontend::WindowSystemType::Cocoa;
#elif defined(_WIN32)
    constexpr auto platform = Core::Frontend::WindowSystemType::Windows;
#else
    constexpr auto platform = Core::Frontend::WindowSystemType::Wayland;
#endif
    auto instance = Vulkan::CreateInstance(library, dispatch, VK_API_VERSION_1_1,
        platform, true);
    VkDebugUtilsMessengerEXT messenger{};
    const VkDebugUtilsMessengerCreateInfoEXT messenger_ci{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = Validation};
    Require(dispatch.vkCreateDebugUtilsMessengerEXT(*instance, &messenger_ci, nullptr, &messenger) == VK_SUCCESS,
            "validation messenger created");
    const auto devices = instance.EnumeratePhysicalDevices();
    Require(!devices.empty(), "physical device available");
    {
        Vulkan::Device device{*instance, Vulkan::vk::PhysicalDevice{devices[0], dispatch}, nullptr,
                              dispatch};
        Vulkan::StateTracker state;
        Vulkan::Scheduler scheduler{device, state};
        Vulkan::MemoryAllocator allocator{device};
        Vulkan::StagingBufferPool staging{device, allocator, scheduler};
        Vulkan::DescriptorPool descriptors{device, scheduler};
        Vulkan::GuestDescriptorQueue descriptor_queue{device, scheduler};
        auto pipeline_cache = device.GetLogical().CreatePipelineCache({
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO});
        auto& dld = const_cast<Vulkan::vk::DeviceDispatch&>(device.GetDispatchLoader());
        original_destroy = dld.vkDestroyShaderModule;
        original_create = dld.vkCreateComputePipelines;
        dld.vkDestroyShaderModule = DestroyModule;
        dld.vkCreateComputePipelines = CreatePipeline;
        constexpr u32 code[]{
            0x7230203, 0x10000, 0x8000b, 0xa, 0x0, 0x20011, 0x1, 0x6000b,
            0x1, 0x4c534c47, 0x6474732e, 0x3035342e, 0x0, 0x3000e, 0x0, 0x1,
            0x5000f, 0x5, 0x4, 0x6e69616d, 0x0, 0x60010, 0x4, 0x11,
            0x1, 0x1, 0x1, 0x30003, 0x2, 0x1c2, 0x40005, 0x4,
            0x6e69616d, 0x0, 0x40047, 0x9, 0xb, 0x19, 0x20013, 0x2,
            0x30021, 0x3, 0x2, 0x40015, 0x6, 0x20, 0x0, 0x40017,
            0x7, 0x6, 0x3, 0x4002b, 0x6, 0x8, 0x1, 0x6002c,
            0x7, 0x9, 0x8, 0x8, 0x8, 0x50036, 0x2, 0x4,
            0x0, 0x3, 0x200f8, 0x5, 0x100fd, 0x10038,
        };
        const auto make_module = [&] {
            return device.GetLogical().CreateShaderModule({
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = sizeof(code), .pCode = code});
        };
        const auto dispatch_pipeline = [&] {
            scheduler.Record([handle = created_pipeline](Vulkan::vk::CommandBuffer cmdbuf) {
                cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, handle);
                cmdbuf.Dispatch(1, 1, 1);
            });
            scheduler.Finish();
        };
        for (int mode = 0; mode != 3; ++mode) {
            failure = mode;
            const auto before = destroyed.load();
            Vulkan::ComputePipeline pipeline{device, pipeline_cache, descriptors, descriptor_queue,
                nullptr, nullptr, nullptr, Shader::Info{}, make_module(), staging};
            Require(pipeline.HasBuildFailed() == (mode != 0), "synchronous result");
            Require(destroyed == before + 1, "module released before pipeline destruction");
            if (mode == 0) dispatch_pipeline();
        }
        for (int mode = 0; mode != 3; ++mode) {
            failure = mode;
            Vulkan::NativePipelineBuildQueue queue{1};
            std::promise<void> entered, release;
            auto gate = release.get_future();
            queue.QueueWork([&] { entered.set_value(); gate.wait(); });
            entered.get_future().wait();
            const auto before = destroyed.load();
            Vulkan::ComputePipeline pipeline{device, pipeline_cache, descriptors, descriptor_queue,
                &queue, nullptr, nullptr, Shader::Info{}, make_module(), staging};
            Require(destroyed == before, "queued build retains module until worker starts");
            release.set_value();
            queue.WaitForRequests();
            Require(pipeline.HasBuildFailed() == (mode != 0), "asynchronous result");
            Require(destroyed == before + 1, "worker releases module when worker finishes");
            if (mode == 0) dispatch_pipeline();
        }
        failure = 0;
        scheduler.Finish();
        dld.vkDestroyShaderModule = original_destroy;
        dld.vkCreateComputePipelines = original_create;
    }
    dispatch.vkDestroyDebugUtilsMessengerEXT(*instance, messenger, nullptr);
    Require(validation_errors == 0, "no Vulkan validation errors");
    std::puts("Compute module lifetime: sync/async success, Vulkan failure and allocation failure passed");
}
