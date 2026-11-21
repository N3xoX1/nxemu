// SPDX-License-Identifier: GPL-2.0-or-later

// pipeline_probe: replays a graphics pipeline captured by the pipeline crash trace
// (log/pipeline_trace/<hash>/pipeline.txt + vk_stage_*.spv, and log/pipeline_trace/device.txt)
// on this machine's GPU, once with the captured SPIR-V and once per variant folder. Each attempt
// runs in its own child process, so a driver crash only ends that attempt.
//
//   pipeline_probe                       probe every pipeline the trace shows was still building
//   pipeline_probe <pipeline dir>...     probe the given pipeline folders
//   --variants <dir>                     variant folders (default: probe_variants next to the exe)
//
// A variant folder holds files that replace the captured ones by name (vk_stage_1.spv,
// vk_stage_16.spv, pipeline.txt, ...). Results go to the console and probe_results.txt.

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "yuzu_video_core/renderer_vulkan/pipeline_replay.h"

namespace fs = std::filesystem;
using namespace PipelineReplay;

namespace {

enum ExitCode : DWORD {
    ExitBuilt = 0,
    ExitPipelineError = 2,
    ExitNoVulkan = 11,
    ExitNoDevice = 12,
    ExitDeviceFailed = 13,
    ExitBadCapture = 14,
    ExitObjectFailed = 15,
};

std::string ReadText(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

std::vector<uint32_t> ReadWords(const fs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const auto size = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> words(size / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
    return words;
}

fs::path ExeDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path{path}.parent_path();
}

// ---------------------------------------------------------------------------------------------
// Child: create the device and the pipeline

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr_;
PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr_;

#define LOAD_INSTANCE(instance, name) auto name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr_(instance, #name))
#define LOAD_DEVICE(device, name) auto name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr_(device, #name))

LONG WINAPI ChildCrash(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD& record{*info->ExceptionRecord};
    wchar_t module_path[MAX_PATH] = L"unknown";
    uintptr_t base{};
    HMODULE module{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(record.ExceptionAddress), &module)) {
        GetModuleFileNameW(module, module_path, MAX_PATH);
        base = reinterpret_cast<uintptr_t>(module);
    }
    const auto address{reinterpret_cast<uintptr_t>(record.ExceptionAddress)};
    std::printf("  crash 0x%08lx in %ls+0x%llx\n", static_cast<unsigned long>(record.ExceptionCode),
                fs::path{module_path}.filename().c_str(),
                static_cast<unsigned long long>(address - base));
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

struct ChainBuffer {
    std::vector<std::unique_ptr<uint8_t[]>> storage;

    VkBaseOutStructure* Make(uint32_t type, size_t body_size) {
        auto& block = storage.emplace_back(new uint8_t[sizeof(VkBaseOutStructure) + body_size]());
        auto* base = reinterpret_cast<VkBaseOutStructure*>(block.get());
        base->sType = static_cast<VkStructureType>(type);
        return base;
    }
};

DWORD RunChild(const fs::path& pipeline_dir, const fs::path& device_file, const fs::path& override_dir) {
    SetUnhandledExceptionFilter(ChildCrash);
    const auto pick = [&](const std::string& name) {
        if (!override_dir.empty() && fs::exists(override_dir / name)) {
            return override_dir / name;
        }
        return pipeline_dir / name;
    };

    Device device_capture;
    Pipeline pipeline;
    try {
        device_capture = Deserialize<Device>(ReadText(device_file));
        pipeline = Deserialize<Pipeline>(ReadText(pick("pipeline.txt")));
    } catch (const std::exception& e) {
        std::printf("  bad capture: %s\n", e.what());
        return ExitBadCapture;
    }
    if (!pipeline.unknown_structures.empty() || !device_capture.unknown_structures.empty()) {
        std::printf("  warning: capture has %zu unsupported structures, replay is not exact\n",
                    pipeline.unknown_structures.size() + device_capture.unknown_structures.size());
    }

    HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
    if (loader == nullptr) {
        std::printf("  vulkan-1.dll not found\n");
        return ExitNoVulkan;
    }
    vkGetInstanceProcAddr_ =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    LOAD_INSTANCE(nullptr, vkEnumerateInstanceVersion);
    LOAD_INSTANCE(nullptr, vkEnumerateInstanceExtensionProperties);
    LOAD_INSTANCE(nullptr, vkCreateInstance);

    uint32_t loader_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != nullptr) {
        vkEnumerateInstanceVersion(&loader_version);
    }
    const uint32_t api_version =
        std::max<uint32_t>(VK_API_VERSION_1_1, std::min(loader_version, device_capture.api_version));

    uint32_t count{};
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> instance_extensions(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, instance_extensions.data());
    std::vector<const char*> enabled_instance;
    for (const char* wanted : {VK_KHR_SURFACE_EXTENSION_NAME, "VK_KHR_win32_surface",
                               VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME}) {
        for (const auto& ext : instance_extensions) {
            if (std::strcmp(ext.extensionName, wanted) == 0) {
                enabled_instance.push_back(wanted);
            }
        }
    }
    const VkApplicationInfo app{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "nxemu pipeline_probe",
        .applicationVersion = 1,
        .pEngineName = "nxemu",
        .engineVersion = 1,
        .apiVersion = api_version,
    };
    const VkInstanceCreateInfo instance_ci{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = static_cast<uint32_t>(enabled_instance.size()),
        .ppEnabledExtensionNames = enabled_instance.data(),
    };
    VkInstance instance{};
    if (vkCreateInstance(&instance_ci, nullptr, &instance) != VK_SUCCESS) {
        std::printf("  vkCreateInstance failed\n");
        return ExitNoVulkan;
    }
    LOAD_INSTANCE(instance, vkEnumeratePhysicalDevices);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceProperties);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceFeatures2);
    LOAD_INSTANCE(instance, vkEnumerateDeviceExtensionProperties);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE(instance, vkCreateDevice);
    vkGetDeviceProcAddr_ =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(vkGetInstanceProcAddr_(instance, "vkGetDeviceProcAddr"));

    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physical_devices(count);
    vkEnumeratePhysicalDevices(instance, &count, physical_devices.data());
    VkPhysicalDevice physical{};
    VkPhysicalDeviceProperties properties{};
    for (VkPhysicalDevice candidate : physical_devices) {
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.vendorID == device_capture.vendor_id &&
            properties.deviceID == device_capture.device_id) {
            physical = candidate;
            break;
        }
    }
    if (physical == nullptr) {
        std::printf("  captured GPU %s not found\n", device_capture.device_name.c_str());
        return ExitNoDevice;
    }
    if (properties.driverVersion != device_capture.driver_version) {
        std::printf("  warning: driver version 0x%x differs from the capture (0x%x)\n",
                    properties.driverVersion, device_capture.driver_version);
    }

    // Extensions: the captured list, minus anything this driver lacks.
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> device_extensions(count);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, device_extensions.data());
    std::vector<const char*> enabled_extensions;
    for (const std::string& name : device_capture.extensions) {
        const bool supported = std::ranges::any_of(device_extensions, [&](const auto& ext) {
            return name == ext.extensionName;
        });
        if (supported) {
            enabled_extensions.push_back(name.c_str());
        } else {
            std::printf("  warning: extension %s is not supported here, skipped\n", name.c_str());
        }
    }

    // Features: the captured chain, masked with what this driver reports.
    ChainBuffer requested;
    ChainBuffer queried;
    std::vector<VkBaseOutStructure*> wants;
    std::vector<VkBaseOutStructure*> haves;
    VkBaseOutStructure* query_root{};
    for (const FeatureStruct& feature : device_capture.features) {
        auto* want = requested.Make(feature.type, feature.body.size());
        std::memcpy(reinterpret_cast<uint8_t*>(want) + sizeof(VkBaseOutStructure),
                    feature.body.data(), feature.body.size());
        auto* have = queried.Make(feature.type, feature.body.size());
        if (feature.type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            query_root = have;
        }
        wants.push_back(want);
        haves.push_back(have);
    }
    if (query_root == nullptr) {
        query_root = queried.Make(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                  sizeof(VkPhysicalDeviceFeatures2) - sizeof(VkBaseOutStructure));
    }
    VkBaseOutStructure* query_tail{query_root};
    for (VkBaseOutStructure* have : haves) {
        if (have != query_root) {
            query_tail->pNext = have;
            query_tail = have;
        }
    }
    vkGetPhysicalDeviceFeatures2(physical, reinterpret_cast<VkPhysicalDeviceFeatures2*>(query_root));
    VkBaseOutStructure* requested_head{};
    VkBaseOutStructure** requested_tail{&requested_head};
    for (size_t i = 0; i < wants.size(); ++i) {
        const FeatureStruct& feature{device_capture.features[i]};
        const size_t words = feature.body.size() / sizeof(VkBool32);
        auto* want = reinterpret_cast<VkBool32*>(reinterpret_cast<uint8_t*>(wants[i]) +
                                                 sizeof(VkBaseOutStructure));
        const auto* have = reinterpret_cast<const VkBool32*>(
            reinterpret_cast<const uint8_t*>(haves[i]) + sizeof(VkBaseOutStructure));
        bool any{};
        for (size_t j = 0; j < words; ++j) {
            if (want[j] && !have[j]) {
                std::printf("  warning: feature struct %u field %zu is not supported here, cleared\n",
                            feature.type, j);
                want[j] = VK_FALSE;
            }
            any |= want[j] != VK_FALSE;
        }
        if (any || feature.type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            wants[i]->pNext = nullptr;
            *requested_tail = wants[i];
            requested_tail = &wants[i]->pNext;
        }
    }

    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family{};
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        ++family;
    }
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_ci{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const VkDeviceCreateInfo device_ci{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = requested_head,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_ci,
        .enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size()),
        .ppEnabledExtensionNames = enabled_extensions.data(),
    };
    VkDevice device{};
    if (const VkResult result = vkCreateDevice(physical, &device_ci, nullptr, &device);
        result != VK_SUCCESS) {
        std::printf("  vkCreateDevice failed: %d\n", result);
        return ExitDeviceFailed;
    }
    LOAD_DEVICE(device, vkCreateShaderModule);
    LOAD_DEVICE(device, vkCreateDescriptorSetLayout);
    LOAD_DEVICE(device, vkCreatePipelineLayout);
    LOAD_DEVICE(device, vkCreateRenderPass);
    LOAD_DEVICE(device, vkCreateGraphicsPipelines);

    // Shader modules
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    for (const Stage& stage : pipeline.stages) {
        const fs::path file{pick(stage.file)};
        const std::vector<uint32_t> code{ReadWords(file)};
        if (code.empty()) {
            std::printf("  missing SPIR-V %s\n", stage.file.c_str());
            return ExitBadCapture;
        }
        if (stage.has_specialization) {
            std::printf("  warning: specialization constants are not replayed\n");
        }
        const VkShaderModuleCreateInfo module_ci{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = code.size() * sizeof(uint32_t),
            .pCode = code.data(),
        };
        VkShaderModule module{};
        if (vkCreateShaderModule(device, &module_ci, nullptr, &module) != VK_SUCCESS) {
            std::printf("  vkCreateShaderModule failed for %s\n", file.string().c_str());
            return ExitObjectFailed;
        }
        std::printf("  stage 0x%x: %s (%zu bytes)\n", static_cast<unsigned>(stage.stage),
                    file.string().c_str(), code.size() * 4);
        stages.push_back({
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .flags = stage.flags,
            .stage = stage.stage,
            .module = module,
            .pName = stage.entry.c_str(),
        });
    }

    // Layout
    std::vector<VkDescriptorSetLayout> set_layouts;
    for (const PipelineReplay::SetLayout& set : pipeline.set_layouts) {
        if (set.immutable_samplers != 0) {
            std::printf("  warning: immutable samplers are not replayed\n");
        }
        const VkDescriptorSetLayoutCreateInfo set_ci{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags = set.flags,
            .bindingCount = static_cast<uint32_t>(set.bindings.size()),
            .pBindings = set.bindings.data(),
        };
        VkDescriptorSetLayout layout{};
        if (vkCreateDescriptorSetLayout(device, &set_ci, nullptr, &layout) != VK_SUCCESS) {
            std::printf("  vkCreateDescriptorSetLayout failed\n");
            return ExitObjectFailed;
        }
        set_layouts.push_back(layout);
    }
    const VkPipelineLayoutCreateInfo layout_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .flags = pipeline.layout_flags,
        .setLayoutCount = static_cast<uint32_t>(set_layouts.size()),
        .pSetLayouts = set_layouts.data(),
        .pushConstantRangeCount = static_cast<uint32_t>(pipeline.push_constants.size()),
        .pPushConstantRanges = pipeline.push_constants.data(),
    };
    VkPipelineLayout pipeline_layout{};
    if (vkCreatePipelineLayout(device, &layout_ci, nullptr, &pipeline_layout) != VK_SUCCESS) {
        std::printf("  vkCreatePipelineLayout failed\n");
        return ExitObjectFailed;
    }

    // Render pass
    VkRenderPass render_pass{};
    std::vector<VkSubpassDescription> subpasses;
    for (const Subpass& s : pipeline.subpasses) {
        subpasses.push_back({
            .flags = s.flags,
            .pipelineBindPoint = s.bind_point,
            .inputAttachmentCount = static_cast<uint32_t>(s.inputs.size()),
            .pInputAttachments = s.inputs.data(),
            .colorAttachmentCount = static_cast<uint32_t>(s.colors.size()),
            .pColorAttachments = s.colors.data(),
            .pResolveAttachments = s.resolves.empty() ? nullptr : s.resolves.data(),
            .pDepthStencilAttachment = s.has_depth ? &s.depth : nullptr,
            .preserveAttachmentCount = static_cast<uint32_t>(s.preserves.size()),
            .pPreserveAttachments = s.preserves.data(),
        });
    }
    if (pipeline.has_render_pass) {
        const VkRenderPassCreateInfo rp_ci{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .flags = pipeline.render_pass_flags,
            .attachmentCount = static_cast<uint32_t>(pipeline.attachments.size()),
            .pAttachments = pipeline.attachments.data(),
            .subpassCount = static_cast<uint32_t>(subpasses.size()),
            .pSubpasses = subpasses.data(),
            .dependencyCount = static_cast<uint32_t>(pipeline.dependencies.size()),
            .pDependencies = pipeline.dependencies.data(),
        };
        if (vkCreateRenderPass(device, &rp_ci, nullptr, &render_pass) != VK_SUCCESS) {
            std::printf("  vkCreateRenderPass failed\n");
            return ExitObjectFailed;
        }
    }

    // Fixed function state
    const VkPipelineVertexInputDivisorStateCreateInfoEXT divisor_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
        .vertexBindingDivisorCount = static_cast<uint32_t>(pipeline.divisors.size()),
        .pVertexBindingDivisors = pipeline.divisors.data(),
    };
    const VkPipelineVertexInputStateCreateInfo vertex_input_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = pipeline.has_divisors ? &divisor_ci : nullptr,
        .vertexBindingDescriptionCount = static_cast<uint32_t>(pipeline.vertex_bindings.size()),
        .pVertexBindingDescriptions = pipeline.vertex_bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<uint32_t>(pipeline.vertex_attributes.size()),
        .pVertexAttributeDescriptions = pipeline.vertex_attributes.data(),
    };
    const VkPipelineInputAssemblyStateCreateInfo input_assembly_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = pipeline.topology,
        .primitiveRestartEnable = pipeline.primitive_restart,
    };
    const VkPipelineTessellationStateCreateInfo tessellation_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
        .patchControlPoints = pipeline.patch_control_points,
    };
    VkPipelineViewportSwizzleStateCreateInfoNV swizzle_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_SWIZZLE_STATE_CREATE_INFO_NV,
        .viewportCount = static_cast<uint32_t>(pipeline.swizzles.size()),
        .pViewportSwizzles = pipeline.swizzles.data(),
    };
    VkPipelineViewportDepthClipControlCreateInfoEXT depth_clip_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT,
        .negativeOneToOne = pipeline.negative_one_to_one,
    };
    VkPipelineViewportStateCreateInfo viewport_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = pipeline.viewport_count,
        .scissorCount = pipeline.scissor_count,
    };
    const void** viewport_next{&viewport_ci.pNext};
    if (pipeline.has_swizzle) {
        *viewport_next = &swizzle_ci;
        viewport_next = &swizzle_ci.pNext;
    }
    if (pipeline.has_depth_clip_control) {
        *viewport_next = &depth_clip_ci;
    }
    VkPipelineRasterizationLineStateCreateInfoEXT line_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT,
        .lineRasterizationMode = pipeline.line_mode,
        .stippledLineEnable = pipeline.stippled_line,
        .lineStippleFactor = pipeline.line_stipple_factor,
        .lineStipplePattern = static_cast<uint16_t>(pipeline.line_stipple_pattern),
    };
    VkPipelineRasterizationConservativeStateCreateInfoEXT conservative_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT,
        .conservativeRasterizationMode = pipeline.conservative_mode,
        .extraPrimitiveOverestimationSize = pipeline.conservative_extra,
    };
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
        .provokingVertexMode = pipeline.provoking_vertex,
    };
    VkPipelineRasterizationStateCreateInfo rasterization_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = pipeline.depth_clamp,
        .rasterizerDiscardEnable = pipeline.rasterizer_discard,
        .polygonMode = pipeline.polygon_mode,
        .cullMode = pipeline.cull_mode,
        .frontFace = pipeline.front_face,
        .depthBiasEnable = pipeline.depth_bias,
        .depthBiasConstantFactor = pipeline.depth_bias_constant,
        .depthBiasClamp = pipeline.depth_bias_clamp,
        .depthBiasSlopeFactor = pipeline.depth_bias_slope,
        .lineWidth = pipeline.line_width,
    };
    const void** raster_next{&rasterization_ci.pNext};
    if (pipeline.has_line_state) {
        *raster_next = &line_ci;
        raster_next = &line_ci.pNext;
    }
    if (pipeline.has_conservative) {
        *raster_next = &conservative_ci;
        raster_next = &conservative_ci.pNext;
    }
    if (pipeline.has_provoking_vertex) {
        *raster_next = &provoking_ci;
    }
    const VkPipelineMultisampleStateCreateInfo multisample_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = pipeline.samples,
        .sampleShadingEnable = pipeline.sample_shading,
        .minSampleShading = pipeline.min_sample_shading,
        .pSampleMask = pipeline.has_sample_mask ? &pipeline.sample_mask : nullptr,
        .alphaToCoverageEnable = pipeline.alpha_to_coverage,
        .alphaToOneEnable = pipeline.alpha_to_one,
    };
    const VkPipelineDepthStencilStateCreateInfo depth_stencil_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = pipeline.depth_test,
        .depthWriteEnable = pipeline.depth_write,
        .depthCompareOp = pipeline.depth_compare,
        .depthBoundsTestEnable = pipeline.depth_bounds_test,
        .stencilTestEnable = pipeline.stencil_test,
        .front = pipeline.front,
        .back = pipeline.back,
        .minDepthBounds = pipeline.min_depth_bounds,
        .maxDepthBounds = pipeline.max_depth_bounds,
    };
    VkPipelineColorBlendStateCreateInfo color_blend_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = pipeline.logic_op_enable,
        .logicOp = pipeline.logic_op,
        .attachmentCount = static_cast<uint32_t>(pipeline.blend_attachments.size()),
        .pAttachments = pipeline.blend_attachments.data(),
    };
    std::copy(std::begin(pipeline.blend_constants), std::end(pipeline.blend_constants),
              color_blend_ci.blendConstants);
    const VkPipelineDynamicStateCreateInfo dynamic_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<uint32_t>(pipeline.dynamic_states.size()),
        .pDynamicStates = pipeline.dynamic_states.data(),
    };
    const VkGraphicsPipelineCreateInfo pipeline_ci{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .flags = pipeline.flags,
        .stageCount = static_cast<uint32_t>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = pipeline.has_vertex_input ? &vertex_input_ci : nullptr,
        .pInputAssemblyState = pipeline.has_input_assembly ? &input_assembly_ci : nullptr,
        .pTessellationState = pipeline.has_tessellation ? &tessellation_ci : nullptr,
        .pViewportState = pipeline.has_viewport ? &viewport_ci : nullptr,
        .pRasterizationState = pipeline.has_rasterization ? &rasterization_ci : nullptr,
        .pMultisampleState = pipeline.has_multisample ? &multisample_ci : nullptr,
        .pDepthStencilState = pipeline.has_depth_stencil ? &depth_stencil_ci : nullptr,
        .pColorBlendState = pipeline.has_color_blend ? &color_blend_ci : nullptr,
        .pDynamicState = pipeline.has_dynamic ? &dynamic_ci : nullptr,
        .layout = pipeline_layout,
        .renderPass = render_pass,
        .subpass = pipeline.subpass,
    };
    std::printf("  calling vkCreateGraphicsPipelines on %s\n", properties.deviceName);
    std::fflush(stdout);
    const auto start = std::chrono::steady_clock::now();
    VkPipeline result_pipeline{};
    const VkResult result =
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &result_pipeline);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    std::printf("  vkCreateGraphicsPipelines returned %d after %lld ms\n", result,
                static_cast<long long>(ms));
    return result == VK_SUCCESS ? ExitBuilt : ExitPipelineError;
}

// ---------------------------------------------------------------------------------------------
// Parent: run one child per attempt and collect the results

struct Attempt {
    std::string name;
    fs::path override_dir;
};

std::string Describe(DWORD code) {
    char text[64];
    switch (code) {
    case ExitBuilt:
        return "BUILT";
    case ExitPipelineError:
        return "FAILED (driver returned an error)";
    case ExitNoVulkan:
        return "ERROR (no Vulkan)";
    case ExitNoDevice:
        return "ERROR (captured GPU not present)";
    case ExitDeviceFailed:
        return "ERROR (device creation failed)";
    case ExitBadCapture:
        return "ERROR (bad capture files)";
    case ExitObjectFailed:
        return "ERROR (object creation failed)";
    case STILL_ACTIVE:
        return "HANG (timed out)";
    default:
        std::snprintf(text, sizeof(text), "CRASH 0x%08lx", static_cast<unsigned long>(code));
        return text;
    }
}

std::wstring Quote(const fs::path& path) {
    return L"\"" + path.wstring() + L"\"";
}

DWORD RunAttempt(const fs::path& pipeline_dir, const fs::path& device_file, const Attempt& attempt,
                 std::string& output) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_pipe{}, write_pipe{};
    CreatePipe(&read_pipe, &write_pipe, &sa, 0);
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command = Quote(exe) + L" --run " + Quote(pipeline_dir) + L" " + Quote(device_file) +
                           L" " + (attempt.override_dir.empty() ? L"-" : Quote(attempt.override_dir));
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si,
                        &pi)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        output += "  could not start child process\n";
        return ExitNoVulkan;
    }
    CloseHandle(write_pipe);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    bool exited = false;
    for (;;) {
        DWORD available{};
        while (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) && available) {
            char buffer[4096];
            DWORD read{};
            if (!ReadFile(read_pipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &read,
                          nullptr) ||
                read == 0) {
                break;
            }
            std::fwrite(buffer, 1, read, stdout);
            output.append(buffer, read);
        }
        if (exited) {
            break;
        }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            exited = true; // Drain once more
        } else if (std::chrono::steady_clock::now() > deadline) {
            TerminateProcess(pi.hProcess, STILL_ACTIVE);
            WaitForSingleObject(pi.hProcess, INFINITE);
            exited = true;
        }
    }
    DWORD code{};
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(read_pipe);
    return code;
}

bool IsStillBuilding(const fs::path& dir) {
    const std::string status{ReadText(dir / "status.txt")};
    return status.rfind("building", 0) == 0 && fs::exists(dir / "pipeline.txt");
}

std::vector<fs::path> FindCrashedPipelines() {
    std::vector<fs::path> roots;
    const fs::path exe_dir{ExeDirectory()};
    for (const char* name : {"pipeline_trace", "pipeline_trace.old"}) {
        roots.push_back(exe_dir / "user" / "log" / name);
        wchar_t appdata[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) != 0) {
            roots.push_back(fs::path{appdata} / "nxemu" / "log" / name);
        }
    }
    std::vector<fs::path> found;
    for (const fs::path& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            continue;
        }
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (entry.is_directory() && IsStillBuilding(entry.path())) {
                found.push_back(entry.path());
            }
        }
    }
    return found;
}

int RunParent(std::vector<fs::path> pipelines, fs::path variants_dir, bool pause) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (pipelines.empty()) {
        pipelines = FindCrashedPipelines();
    }
    if (variants_dir.empty()) {
        variants_dir = ExeDirectory() / "probe_variants";
    }
    std::vector<Attempt> attempts{{"original", {}}};
    std::error_code ec;
    if (fs::is_directory(variants_dir, ec)) {
        std::vector<fs::path> dirs;
        for (const auto& entry : fs::directory_iterator(variants_dir, ec)) {
            if (entry.is_directory()) {
                dirs.push_back(entry.path());
            }
        }
        std::ranges::sort(dirs);
        for (const fs::path& dir : dirs) {
            attempts.push_back({dir.filename().string(), dir});
        }
    }

    std::string report;
    if (pipelines.empty()) {
        report += "No crashed pipeline found. Run the game until it crashes, or pass a pipeline "
                  "folder from log\\pipeline_trace.\n";
        std::fputs(report.c_str(), stdout);
    }
    std::string summary;
    for (const fs::path& pipeline_dir : pipelines) {
        const fs::path device_file{pipeline_dir.parent_path() / "device.txt"};
        for (const Attempt& attempt : attempts) {
            const std::string header =
                "== " + pipeline_dir.filename().string() + " / " + attempt.name + "\n";
            std::fputs(header.c_str(), stdout);
            report += header;
            const DWORD code = RunAttempt(pipeline_dir, device_file, attempt, report);
            const std::string line = "  result: " + Describe(code) + "\n";
            std::fputs(line.c_str(), stdout);
            report += line;
            summary += pipeline_dir.filename().string() + "  " + attempt.name + "  " +
                       Describe(code) + "\n";
        }
    }
    report += "\nSummary\n" + summary;
    std::printf("\nSummary\n%s", summary.c_str());

    const fs::path results{ExeDirectory() / "probe_results.txt"};
    std::ofstream(results, std::ios::binary) << report;
    std::printf("\nResults written to %s\n", results.string().c_str());
    if (pause) {
        std::printf("Press Enter to close.\n");
        std::getchar();
    }
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 5 && std::wstring_view{argv[1]} == L"--run") {
        const std::wstring override_arg{argv[4]};
        return static_cast<int>(
            RunChild(argv[2], argv[3], override_arg == L"-" ? fs::path{} : fs::path{override_arg}));
    }
    std::vector<fs::path> pipelines;
    fs::path variants;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg{argv[i]};
        if (arg == L"--variants" && i + 1 < argc) {
            variants = argv[++i];
        } else {
            pipelines.emplace_back(argv[i]);
        }
    }
    return RunParent(std::move(pipelines), std::move(variants), argc == 1);
}
