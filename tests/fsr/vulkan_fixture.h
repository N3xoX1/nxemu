// SPDX-License-Identifier: GPL-2.0-or-later
// Shared offscreen Vulkan test fixture.
#pragma once
#include "nxemu-module-spec/fsr_sharpness.h"
#include "yuzu_video_core/fsr.h"
#include "yuzu_video_core/host_shaders/vulkan_fidelityfx_fsr_easu_fp16_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_fidelityfx_fsr_easu_fp32_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_fidelityfx_fsr_rcas_fp16_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_fidelityfx_fsr_rcas_fp32_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_fidelityfx_fsr_vert_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_easu_fp16_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_easu_fp32_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_rcas_fp16_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_rcas_fp32_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_vert_spv.h"
#include "yuzu_video_core/renderer_vulkan/present/present_push_constants.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>
#include <vulkan/vulkan.h>

namespace fs = std::filesystem;
void Check(VkResult r) {
    if (r != VK_SUCCESS)
        throw std::runtime_error("Vulkan error " + std::to_string(r));
}
unsigned validation_errors{};
VKAPI_ATTR VkBool32 VKAPI_CALL Debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                     VkDebugUtilsMessageTypeFlagsEXT,
                                     const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        ++validation_errors;
    std::cerr << data->pMessage << '\n';
    return VK_FALSE;
}
double Percentile(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(q * (v.size() - 1))];
}
std::vector<unsigned char> Pattern(int w, int h) {
    std::vector<unsigned char> p(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double u = (x + 0.5) / w, v = (y + 0.5) / h;
            double checker = ((int(u * 40) ^ int(v * 24)) & 1) ? 0.85 : 0.15;
            double diagonal = std::abs(u - v * 0.7 - 0.1) < 0.004 ? 1.0 : 0.05;
            size_t i = (size_t(y) * w + x) * 4;
            p[i] = static_cast<unsigned char>(255 * (v < 0.5 ? u : checker));
            p[i + 1] = static_cast<unsigned char>(255 * (u < 0.5 ? v : diagonal));
            p[i + 2] = static_cast<unsigned char>(
                255 * (0.5 + 0.45 * std::sin(170 * u) * std::sin(90 * v)));
            p[i + 3] = static_cast<unsigned char>(64 + 191 * u);
        }
    return p;
}
void Save(const fs::path &path, const std::vector<unsigned char> &p, int w, int h) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << ' ' << h << "\n255\n";
    for (size_t i = 0; i < p.size(); i += 4)
        f.write(reinterpret_cast<const char *>(p.data() + i), 3);
}

struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    VkFramebuffer fb{};
    VkDescriptorSet set{};
    int w{}, h{};
};
struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void *mapped{};
};
struct Context {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    VkCommandPool commands{};
    VkCommandBuffer cmd{};
    VkFence fence{};
    VkQueryPool queries{};
    VkDescriptorPool descriptors{};
    VkDescriptorSetLayout descriptor_layout{};
    VkPipelineLayout layout{};
    VkSampler sampler{};
    VkRenderPass pass16{}, pass8{};
    VkPhysicalDeviceProperties properties{};
    VkDebugUtilsMessengerEXT messenger{};
    unsigned family{}, bits{};
    bool fp16{};

    Context(VkSamplerAddressMode address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            bool storage_images = false) {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "nxemu FSR A/B";
        app.apiVersion = VK_API_VERSION_1_2;
        const char *layer = "VK_LAYER_KHRONOS_validation";
        const char *ext = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        const bool validate = std::getenv("NXEMU_FSR_VALIDATION") != nullptr;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        if (validate) {
            ci.enabledLayerCount = 1;
            ci.ppEnabledLayerNames = &layer;
            ci.enabledExtensionCount = 1;
            ci.ppEnabledExtensionNames = &ext;
        }
        Check(vkCreateInstance(&ci, nullptr, &instance));
        if (validate) {
            VkDebugUtilsMessengerCreateInfoEXT debug{
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debug.pfnUserCallback = Debug;
            Check(reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"))(
                instance, &debug, nullptr, &messenger));
        }
        uint32_t n = 0;
        Check(vkEnumeratePhysicalDevices(instance, &n, nullptr));
        std::vector<VkPhysicalDevice> devices(n);
        Check(vkEnumeratePhysicalDevices(instance, &n, devices.data()));
        for (auto d : devices) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(d, &p);
            if (!physical || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                physical = d;
        }
        if (!physical)
            throw std::runtime_error("No Vulkan GPU");
        vkGetPhysicalDeviceProperties(physical, &properties);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, families.data());
        bool found = false;
        for (unsigned i = 0; i < n; ++i)
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                families[i].timestampValidBits) {
                family = i;
                bits = families[i].timestampValidBits;
                found = true;
                break;
            }
        if (!found)
            throw std::runtime_error("No graphics queue with timestamps");
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &f12;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        fp16 = f12.shaderFloat16 && features.features.shaderInt16;
        // Enable only what the compiled shaders use.
        VkPhysicalDeviceVulkan12Features enabled{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        enabled.shaderFloat16 = fp16;
        float priority = 1;
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = family;
        q.queueCount = 1;
        q.pQueuePriorities = &priority;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dc.pNext = &enabled;
        dc.queueCreateInfoCount = 1;
        dc.pQueueCreateInfos = &q;
        VkPhysicalDeviceFeatures core{};
        core.shaderInt16 = fp16;
        core.shaderStorageImageExtendedFormats = storage_images && features.features.shaderStorageImageExtendedFormats;
        dc.pEnabledFeatures = &core;
        Check(vkCreateDevice(physical, &dc, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pc.queueFamilyIndex = family;
        pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        Check(vkCreateCommandPool(device, &pc, nullptr, &commands));
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = commands;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        Check(vkAllocateCommandBuffers(device, &ca, &cmd));
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Check(vkCreateFence(device, &fc, nullptr, &fence));
        VkQueryPoolCreateInfo qc{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qc.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qc.queryCount = 4;
        Check(vkCreateQueryPool(device, &qc, nullptr, &queries));
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = 1;
        dl.pBindings = &binding;
        Check(vkCreateDescriptorSetLayout(device, &dl, nullptr, &descriptor_layout));
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = 256;
        dp.poolSizeCount = 1;
        dp.pPoolSizes = &size;
        Check(vkCreateDescriptorPool(device, &dp, nullptr, &descriptors));
        VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                  128};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &descriptor_layout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &range;
        Check(vkCreatePipelineLayout(device, &pl, nullptr, &layout));
        VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
        sc.addressModeU = sc.addressModeV = sc.addressModeW =
            address_mode;
        sc.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        Check(vkCreateSampler(device, &sc, nullptr, &sampler));
        pass16 = Pass(VK_FORMAT_R16G16B16A16_SFLOAT);
        pass8 = Pass(VK_FORMAT_R8G8B8A8_UNORM);
    }
    unsigned MemoryType(unsigned mask, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties p;
        vkGetPhysicalDeviceMemoryProperties(physical, &p);
        for (unsigned i = 0; i < p.memoryTypeCount; ++i)
            if ((mask & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        throw std::runtime_error("No memory type");
    }
    Buffer MakeBuffer(VkDeviceSize size, VkBufferUsageFlags extra_usage = 0,
                      VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
        Buffer b;
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = size;
        ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | extra_usage;
        Check(vkCreateBuffer(device, &ci, nullptr, &b.buffer));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(device, b.buffer, &mr);
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ma.allocationSize = mr.size;
        ma.memoryTypeIndex = MemoryType(mr.memoryTypeBits, memory);
        Check(vkAllocateMemory(device, &ma, nullptr, &b.memory));
        Check(vkBindBufferMemory(device, b.buffer, b.memory, 0));
        if (memory & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            Check(vkMapMemory(device, b.memory, 0, size, 0, &b.mapped));
        return b;
    }
    VkRenderPass Pass(VkFormat format, VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_CLEAR) {
        VkAttachmentDescription a{};
        a.format = format;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = load;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ci.attachmentCount = 1;
        ci.pAttachments = &a;
        ci.subpassCount = 1;
        ci.pSubpasses = &sub;
        VkRenderPass p;
        Check(vkCreateRenderPass(device, &ci, nullptr, &p));
        return p;
    }
    Image MakeImage(int w, int h, VkFormat format, VkRenderPass pass,
                    VkImageUsageFlags extra_usage = 0) {
        Image im;
        im.w = w;
        im.h = h;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {uint32_t(w), uint32_t(h), 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | extra_usage;
        if (!pass) ci.usage &= ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        Check(vkCreateImage(device, &ci, nullptr, &im.image));
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(device, im.image, &mr);
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ma.allocationSize = mr.size;
        ma.memoryTypeIndex = MemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(device, &ma, nullptr, &im.memory));
        Check(vkBindImageMemory(device, im.image, im.memory, 0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(vkCreateImageView(device, &vi, nullptr, &im.view));
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = pass;
        fb.attachmentCount = 1;
        fb.pAttachments = &im.view;
        fb.width = w;
        fb.height = h;
        fb.layers = 1;
        if (pass) Check(vkCreateFramebuffer(device, &fb, nullptr, &im.fb));
        VkDescriptorSetAllocateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ds.descriptorPool = descriptors;
        ds.descriptorSetCount = 1;
        ds.pSetLayouts = &descriptor_layout;
        Check(vkAllocateDescriptorSets(device, &ds, &im.set));
        VkDescriptorImageInfo ii{sampler, im.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = im.set;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &ii;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        return im;
    }
    VkPipeline Pipeline(std::span<const uint32_t> vert, std::span<const uint32_t> frag,
                        VkRenderPass pass) {
        auto module = [&](std::span<const uint32_t> code) {
            VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            ci.codeSize = code.size_bytes();
            ci.pCode = code.data();
            VkShaderModule m;
            Check(vkCreateShaderModule(device, &ci, nullptr, &m));
            return m;
        };
        auto v = module(vert), f = module(frag);
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (auto &s : stages) {
            s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            s.pName = "main";
        }
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = v;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = f;
        VkPipelineVertexInputStateCreateInfo input{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        VkPipelineViewportStateCreateInfo viewport{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo blends{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blends.attachmentCount = 1;
        blends.pAttachments = &blend;
        VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = states;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &input;
        ci.pInputAssemblyState = &assembly;
        ci.pViewportState = &viewport;
        ci.pRasterizationState = &raster;
        ci.pMultisampleState = &ms;
        ci.pColorBlendState = &blends;
        ci.pDynamicState = &dyn;
        ci.layout = layout;
        ci.renderPass = pass;
        VkPipeline pipeline;
        Check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline));
        vkDestroyShaderModule(device, v, nullptr);
        vkDestroyShaderModule(device, f, nullptr);
        return pipeline;
    }
    void Begin() {
        Check(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(cmd, &b));
    }
    void Submit() {
        Check(vkEndCommandBuffer(cmd));
        Check(vkResetFences(device, 1, &fence));
        VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        s.commandBufferCount = 1;
        s.pCommandBuffers = &cmd;
        Check(vkQueueSubmit(queue, 1, &s, fence));
        Check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    }
    void Barrier(const Image &im, VkImageLayout old = VK_IMAGE_LAYOUT_GENERAL) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = old == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = old;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = im.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    void Draw(const Image &target, const Image &source, VkRenderPass pass, VkPipeline pipeline,
              const void *constants, uint32_t bytes, unsigned vertices) {
        Barrier(target);
        VkClearValue clear{};
        VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        b.renderPass = pass;
        b.framebuffer = target.fb;
        b.renderArea.extent = {uint32_t(target.w), uint32_t(target.h)};
        b.clearValueCount = 1;
        b.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &b, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport vp{0, 0, float(target.w), float(target.h), 0, 1};
        VkRect2D sc{{0, 0}, {uint32_t(target.w), uint32_t(target.h)}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &source.set, 0,
                                nullptr);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, bytes, constants);
        vkCmdDraw(cmd, vertices, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        Barrier(target);
    }
    void Free(Image &i) {
        vkDestroyFramebuffer(device, i.fb, nullptr);
        vkDestroyImageView(device, i.view, nullptr);
        vkDestroyImage(device, i.image, nullptr);
        vkFreeMemory(device, i.memory, nullptr);
    }
    void Free(Buffer &b) {
        if (b.mapped) vkUnmapMemory(device, b.memory);
        vkDestroyBuffer(device, b.buffer, nullptr);
        vkFreeMemory(device, b.memory, nullptr);
    }
    ~Context() {
        if (device) {
            vkDeviceWaitIdle(device);
            vkDestroyRenderPass(device, pass16, nullptr);
            vkDestroyRenderPass(device, pass8, nullptr);
            vkDestroySampler(device, sampler, nullptr);
            vkDestroyPipelineLayout(device, layout, nullptr);
            vkDestroyDescriptorPool(device, descriptors, nullptr);
            vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
            vkDestroyQueryPool(device, queries, nullptr);
            vkDestroyFence(device, fence, nullptr);
            vkDestroyCommandPool(device, commands, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (messenger)
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(
                instance, "vkDestroyDebugUtilsMessengerEXT"))(instance, messenger, nullptr);
        vkDestroyInstance(instance, nullptr);
    }
};
