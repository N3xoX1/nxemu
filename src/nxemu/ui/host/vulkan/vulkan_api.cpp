#include "vulkan_api.h"

bool VulkanApi::Load(PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkInstance instance, VkDevice device)
{
    GetInstanceProcAddr = getInstanceProcAddr;
    if (GetInstanceProcAddr == nullptr)
    {
        return false;
    }
    bool complete = true;
#define NXEMU_VK_LOAD(name)                                                             \
    name = reinterpret_cast<PFN_vk##name>(GetInstanceProcAddr(instance, "vk" #name)); \
    complete = complete && name != nullptr;
    NXEMU_VK_INSTANCE_FUNCTIONS(NXEMU_VK_LOAD)
#undef NXEMU_VK_LOAD
    if (!complete)
    {
        return false;
    }
#define NXEMU_VK_LOAD(name)                                                         \
    name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device, "vk" #name)); \
    complete = complete && name != nullptr;
    NXEMU_VK_DEVICE_FUNCTIONS(NXEMU_VK_LOAD)
#undef NXEMU_VK_LOAD
    return complete;
}

namespace
{
struct LayoutUse
{
    VkAccessFlags access;
    VkPipelineStageFlags stage;
};

LayoutUse UseOf(VkImageLayout layout)
{
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return {VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return {VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT};
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return {VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return {VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        return {VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        return {0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT};
    default:
        return {0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT};
    }
}
} // namespace

void VulkanTransitionImage(const VulkanApi & api, VkCommandBuffer commandBuffer, VkImage image, VkImageAspectFlags aspect,
                           VkImageLayout from, VkImageLayout to)
{
    const LayoutUse source = UseOf(from);
    const LayoutUse destination = UseOf(to);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = source.access;
    barrier.dstAccessMask = destination.access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};
    api.CmdPipelineBarrier(commandBuffer, source.stage, destination.stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}
