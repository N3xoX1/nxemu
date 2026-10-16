#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

// Vulkan entry points the interface uses, resolved at run time so the frontend
// never links against a Vulkan library.
#define NXEMU_VK_INSTANCE_FUNCTIONS(X) \
    X(EnumeratePhysicalDevices)        \
    X(GetPhysicalDeviceProperties)     \
    X(GetPhysicalDeviceFormatProperties) \
    X(GetPhysicalDeviceImageFormatProperties) \
    X(GetDeviceProcAddr)

#define NXEMU_VK_DEVICE_FUNCTIONS(X) \
    X(CreateImageView)               \
    X(DestroyImageView)              \
    X(CreateFramebuffer)             \
    X(DestroyFramebuffer)            \
    X(CreateRenderPass)              \
    X(DestroyRenderPass)             \
    X(CreateShaderModule)            \
    X(DestroyShaderModule)           \
    X(CreatePipelineLayout)          \
    X(DestroyPipelineLayout)         \
    X(CreateGraphicsPipelines)       \
    X(DestroyPipeline)               \
    X(CreateDescriptorSetLayout)     \
    X(DestroyDescriptorSetLayout)    \
    X(CreateDescriptorPool)          \
    X(DestroyDescriptorPool)         \
    X(AllocateDescriptorSets)        \
    X(FreeDescriptorSets)            \
    X(UpdateDescriptorSets)          \
    X(CreateSampler)                 \
    X(DestroySampler)                \
    X(CmdBeginRenderPass)            \
    X(CmdEndRenderPass)              \
    X(CmdBindPipeline)               \
    X(CmdBindDescriptorSets)         \
    X(CmdBindVertexBuffers)          \
    X(CmdBindIndexBuffer)            \
    X(CmdDrawIndexed)                \
    X(CmdDraw)                       \
    X(CmdPushConstants)              \
    X(CmdSetViewport)                \
    X(CmdSetScissor)                 \
    X(CmdSetStencilReference)        \
    X(CmdSetBlendConstants)          \
    X(CmdClearAttachments)           \
    X(CmdPipelineBarrier)            \
    X(CmdCopyBufferToImage)          \
    X(CmdCopyImage)                  \
    X(CmdBlitImage)                  \
    X(CmdResolveImage)

struct VulkanApi
{
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;

#define NXEMU_VK_DECLARE(name) PFN_vk##name name = nullptr;
    NXEMU_VK_INSTANCE_FUNCTIONS(NXEMU_VK_DECLARE)
    NXEMU_VK_DEVICE_FUNCTIONS(NXEMU_VK_DECLARE)
#undef NXEMU_VK_DECLARE

    // Resolves the entry points for a device. Returns false when one is missing.
    bool Load(PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkInstance instance, VkDevice device);
};

// The device the interface is drawn with. It belongs to the video module, which
// lends it to the renderer.
struct VulkanContext
{
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
};

// Moves an image between the layouts the interface uses, keeping its contents
// unless `from` is VK_IMAGE_LAYOUT_UNDEFINED.
void VulkanTransitionImage(const VulkanApi & api, VkCommandBuffer commandBuffer, VkImage image, VkImageAspectFlags aspect,
                           VkImageLayout from, VkImageLayout to);
