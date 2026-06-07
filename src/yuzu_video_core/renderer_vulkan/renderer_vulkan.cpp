// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "frontend/graphics_context.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/polyfill_ranges.h"
#include "yuzu_common/scope_exit.h"
#include "yuzu_common/settings.h"
#include "yuzu_common/stb.h"
#include "yuzu_video_core/capture.h"
#include "yuzu_video_core/gpu.h"
#include "yuzu_video_core/host_shaders/watermark_frag_spv.h"
#include "yuzu_video_core/host_shaders/watermark_vert_spv.h"
#include "yuzu_video_core/present.h"
#include "yuzu_video_core/renderer_vulkan/present/util.h"
#include "yuzu_video_core/renderer_vulkan/renderer_vulkan.h"
#include "yuzu_video_core/renderer_vulkan/vk_blit_screen.h"
#include "yuzu_video_core/renderer_vulkan/vk_rasterizer.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_shader_util.h"
#include "yuzu_video_core/renderer_vulkan/vk_state_tracker.h"
#include "yuzu_video_core/renderer_vulkan/vk_swapchain.h"
#include "yuzu_video_core/textures/decoders.h"
#include "yuzu_video_core/vulkan_common/vulkan_debug_callback.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"
#include "yuzu_video_core/vulkan_common/vulkan_instance.h"
#include "yuzu_video_core/vulkan_common/vulkan_library.h"
#include "yuzu_video_core/vulkan_common/vulkan_memory_allocator.h"
#include "yuzu_video_core/vulkan_common/vulkan_surface.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"
#include "yuzu_video_core/watermark.h"

namespace Vulkan
{
namespace
{

constexpr VkExtent2D CaptureImageSize{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
};

constexpr VkExtent3D CaptureImageExtent{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
    .depth = VideoCore::Capture::LinearDepth,
};

constexpr VkFormat CaptureFormat = VK_FORMAT_A8B8G8R8_UNORM_PACK32;

std::string GetReadableVersion(u32 version)
{
    return fmt::format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                       VK_VERSION_PATCH(version));
}

std::string GetDriverVersion(const Device & device)
{
    // Extracted from
    // https://github.com/SaschaWillems/vulkan.gpuinfo.org/blob/5dddea46ea1120b0df14eef8f15ff8e318e35462/functions.php#L308-L314
    const u32 version = device.GetDriverVersion();

    if (device.GetDriverID() == VK_DRIVER_ID_NVIDIA_PROPRIETARY)
    {
        const u32 major = (version >> 22) & 0x3ff;
        const u32 minor = (version >> 14) & 0x0ff;
        const u32 secondary = (version >> 6) & 0x0ff;
        const u32 tertiary = version & 0x003f;
        return fmt::format("{}.{}.{}.{}", major, minor, secondary, tertiary);
    }
    if (device.GetDriverID() == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS)
    {
        const u32 major = version >> 14;
        const u32 minor = version & 0x3fff;
        return fmt::format("{}.{}", major, minor);
    }
    return GetReadableVersion(version);
}

std::string BuildCommaSeparatedExtensions(
    const std::set<std::string, std::less<>> & available_extensions)
{
    return fmt::format("{}", fmt::join(available_extensions, ","));
}

} // Anonymous namespace

Device CreateDevice(const vk::Instance & instance, const vk::InstanceDispatch & dld,
                    VkSurfaceKHR surface)
{
    const std::vector<VkPhysicalDevice> devices = instance.EnumeratePhysicalDevices();
    const s32 device_index = Settings::values.vulkan_device.GetValue();
    if (device_index < 0 || device_index >= static_cast<s32>(devices.size()))
    {
        LOG_ERROR(Render_Vulkan, "Invalid device index {}!", device_index);
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    const vk::PhysicalDevice physical_device(devices[device_index], dld);
    return Device(*instance, physical_device, surface, dld);
}

RendererVulkan::RendererVulkan(Core::Frontend::EmuWindow & emu_window,
                               Tegra::MaxwellDeviceMemoryManager & device_memory_, Tegra::GPU & gpu_,
                               std::unique_ptr<Core::Frontend::GraphicsContext> context_)
try :
    RendererBase(emu_window, std::move(context_)),
    device_memory(device_memory_), gpu(gpu_), library(OpenLibrary(context.get())),
    instance(CreateInstance(*library, dld, VK_API_VERSION_1_1, render_window.GetWindowInfo().type,
                            Settings::values.renderer_debug.GetValue())),
    debug_messenger(Settings::values.renderer_debug ? CreateDebugUtilsCallback(instance)
                                                    : vk::DebugUtilsMessenger{}),
    surface(CreateSurface(instance, render_window.GetWindowInfo())),
    device(CreateDevice(instance, dld, *surface)), memory_allocator(device), state_tracker(),
    scheduler(device, state_tracker),
    swapchain(*surface, device, scheduler, render_window.GetFramebufferLayout().width,
              render_window.GetFramebufferLayout().height),
    present_manager(instance, render_window, device, memory_allocator, scheduler, swapchain,
                    surface),
    blit_swapchain(device_memory, device, memory_allocator, present_manager, scheduler,
                   PresentFiltersForDisplay),
    blit_capture(device_memory, device, memory_allocator, present_manager, scheduler,
                 PresentFiltersForDisplay),
    blit_applet(device_memory, device, memory_allocator, present_manager, scheduler,
                PresentFiltersForAppletCapture),
    rasterizer(render_window, gpu, device_memory, device, memory_allocator, state_tracker,
               scheduler),
    applet_frame(),
    wm{}
{
    if (Settings::values.renderer_force_max_clock.GetValue() && device.ShouldBoostClocks())
    {
        turbo_mode.emplace(instance, dld);
        scheduler.RegisterOnSubmit([this] { turbo_mode->QueueSubmitted(); });
    }
    InitializeWatermark();
    Report();
}
catch (const vk::Exception & exception)
{
    LOG_ERROR(Render_Vulkan, "Vulkan initialization failed with error: {}", exception.what());
    throw std::runtime_error{fmt::format("Vulkan initialization error {}", exception.what())};
}

RendererVulkan::~RendererVulkan()
{
    scheduler.RegisterOnSubmit([] {});
    void(device.GetLogical().WaitIdle());
    CleanupWatermark();
}

void RendererVulkan::Composite(std::span<const Tegra::FramebufferConfig> framebuffers)
{
    if (framebuffers.empty())
    {
        return;
    }

    SCOPE_EXIT
    {
        render_window.OnFrameDisplayed();
    };

    RenderAppletCaptureLayer(framebuffers);

    if (!render_window.IsShown())
    {
        return;
    }

    RenderScreenshot(framebuffers);
    Frame * frame = present_manager.GetRenderFrame();
    blit_swapchain.DrawToFrame(rasterizer, frame, framebuffers,
                               render_window.GetFramebufferLayout(), swapchain.GetImageCount(),
                               swapchain.GetImageViewFormat());
    if (wm.show)
    {
        RenderWatermark(*frame);
    }
    scheduler.Flush(*frame->render_ready);
    present_manager.Present(frame);

    gpu.RendererFrameEndNotify();
    rasterizer.TickFrame();
}

void RendererVulkan::InitializeWatermark()
{
    if (wm.valid)
    {
        return;
    }
    wm.timer_started = false;

    int ch = 0;
    unsigned char * pixels = stbi_load_from_memory(watermark_png, watermark_png_len, &wm.width, &wm.height, &ch, 4);
    if (!pixels)
    {
        LOG_ERROR(Render_Vulkan, "watermark load failed: {}", stbi_failure_reason());
        return;
    }

    const VkExtent2D dims{(uint32_t)wm.width, (uint32_t)wm.height};
    const VkFormat swapFmt = swapchain.GetImageViewFormat();
    const VkFormat texFmt = (swapFmt == VK_FORMAT_B8G8R8A8_SRGB || swapFmt == VK_FORMAT_R8G8B8A8_SRGB) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

    wm.image = Vulkan::CreateWrappedImage(memory_allocator, dims, texFmt);
    Vulkan::UploadImage(device, memory_allocator, scheduler, wm.image, dims, texFmt, std::span<const u8>(pixels, wm.width * wm.height * 4));
    stbi_image_free(pixels);
    wm.image_view = Vulkan::CreateWrappedImageView(device, wm.image, texFmt);
    wm.sampler = Vulkan::CreateWrappedSampler(device, VK_FILTER_LINEAR);
    wm.dsl = Vulkan::CreateWrappedDescriptorSetLayout(device, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});
    wm.dpool = Vulkan::CreateWrappedDescriptorPool(device, 1, 1, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});
    std::array<VkDescriptorSetLayout, 1> layouts{*wm.dsl};
    vk::DescriptorSets sets = Vulkan::CreateWrappedDescriptorSets(wm.dpool, vk::Span<VkDescriptorSetLayout>(layouts.data(), layouts.size()));
    wm.set = sets[0];

    std::vector<VkDescriptorImageInfo> img_infos;
    img_infos.reserve(1);
    const VkWriteDescriptorSet write = Vulkan::CreateWriteDescriptorSet(img_infos, *wm.sampler, *wm.image_view, wm.set, 0);
    device.GetLogical().UpdateDescriptorSets({write}, {});

    CreateWatermarkRenderPass();
    CreateWatermarkPipeline();
    CreateWatermarkVertexBuffer();

    wm.show = true;
    wm.valid = true;
}

void RendererVulkan::CleanupWatermark()
{
    if (!wm.valid)
    {
        return;
    }

    wm.image = {};
    wm.image_view = {};
    wm.sampler = {};
    wm.dsl = {};
    wm.dpool = {};
    wm.set = {};
    wm.render_pass = {};
    wm.pipeline = {};
    wm.pipeline_layout = {};
    wm.vertex_buffer = {};

    wm.show = false;
    wm.valid = false;
}

void RendererVulkan::RenderWatermark(Frame & frame)
{
    if (!wm.timer_started)
    {
        wm.timer_started = true;
        wm.start_time = std::chrono::steady_clock::now();
    }

    std::chrono::steady_clock::time_point current_time = std::chrono::steady_clock::now();
    long long elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(current_time - wm.start_time).count();
    const float elapsed = static_cast<float>(elapsed_seconds);

    float fade_alpha;
    const float fade_duration = 300.0f;
    if (elapsed >= fade_duration)
    {
        fade_alpha = 0.0f;
        wm.show = false;
        return;
    }
    else
    {
        float progress = elapsed / fade_duration;
        float eased = 0.5f * (1.0f + cosf(progress * 3.14159f));
        fade_alpha = 0.8f * eased;
    }

    const float frame_w = static_cast<float>(frame.width);
    const float frame_h = static_cast<float>(frame.height);
    float watermark_scale = 0.14f;
    float aspect_ratio = static_cast<float>(wm.height) / static_cast<float>(wm.width);
    float watermark_screen_width = frame_w * watermark_scale;
    float watermark_screen_height = watermark_screen_width * aspect_ratio;

    float padding = 18.0f;
    float x = frame_w - watermark_screen_width - padding;
    float y = padding;

    float ndc_left = (x / frame_w) * 2.0f - 1.0f;
    float ndc_right = ((x + watermark_screen_width) / frame_w) * 2.0f - 1.0f;
    float ndc_bottom = -(((y + watermark_screen_height) / frame_h) * 2.0f - 1.0f);
    float ndc_top = -((y / frame_h) * 2.0f - 1.0f);

    float vertices[] = {
        ndc_left, ndc_bottom, 0.0f, 0.0f,  // Bottom-left -> tex (0,0)
        ndc_right, ndc_bottom, 1.0f, 0.0f, // Bottom-right -> tex (1,0)
        ndc_left, ndc_top, 0.0f, 1.0f,     // Top-left -> tex (0,1)

        ndc_right, ndc_bottom, 1.0f, 0.0f, // Bottom-right -> tex (1,0)
        ndc_right, ndc_top, 1.0f, 1.0f,    // Top-right -> tex (1,1)
        ndc_left, ndc_top, 0.0f, 1.0f      // Top-left -> tex (0,1)
    };

    std::memcpy(wm.vertex_buffer.Mapped().data(), vertices, sizeof(vertices));

    if (!frame.watermark_framebuffer)
    {
        frame.watermark_framebuffer = CreateWrappedFramebuffer(device, wm.render_pass, frame.image_view, VkExtent2D{.width = frame.width, .height = frame.height});
    }

    const VkFramebuffer framebuffer{*frame.watermark_framebuffer};
    const VkRenderPass renderpass{*wm.render_pass};
    const VkImage image{*frame.image};
    const VkExtent2D extent{.width = frame.width, .height = frame.height};
    const VkPipeline pipeline{*wm.pipeline};
    const VkPipelineLayout pipeline_layout{*wm.pipeline_layout};
    const VkDescriptorSet descriptor_set{wm.set};
    const VkBuffer vertex_buffer{*wm.vertex_buffer};
    const std::array<float, 4> push_constants{fade_alpha, 0.0f, 0.0f, 0.0f};

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([framebuffer, renderpass, image, extent, pipeline, pipeline_layout, descriptor_set, vertex_buffer, push_constants](vk::CommandBuffer cmdbuf)
    {
        TransitionImageLayout(cmdbuf, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        BeginRenderPass(cmdbuf, renderpass, framebuffer, extent);

        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, descriptor_set, {});
        cmdbuf.PushConstants(pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, static_cast<u32>(push_constants.size() * sizeof(float)), push_constants.data());
        cmdbuf.BindVertexBuffers(0, 1, &vertex_buffer, std::array<VkDeviceSize, 1>{0}.data());
        cmdbuf.Draw(6, 1, 0, 0);
        cmdbuf.EndRenderPass();

        const VkImageMemoryBarrier transition{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };

        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, {}, {}, transition);
    });
    scheduler.InvalidateState();
}

void RendererVulkan::CreateWatermarkRenderPass()
{
    VkAttachmentDescription color_attachment{
        .flags = 0,
        .format = swapchain.GetImageViewFormat(),
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, // Keep existing content
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkAttachmentReference color_ref{
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkSubpassDescription subpass{
        .flags = 0,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .inputAttachmentCount = 0,
        .pInputAttachments = nullptr,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
        .pResolveAttachments = nullptr,
        .pDepthStencilAttachment = nullptr,
        .preserveAttachmentCount = 0,
        .pPreserveAttachments = nullptr,
    };

    VkRenderPassCreateInfo render_pass_info{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 0,
        .pDependencies = nullptr,
    };

    wm.render_pass = device.GetLogical().CreateRenderPass(render_pass_info);
}

void RendererVulkan::CreateWatermarkPipeline()
{
    Vulkan::vk::ShaderModule vertex_shader = BuildShader(device, std::span<const u32>{WATERMARK_VERT_SPV, sizeof(WATERMARK_VERT_SPV) / sizeof(u32)});
    Vulkan::vk::ShaderModule fragment_shader = BuildShader(device, std::span<const u32>{WATERMARK_FRAG_SPV, sizeof(WATERMARK_FRAG_SPV) / sizeof(u32)});

    VkPushConstantRange push_constant_range{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(float) * 4, // vec4 = 16 bytes
    };

    VkPipelineLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = wm.dsl.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constant_range,
    };

    wm.pipeline_layout = device.GetLogical().CreatePipelineLayout(layout_info);

    VkVertexInputBindingDescription binding_desc{
        .binding = 0,
        .stride = 4 * sizeof(float), // x, y, u, v
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };

    std::array<VkVertexInputAttributeDescription, 2> attribute_descs{{
        {
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offset = 0,
        },
        {
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offset = 2 * sizeof(float),
        },
    }};

    VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding_desc,
        .vertexAttributeDescriptionCount = static_cast<uint32_t>(attribute_descs.size()),
        .pVertexAttributeDescriptions = attribute_descs.data(),
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .viewportCount = 1,
        .pViewports = nullptr, // Dynamic
        .scissorCount = 1,
        .pScissors = nullptr, // Dynamic
    };

    VkPipelineRasterizationStateCreateInfo rasterizer{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .lineWidth = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo multisampling{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable = VK_FALSE,
    };

    // Enable alpha blending like OpenGL version
    VkPipelineColorBlendAttachmentState color_blend_attachment{
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };

    VkPipelineColorBlendStateCreateInfo color_blending{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .logicOpEnable = VK_FALSE,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment,
    };

    std::array<VkDynamicState, 2> dynamic_states = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dynamic_state{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dynamicStateCount = static_cast<uint32_t>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    std::array<VkPipelineShaderStageCreateInfo, 2> shader_stages{{
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = *vertex_shader,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = *fragment_shader,
            .pName = "main",
        },
    }};

    VkGraphicsPipelineCreateInfo pipeline_info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stageCount = static_cast<uint32_t>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = nullptr,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = *wm.pipeline_layout,
        .renderPass = *wm.render_pass,
        .subpass = 0,
    };

    wm.pipeline = device.GetLogical().CreateGraphicsPipeline(pipeline_info);
}

void RendererVulkan::CreateWatermarkVertexBuffer()
{
    // Create vertex buffer for a quad (2 triangles = 6 vertices)
    // Each vertex has 4 floats: x, y, u, v
    const size_t buffer_size = 6 * 4 * sizeof(float);

    wm.vertex_buffer = memory_allocator.CreateBuffer({
                                                         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                                         .pNext = nullptr,
                                                         .flags = 0,
                                                         .size = buffer_size,
                                                         .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                                         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                                     },
                                                     MemoryUsage::Upload);
}

void RendererVulkan::Report() const
{
    using namespace Common::Literals;
    const std::string vendor_name{device.GetVendorName()};
    const std::string model_name{device.GetModelName()};
    const std::string driver_version = GetDriverVersion(device);
    const std::string driver_name = fmt::format("{} {}", vendor_name, driver_version);

    const std::string api_version = GetReadableVersion(device.ApiVersion());

    const std::string extensions = BuildCommaSeparatedExtensions(device.GetAvailableExtensions());

    const auto available_vram = static_cast<f64>(device.GetDeviceLocalMemory()) / f64{1_GiB};

    LOG_INFO(Render_Vulkan, "Driver: {}", driver_name);
    LOG_INFO(Render_Vulkan, "Device: {}", model_name);
    LOG_INFO(Render_Vulkan, "Vulkan: {}", api_version);
    LOG_INFO(Render_Vulkan, "Available VRAM: {:.2f} GiB", available_vram);
}

vk::Buffer RendererVulkan::RenderToBuffer(std::span<const Tegra::FramebufferConfig> framebuffers, const Layout::FramebufferLayout & layout, VkFormat format, VkDeviceSize buffer_size)
{
    auto frame = [&]() {
        Frame f{};
        f.image = CreateWrappedImage(memory_allocator, VkExtent2D{layout.width, layout.height}, format);
        f.image_view = CreateWrappedImageView(device, f.image, format);
        f.framebuffer = blit_capture.CreateFramebuffer(layout, *f.image_view, format);
        return f;
    }();

    auto dst_buffer = CreateWrappedBuffer(memory_allocator, buffer_size, MemoryUsage::Download);
    blit_capture.DrawToFrame(rasterizer, &frame, framebuffers, layout, 1, format);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *frame.image, *dst_buffer, VkExtent3D{layout.width, layout.height, 1});
    });

    // Ensure the copy is fully completed before saving the capture
    scheduler.Finish();

    // Copy backing image data to the capture buffer
    dst_buffer.Invalidate();
    return dst_buffer;
}

void RendererVulkan::RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers)
{
    if (!renderer_settings.screenshot_requested)
    {
        return;
    }

    const auto & layout{renderer_settings.screenshot_framebuffer_layout};
    const auto dst_buffer = RenderToBuffer(framebuffers, layout, VK_FORMAT_B8G8R8A8_UNORM, layout.width * layout.height * 4);

    std::memcpy(renderer_settings.screenshot_bits, dst_buffer.Mapped().data(), dst_buffer.Mapped().size());
    renderer_settings.screenshot_complete_callback(false);
    renderer_settings.screenshot_requested = false;
}

std::vector<u8> RendererVulkan::GetAppletCaptureBuffer()
{
    using namespace VideoCore::Capture;

    std::vector<u8> out(VideoCore::Capture::TiledSize);

    if (!applet_frame.image)
    {
        return out;
    }

    const auto dst_buffer = CreateWrappedBuffer(memory_allocator, VideoCore::Capture::TiledSize, MemoryUsage::Download);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *applet_frame.image, *dst_buffer, CaptureImageExtent);
    });

    // Ensure the copy is fully completed before writing the capture
    scheduler.Finish();

    // Swizzle image data to the capture buffer
    dst_buffer.Invalidate();
    Tegra::Texture::SwizzleTexture(out, dst_buffer.Mapped(), BytesPerPixel, LinearWidth, LinearHeight, LinearDepth, BlockHeight, BlockDepth);

    return out;
}

void RendererVulkan::RenderAppletCaptureLayer(std::span<const Tegra::FramebufferConfig> framebuffers)
{
    if (!applet_frame.image)
    {
        applet_frame.image = CreateWrappedImage(memory_allocator, CaptureImageSize, CaptureFormat);
        applet_frame.image_view = CreateWrappedImageView(device, applet_frame.image, CaptureFormat);
        applet_frame.framebuffer = blit_applet.CreateFramebuffer(VideoCore::Capture::Layout, *applet_frame.image_view, CaptureFormat);
    }

    blit_applet.DrawToFrame(rasterizer, &applet_frame, framebuffers, VideoCore::Capture::Layout, 1, CaptureFormat);
}

} // namespace Vulkan
