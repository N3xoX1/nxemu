// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Captures what the driver receives in vkCreateGraphicsPipelines so src/pipeline_probe can
// replay it. vulkan_wrapper.cpp records the objects a pipeline refers to; the pipeline itself
// is written only on threads where PipelineCrashTrace has set an output directory.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "yuzu_video_core/renderer_vulkan/pipeline_replay.h"

namespace PipelineReplay::Capture {

inline thread_local std::filesystem::path output_directory;

struct Registry {
    std::mutex mutex;
    std::unordered_map<uint64_t, SetLayout> set_layouts;
    std::unordered_map<uint64_t, std::pair<VkPipelineLayoutCreateFlags, std::vector<uint64_t>>>
        pipeline_layouts;
    std::unordered_map<uint64_t, std::vector<VkPushConstantRange>> push_constants;
    std::unordered_map<uint64_t, Pipeline> render_passes; // Only the render pass fields are used
    std::unordered_map<uint64_t, std::vector<uint32_t>> shader_modules;
};

inline Registry& GetRegistry() {
    static Registry registry;
    return registry;
}

template <typename Handle>
uint64_t Key(Handle handle) {
    if constexpr (std::is_pointer_v<Handle>) {
        return reinterpret_cast<uint64_t>(handle);
    } else {
        return static_cast<uint64_t>(handle);
    }
}

inline void WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.flush();
}

inline void WriteWords(const std::filesystem::path& path, const std::vector<uint32_t>& words) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(words.data()),
               static_cast<std::streamsize>(words.size() * sizeof(uint32_t)));
    file.flush();
}

inline void OnShaderModule(VkShaderModule module, const VkShaderModuleCreateInfo& ci) {
    std::vector<uint32_t> code(ci.pCode, ci.pCode + ci.codeSize / sizeof(uint32_t));
    auto& registry{GetRegistry()};
    std::scoped_lock lock{registry.mutex};
    registry.shader_modules[Key(module)] = std::move(code);
}

inline void OnDescriptorSetLayout(VkDescriptorSetLayout layout,
                                  const VkDescriptorSetLayoutCreateInfo& ci) {
    SetLayout set;
    set.flags = ci.flags;
    for (uint32_t i = 0; i < ci.bindingCount; ++i) {
        VkDescriptorSetLayoutBinding binding{ci.pBindings[i]};
        if (binding.pImmutableSamplers != nullptr) {
            ++set.immutable_samplers;
        }
        binding.pImmutableSamplers = nullptr;
        set.bindings.push_back(binding);
    }
    auto& registry{GetRegistry()};
    std::scoped_lock lock{registry.mutex};
    registry.set_layouts[Key(layout)] = std::move(set);
}

inline void OnPipelineLayout(VkPipelineLayout layout, const VkPipelineLayoutCreateInfo& ci) {
    std::vector<uint64_t> sets;
    for (uint32_t i = 0; i < ci.setLayoutCount; ++i) {
        sets.push_back(Key(ci.pSetLayouts[i]));
    }
    std::vector<VkPushConstantRange> ranges(ci.pPushConstantRanges,
                                            ci.pPushConstantRanges + ci.pushConstantRangeCount);
    auto& registry{GetRegistry()};
    std::scoped_lock lock{registry.mutex};
    registry.pipeline_layouts[Key(layout)] = {ci.flags, std::move(sets)};
    registry.push_constants[Key(layout)] = std::move(ranges);
}

inline void OnRenderPass(VkRenderPass render_pass, const VkRenderPassCreateInfo& ci) {
    Pipeline rp;
    rp.has_render_pass = 1;
    rp.render_pass_flags = ci.flags;
    rp.attachments.assign(ci.pAttachments, ci.pAttachments + ci.attachmentCount);
    for (uint32_t i = 0; i < ci.subpassCount; ++i) {
        const VkSubpassDescription& desc{ci.pSubpasses[i]};
        Subpass subpass;
        subpass.flags = desc.flags;
        subpass.bind_point = desc.pipelineBindPoint;
        subpass.inputs.assign(desc.pInputAttachments,
                              desc.pInputAttachments + desc.inputAttachmentCount);
        subpass.colors.assign(desc.pColorAttachments,
                              desc.pColorAttachments + desc.colorAttachmentCount);
        if (desc.pResolveAttachments != nullptr) {
            subpass.resolves.assign(desc.pResolveAttachments,
                                    desc.pResolveAttachments + desc.colorAttachmentCount);
        }
        if (desc.pDepthStencilAttachment != nullptr) {
            subpass.has_depth = 1;
            subpass.depth = *desc.pDepthStencilAttachment;
        }
        subpass.preserves.assign(desc.pPreserveAttachments,
                                 desc.pPreserveAttachments + desc.preserveAttachmentCount);
        rp.subpasses.push_back(std::move(subpass));
    }
    rp.dependencies.assign(ci.pDependencies, ci.pDependencies + ci.dependencyCount);
    if (ci.pNext != nullptr) {
        rp.unknown_structures.push_back(
            static_cast<uint32_t>(static_cast<const VkBaseInStructure*>(ci.pNext)->sType));
    }
    auto& registry{GetRegistry()};
    std::scoped_lock lock{registry.mutex};
    registry.render_passes[Key(render_pass)] = std::move(rp);
}

// Modules are used by exactly one pipeline. Drop their code once the pipeline is created so
// the registry does not keep every SPIR-V module alive.
inline void ForgetModules(const VkPipelineShaderStageCreateInfo* stages, uint32_t count) {
    auto& registry{GetRegistry()};
    std::scoped_lock lock{registry.mutex};
    for (uint32_t i = 0; i < count; ++i) {
        registry.shader_modules.erase(Key(stages[i].module));
    }
}

inline Pipeline Convert(const VkGraphicsPipelineCreateInfo& ci) {
    Pipeline p;
    p.flags = ci.flags;
    const auto unknown = [&p](const void* next) {
        for (auto* s = static_cast<const VkBaseInStructure*>(next); s != nullptr; s = s->pNext) {
            p.unknown_structures.push_back(static_cast<uint32_t>(s->sType));
        }
    };
    unknown(ci.pNext);

    auto& registry{GetRegistry()};
    std::scoped_lock lock{registry.mutex};

    for (uint32_t i = 0; i < ci.stageCount; ++i) {
        const VkPipelineShaderStageCreateInfo& stage{ci.pStages[i]};
        unknown(stage.pNext);
        Stage s;
        s.stage = stage.stage;
        s.flags = stage.flags;
        s.entry = stage.pName != nullptr ? stage.pName : "";
        s.file = "vk_stage_" + std::to_string(static_cast<uint32_t>(stage.stage)) + ".spv";
        s.has_specialization = stage.pSpecializationInfo != nullptr ? 1 : 0;
        p.stages.push_back(std::move(s));
    }

    if (const auto* vi = ci.pVertexInputState) {
        p.has_vertex_input = 1;
        p.vertex_bindings.assign(vi->pVertexBindingDescriptions,
                                 vi->pVertexBindingDescriptions + vi->vertexBindingDescriptionCount);
        p.vertex_attributes.assign(vi->pVertexAttributeDescriptions,
                                   vi->pVertexAttributeDescriptions +
                                       vi->vertexAttributeDescriptionCount);
        for (auto* s = static_cast<const VkBaseInStructure*>(vi->pNext); s; s = s->pNext) {
            if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT) {
                const auto* div =
                    reinterpret_cast<const VkPipelineVertexInputDivisorStateCreateInfoEXT*>(s);
                p.has_divisors = 1;
                p.divisors.assign(div->pVertexBindingDivisors,
                                  div->pVertexBindingDivisors + div->vertexBindingDivisorCount);
            } else {
                p.unknown_structures.push_back(static_cast<uint32_t>(s->sType));
            }
        }
    }
    if (const auto* ia = ci.pInputAssemblyState) {
        unknown(ia->pNext);
        p.has_input_assembly = 1;
        p.topology = ia->topology;
        p.primitive_restart = ia->primitiveRestartEnable;
    }
    if (const auto* ts = ci.pTessellationState) {
        unknown(ts->pNext);
        p.has_tessellation = 1;
        p.patch_control_points = ts->patchControlPoints;
    }
    if (const auto* vp = ci.pViewportState) {
        p.has_viewport = 1;
        p.viewport_count = vp->viewportCount;
        p.scissor_count = vp->scissorCount;
        if (vp->pViewports != nullptr || vp->pScissors != nullptr) {
            p.unknown_structures.push_back(0xFFFFFFFFu); // Static viewports are not captured
        }
        for (auto* s = static_cast<const VkBaseInStructure*>(vp->pNext); s; s = s->pNext) {
            if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_SWIZZLE_STATE_CREATE_INFO_NV) {
                const auto* sw = reinterpret_cast<const VkPipelineViewportSwizzleStateCreateInfoNV*>(s);
                p.has_swizzle = 1;
                p.swizzles.assign(sw->pViewportSwizzles, sw->pViewportSwizzles + sw->viewportCount);
            } else if (s->sType ==
                       VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT) {
                const auto* dc =
                    reinterpret_cast<const VkPipelineViewportDepthClipControlCreateInfoEXT*>(s);
                p.has_depth_clip_control = 1;
                p.negative_one_to_one = dc->negativeOneToOne;
            } else {
                p.unknown_structures.push_back(static_cast<uint32_t>(s->sType));
            }
        }
    }
    if (const auto* rs = ci.pRasterizationState) {
        p.has_rasterization = 1;
        p.depth_clamp = rs->depthClampEnable;
        p.rasterizer_discard = rs->rasterizerDiscardEnable;
        p.polygon_mode = rs->polygonMode;
        p.cull_mode = rs->cullMode;
        p.front_face = rs->frontFace;
        p.depth_bias = rs->depthBiasEnable;
        p.depth_bias_constant = rs->depthBiasConstantFactor;
        p.depth_bias_clamp = rs->depthBiasClamp;
        p.depth_bias_slope = rs->depthBiasSlopeFactor;
        p.line_width = rs->lineWidth;
        for (auto* s = static_cast<const VkBaseInStructure*>(rs->pNext); s; s = s->pNext) {
            if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT) {
                const auto* ls =
                    reinterpret_cast<const VkPipelineRasterizationLineStateCreateInfoEXT*>(s);
                p.has_line_state = 1;
                p.line_mode = ls->lineRasterizationMode;
                p.stippled_line = ls->stippledLineEnable;
                p.line_stipple_factor = ls->lineStippleFactor;
                p.line_stipple_pattern = ls->lineStipplePattern;
            } else if (s->sType ==
                       VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT) {
                const auto* cs =
                    reinterpret_cast<const VkPipelineRasterizationConservativeStateCreateInfoEXT*>(s);
                p.has_conservative = 1;
                p.conservative_mode = cs->conservativeRasterizationMode;
                p.conservative_extra = cs->extraPrimitiveOverestimationSize;
            } else if (s->sType ==
                       VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT) {
                const auto* pv =
                    reinterpret_cast<const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT*>(s);
                p.has_provoking_vertex = 1;
                p.provoking_vertex = pv->provokingVertexMode;
            } else {
                p.unknown_structures.push_back(static_cast<uint32_t>(s->sType));
            }
        }
    }
    if (const auto* ms = ci.pMultisampleState) {
        unknown(ms->pNext);
        p.has_multisample = 1;
        p.samples = ms->rasterizationSamples;
        p.sample_shading = ms->sampleShadingEnable;
        p.min_sample_shading = ms->minSampleShading;
        if (ms->pSampleMask != nullptr) {
            p.has_sample_mask = 1;
            p.sample_mask = ms->pSampleMask[0];
        }
        p.alpha_to_coverage = ms->alphaToCoverageEnable;
        p.alpha_to_one = ms->alphaToOneEnable;
    }
    if (const auto* ds = ci.pDepthStencilState) {
        unknown(ds->pNext);
        p.has_depth_stencil = 1;
        p.depth_test = ds->depthTestEnable;
        p.depth_write = ds->depthWriteEnable;
        p.depth_compare = ds->depthCompareOp;
        p.depth_bounds_test = ds->depthBoundsTestEnable;
        p.stencil_test = ds->stencilTestEnable;
        p.front = ds->front;
        p.back = ds->back;
        p.min_depth_bounds = ds->minDepthBounds;
        p.max_depth_bounds = ds->maxDepthBounds;
    }
    if (const auto* cb = ci.pColorBlendState) {
        unknown(cb->pNext);
        p.has_color_blend = 1;
        p.logic_op_enable = cb->logicOpEnable;
        p.logic_op = cb->logicOp;
        p.blend_attachments.assign(cb->pAttachments, cb->pAttachments + cb->attachmentCount);
        for (int i = 0; i < 4; ++i) {
            p.blend_constants[i] = cb->blendConstants[i];
        }
    }
    if (const auto* dy = ci.pDynamicState) {
        unknown(dy->pNext);
        p.has_dynamic = 1;
        p.dynamic_states.assign(dy->pDynamicStates, dy->pDynamicStates + dy->dynamicStateCount);
    }

    if (const auto it = registry.pipeline_layouts.find(Key(ci.layout));
        it != registry.pipeline_layouts.end()) {
        p.layout_flags = it->second.first;
        for (const uint64_t set : it->second.second) {
            const auto set_it = registry.set_layouts.find(set);
            p.set_layouts.push_back(set_it != registry.set_layouts.end() ? set_it->second
                                                                         : SetLayout{});
        }
        p.push_constants = registry.push_constants[Key(ci.layout)];
    } else {
        p.unknown_structures.push_back(0xFFFFFFFEu); // Pipeline layout was not recorded
    }
    if (ci.renderPass != VK_NULL_HANDLE) {
        if (const auto it = registry.render_passes.find(Key(ci.renderPass));
            it != registry.render_passes.end()) {
            const Pipeline& rp{it->second};
            p.has_render_pass = 1;
            p.render_pass_flags = rp.render_pass_flags;
            p.attachments = rp.attachments;
            p.subpasses = rp.subpasses;
            p.dependencies = rp.dependencies;
            p.unknown_structures.insert(p.unknown_structures.end(), rp.unknown_structures.begin(),
                                        rp.unknown_structures.end());
        } else {
            p.unknown_structures.push_back(0xFFFFFFFDu); // Render pass was not recorded
        }
    }
    p.subpass = ci.subpass;
    return p;
}

// Writes pipeline.txt and one SPIR-V file per stage. Called right before the driver call so the
// files exist even if the driver crashes inside it.
inline void OnGraphicsPipeline(const VkGraphicsPipelineCreateInfo& ci) {
    const std::filesystem::path dir{output_directory};
    if (dir.empty()) {
        return;
    }
    const Pipeline pipeline{Convert(ci)};
    {
        auto& registry{GetRegistry()};
        std::scoped_lock lock{registry.mutex};
        for (uint32_t i = 0; i < ci.stageCount; ++i) {
            const auto it = registry.shader_modules.find(Key(ci.pStages[i].module));
            if (it != registry.shader_modules.end()) {
                WriteWords(dir / pipeline.stages[i].file, it->second);
            }
        }
    }
    WriteText(dir / "pipeline.txt", Serialize(pipeline));
}

} // namespace PipelineReplay::Capture
