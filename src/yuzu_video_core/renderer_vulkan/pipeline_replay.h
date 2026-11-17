// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Text format for replaying a captured vkCreateGraphicsPipelines call outside the emulator.
// Shared by the capture side (pipeline_replay_capture.h) and src/pipeline_probe, so it only
// depends on the Vulkan headers and the standard library.

#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "yuzu_video_core/vulkan_common/vulkan.h"

namespace PipelineReplay {

class Writer {
public:
    template <typename T>
    void operator()(const char* name, const T& value) {
        out << name << ' ' << Encode(value) << '\n';
    }

    void operator()(const char* name, const std::string& value) {
        out << name << ' ' << (value.empty() ? "-" : value) << '\n';
    }

    void operator()(const char* name, const std::vector<uint8_t>& value) {
        static constexpr char digits[] = "0123456789abcdef";
        out << name << ' ' << value.size() << ' ';
        for (const uint8_t byte : value) {
            out << digits[byte >> 4] << digits[byte & 15];
        }
        out << '\n';
    }

    template <typename T>
    void Count(const char* name, const std::vector<T>& values) {
        out << name << ' ' << values.size() << '\n';
    }

    std::string Text() const {
        return out.str();
    }

private:
    template <typename T>
    static uint64_t Encode(const T& value) {
        if constexpr (std::is_same_v<T, float>) {
            return std::bit_cast<uint32_t>(value);
        } else if constexpr (std::is_enum_v<T>) {
            return static_cast<uint64_t>(static_cast<int64_t>(value));
        } else {
            return static_cast<uint64_t>(value);
        }
    }

    std::ostringstream out;
};

class Reader {
public:
    explicit Reader(const std::string& text) : in{text} {}

    template <typename T>
    void operator()(const char* name, T& value) {
        Expect(name);
        uint64_t raw{};
        if (!(in >> raw)) {
            Fail(name);
        }
        if constexpr (std::is_same_v<T, float>) {
            value = std::bit_cast<float>(static_cast<uint32_t>(raw));
        } else if constexpr (std::is_enum_v<T>) {
            value = static_cast<T>(static_cast<int64_t>(raw));
        } else {
            value = static_cast<T>(raw);
        }
    }

    void operator()(const char* name, std::string& value) {
        Expect(name);
        if (!(in >> value)) {
            Fail(name);
        }
        if (value == "-") {
            value.clear();
        }
    }

    void operator()(const char* name, std::vector<uint8_t>& value) {
        Expect(name);
        size_t size{};
        std::string hex;
        if (!(in >> size)) {
            Fail(name);
        }
        if (size != 0 && !(in >> hex)) {
            Fail(name);
        }
        if (hex.size() != size * 2) {
            Fail(name);
        }
        value.resize(size);
        for (size_t i = 0; i < size; ++i) {
            value[i] = static_cast<uint8_t>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
        }
    }

    template <typename T>
    void Count(const char* name, std::vector<T>& values) {
        size_t size{};
        operator()(name, size);
        values.resize(size);
    }

private:
    void Expect(const char* name) {
        std::string token;
        if (!(in >> token) || token != name) {
            throw std::runtime_error("pipeline replay: expected '" + std::string{name} +
                                     "', found '" + token + "'");
        }
    }

    [[noreturn]] static void Fail(const char* name) {
        throw std::runtime_error("pipeline replay: bad value for '" + std::string{name} + "'");
    }

    std::istringstream in;
};

// Each Visit lists a struct's fields once, so writing and reading cannot drift apart.

template <typename V>
void Visit(V& v, uint32_t& value) {
    v("u32", value);
}

template <typename V>
void Visit(V& v, VkDynamicState& value) {
    v("dynamic", value);
}

template <typename V>
void Visit(V& v, VkDescriptorSetLayoutBinding& b) {
    v("binding", b.binding);
    v("type", b.descriptorType);
    v("count", b.descriptorCount);
    v("stages", b.stageFlags);
}

template <typename V>
void Visit(V& v, VkPushConstantRange& r) {
    v("push_stages", r.stageFlags);
    v("push_offset", r.offset);
    v("push_size", r.size);
}

template <typename V>
void Visit(V& v, VkAttachmentDescription& a) {
    v("att_flags", a.flags);
    v("format", a.format);
    v("samples", a.samples);
    v("load", a.loadOp);
    v("store", a.storeOp);
    v("stencil_load", a.stencilLoadOp);
    v("stencil_store", a.stencilStoreOp);
    v("initial", a.initialLayout);
    v("final", a.finalLayout);
}

template <typename V>
void Visit(V& v, VkAttachmentReference& r) {
    v("attachment", r.attachment);
    v("layout", r.layout);
}

template <typename V>
void Visit(V& v, VkSubpassDependency& d) {
    v("src_subpass", d.srcSubpass);
    v("dst_subpass", d.dstSubpass);
    v("src_stage", d.srcStageMask);
    v("dst_stage", d.dstStageMask);
    v("src_access", d.srcAccessMask);
    v("dst_access", d.dstAccessMask);
    v("dep_flags", d.dependencyFlags);
}

template <typename V>
void Visit(V& v, VkVertexInputBindingDescription& b) {
    v("vb_binding", b.binding);
    v("vb_stride", b.stride);
    v("vb_rate", b.inputRate);
}

template <typename V>
void Visit(V& v, VkVertexInputBindingDivisorDescriptionEXT& d) {
    v("div_binding", d.binding);
    v("div_divisor", d.divisor);
}

template <typename V>
void Visit(V& v, VkVertexInputAttributeDescription& a) {
    v("va_location", a.location);
    v("va_binding", a.binding);
    v("va_format", a.format);
    v("va_offset", a.offset);
}

template <typename V>
void Visit(V& v, VkViewportSwizzleNV& s) {
    v("sw_x", s.x);
    v("sw_y", s.y);
    v("sw_z", s.z);
    v("sw_w", s.w);
}

template <typename V>
void Visit(V& v, VkStencilOpState& s) {
    v("fail", s.failOp);
    v("pass", s.passOp);
    v("depth_fail", s.depthFailOp);
    v("compare", s.compareOp);
    v("compare_mask", s.compareMask);
    v("write_mask", s.writeMask);
    v("reference", s.reference);
}

template <typename V>
void Visit(V& v, VkPipelineColorBlendAttachmentState& a) {
    v("blend", a.blendEnable);
    v("src_color", a.srcColorBlendFactor);
    v("dst_color", a.dstColorBlendFactor);
    v("color_op", a.colorBlendOp);
    v("src_alpha", a.srcAlphaBlendFactor);
    v("dst_alpha", a.dstAlphaBlendFactor);
    v("alpha_op", a.alphaBlendOp);
    v("write", a.colorWriteMask);
}

template <typename V>
void Visit(V& v, std::string& s) {
    v("name", s);
}

template <typename V, typename T>
void VisitList(V& v, const char* name, std::vector<T>& values) {
    v.Count(name, values);
    for (T& value : values) {
        Visit(v, value);
    }
}

struct SetLayout {
    VkDescriptorSetLayoutCreateFlags flags{};
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    uint32_t immutable_samplers{}; // Not replayed; nonzero means the replay is not exact.
};

template <typename V>
void Visit(V& v, SetLayout& s) {
    v("set_flags", s.flags);
    v("immutable_samplers", s.immutable_samplers);
    VisitList(v, "bindings", s.bindings);
}

struct Subpass {
    VkSubpassDescriptionFlags flags{};
    VkPipelineBindPoint bind_point{};
    std::vector<VkAttachmentReference> inputs;
    std::vector<VkAttachmentReference> colors;
    std::vector<VkAttachmentReference> resolves; // Empty or one per color.
    uint32_t has_depth{};
    VkAttachmentReference depth{};
    std::vector<uint32_t> preserves;
};

template <typename V>
void Visit(V& v, Subpass& s) {
    v("subpass_flags", s.flags);
    v("bind_point", s.bind_point);
    VisitList(v, "inputs", s.inputs);
    VisitList(v, "colors", s.colors);
    VisitList(v, "resolves", s.resolves);
    v("has_depth", s.has_depth);
    Visit(v, s.depth);
    VisitList(v, "preserves", s.preserves);
}

struct Stage {
    VkShaderStageFlagBits stage{};
    VkPipelineShaderStageCreateFlags flags{};
    std::string entry;
    std::string file; // SPIR-V file next to pipeline.txt
    uint32_t has_specialization{};
};

template <typename V>
void Visit(V& v, Stage& s) {
    v("stage", s.stage);
    v("stage_flags", s.flags);
    v("entry", s.entry);
    v("file", s.file);
    v("has_specialization", s.has_specialization);
}

struct Pipeline {
    uint32_t version{1};
    VkPipelineCreateFlags flags{};
    std::vector<Stage> stages;

    uint32_t has_vertex_input{};
    std::vector<VkVertexInputBindingDescription> vertex_bindings;
    std::vector<VkVertexInputAttributeDescription> vertex_attributes;
    uint32_t has_divisors{};
    std::vector<VkVertexInputBindingDivisorDescriptionEXT> divisors;

    uint32_t has_input_assembly{};
    VkPrimitiveTopology topology{};
    VkBool32 primitive_restart{};

    uint32_t has_tessellation{};
    uint32_t patch_control_points{};

    uint32_t has_viewport{};
    uint32_t viewport_count{};
    uint32_t scissor_count{};
    uint32_t has_swizzle{};
    std::vector<VkViewportSwizzleNV> swizzles;
    uint32_t has_depth_clip_control{};
    VkBool32 negative_one_to_one{};

    uint32_t has_rasterization{};
    VkBool32 depth_clamp{};
    VkBool32 rasterizer_discard{};
    VkPolygonMode polygon_mode{};
    VkCullModeFlags cull_mode{};
    VkFrontFace front_face{};
    VkBool32 depth_bias{};
    float depth_bias_constant{};
    float depth_bias_clamp{};
    float depth_bias_slope{};
    float line_width{};
    uint32_t has_line_state{};
    VkLineRasterizationModeEXT line_mode{};
    VkBool32 stippled_line{};
    uint32_t line_stipple_factor{};
    uint32_t line_stipple_pattern{};
    uint32_t has_conservative{};
    VkConservativeRasterizationModeEXT conservative_mode{};
    float conservative_extra{};
    uint32_t has_provoking_vertex{};
    VkProvokingVertexModeEXT provoking_vertex{};

    uint32_t has_multisample{};
    VkSampleCountFlagBits samples{};
    VkBool32 sample_shading{};
    float min_sample_shading{};
    uint32_t has_sample_mask{};
    uint32_t sample_mask{};
    VkBool32 alpha_to_coverage{};
    VkBool32 alpha_to_one{};

    uint32_t has_depth_stencil{};
    VkBool32 depth_test{};
    VkBool32 depth_write{};
    VkCompareOp depth_compare{};
    VkBool32 depth_bounds_test{};
    VkBool32 stencil_test{};
    VkStencilOpState front{};
    VkStencilOpState back{};
    float min_depth_bounds{};
    float max_depth_bounds{};

    uint32_t has_color_blend{};
    VkBool32 logic_op_enable{};
    VkLogicOp logic_op{};
    std::vector<VkPipelineColorBlendAttachmentState> blend_attachments;
    float blend_constants[4]{};

    uint32_t has_dynamic{};
    std::vector<VkDynamicState> dynamic_states;

    VkPipelineLayoutCreateFlags layout_flags{};
    std::vector<SetLayout> set_layouts;
    std::vector<VkPushConstantRange> push_constants;

    uint32_t has_render_pass{};
    VkRenderPassCreateFlags render_pass_flags{};
    std::vector<VkAttachmentDescription> attachments;
    std::vector<Subpass> subpasses;
    std::vector<VkSubpassDependency> dependencies;
    uint32_t subpass{};

    // sTypes of pNext structures the capture did not understand. Nonempty means the
    // replay differs from the original call.
    std::vector<uint32_t> unknown_structures;
};

template <typename V>
void Visit(V& v, Pipeline& p) {
    v("pipeline_replay", p.version);
    v("flags", p.flags);
    VisitList(v, "stages", p.stages);

    v("has_vertex_input", p.has_vertex_input);
    VisitList(v, "vertex_bindings", p.vertex_bindings);
    VisitList(v, "vertex_attributes", p.vertex_attributes);
    v("has_divisors", p.has_divisors);
    VisitList(v, "divisors", p.divisors);

    v("has_input_assembly", p.has_input_assembly);
    v("topology", p.topology);
    v("primitive_restart", p.primitive_restart);

    v("has_tessellation", p.has_tessellation);
    v("patch_control_points", p.patch_control_points);

    v("has_viewport", p.has_viewport);
    v("viewport_count", p.viewport_count);
    v("scissor_count", p.scissor_count);
    v("has_swizzle", p.has_swizzle);
    VisitList(v, "swizzles", p.swizzles);
    v("has_depth_clip_control", p.has_depth_clip_control);
    v("negative_one_to_one", p.negative_one_to_one);

    v("has_rasterization", p.has_rasterization);
    v("depth_clamp", p.depth_clamp);
    v("rasterizer_discard", p.rasterizer_discard);
    v("polygon_mode", p.polygon_mode);
    v("cull_mode", p.cull_mode);
    v("front_face", p.front_face);
    v("depth_bias", p.depth_bias);
    v("depth_bias_constant", p.depth_bias_constant);
    v("depth_bias_clamp", p.depth_bias_clamp);
    v("depth_bias_slope", p.depth_bias_slope);
    v("line_width", p.line_width);
    v("has_line_state", p.has_line_state);
    v("line_mode", p.line_mode);
    v("stippled_line", p.stippled_line);
    v("line_stipple_factor", p.line_stipple_factor);
    v("line_stipple_pattern", p.line_stipple_pattern);
    v("has_conservative", p.has_conservative);
    v("conservative_mode", p.conservative_mode);
    v("conservative_extra", p.conservative_extra);
    v("has_provoking_vertex", p.has_provoking_vertex);
    v("provoking_vertex", p.provoking_vertex);

    v("has_multisample", p.has_multisample);
    v("samples", p.samples);
    v("sample_shading", p.sample_shading);
    v("min_sample_shading", p.min_sample_shading);
    v("has_sample_mask", p.has_sample_mask);
    v("sample_mask", p.sample_mask);
    v("alpha_to_coverage", p.alpha_to_coverage);
    v("alpha_to_one", p.alpha_to_one);

    v("has_depth_stencil", p.has_depth_stencil);
    v("depth_test", p.depth_test);
    v("depth_write", p.depth_write);
    v("depth_compare", p.depth_compare);
    v("depth_bounds_test", p.depth_bounds_test);
    v("stencil_test", p.stencil_test);
    Visit(v, p.front);
    Visit(v, p.back);
    v("min_depth_bounds", p.min_depth_bounds);
    v("max_depth_bounds", p.max_depth_bounds);

    v("has_color_blend", p.has_color_blend);
    v("logic_op_enable", p.logic_op_enable);
    v("logic_op", p.logic_op);
    VisitList(v, "blend_attachments", p.blend_attachments);
    for (float& constant : p.blend_constants) {
        v("blend_constant", constant);
    }

    v("has_dynamic", p.has_dynamic);
    VisitList(v, "dynamic_states", p.dynamic_states);

    v("layout_flags", p.layout_flags);
    VisitList(v, "set_layouts", p.set_layouts);
    VisitList(v, "push_constants", p.push_constants);

    v("has_render_pass", p.has_render_pass);
    v("render_pass_flags", p.render_pass_flags);
    VisitList(v, "attachments", p.attachments);
    VisitList(v, "subpasses", p.subpasses);
    VisitList(v, "dependencies", p.dependencies);
    v("subpass", p.subpass);

    VisitList(v, "unknown_structures", p.unknown_structures);
}

struct FeatureStruct {
    uint32_t type{};            // VkStructureType
    std::vector<uint8_t> body; // Bytes after sType and pNext
};

template <typename V>
void Visit(V& v, FeatureStruct& f) {
    v("feature_struct", f.type);
    v("body", f.body);
}

struct Device {
    uint32_t version{1};
    std::string device_name; // Spaces replaced with '_'
    uint32_t vendor_id{};
    uint32_t device_id{};
    uint32_t driver_version{};
    uint32_t api_version{};
    std::vector<std::string> extensions;
    std::vector<FeatureStruct> features; // pNext chain of VkDeviceCreateInfo, in order
    std::vector<uint32_t> unknown_structures;
};

template <typename V>
void Visit(V& v, Device& d) {
    v("device_replay", d.version);
    v("device_name", d.device_name);
    v("vendor_id", d.vendor_id);
    v("device_id", d.device_id);
    v("driver_version", d.driver_version);
    v("api_version", d.api_version);
    VisitList(v, "extensions", d.extensions);
    VisitList(v, "features", d.features);
    VisitList(v, "unknown_structures", d.unknown_structures);
}

template <typename T>
std::string Serialize(T value) {
    Writer writer;
    Visit(writer, value);
    return writer.Text();
}

template <typename T>
T Deserialize(const std::string& text) {
    T value{};
    Reader reader{text};
    Visit(reader, value);
    return value;
}

} // namespace PipelineReplay
