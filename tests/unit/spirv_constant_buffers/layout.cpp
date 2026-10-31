// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <bit>
#include <cstring>
#include "yuzu_shader_recompiler/backend/spirv/emit_spirv.h"
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "yuzu_shader_recompiler/backend/spirv/spirv_emit_context.h"

// Inspect the production SPIR-V declarations, not a duplicate sizing formula.
static std::map<std::string, u32> Layout(bool molten, u32 used_size, bool global = false) {
    Shader::Profile profile{};
    profile.disable_uniform_buffer_descriptor_aliasing = molten;
    Shader::RuntimeInfo runtime{};
    Shader::IR::Program program{};
    program.stage = Shader::Stage::Fragment;
    program.info.uses_global_memory = global;
    program.info.constant_buffer_descriptors.push_back({.index = 0, .count = 1});
    program.info.constant_buffer_descriptors.push_back({.index = 3, .count = 1});
    program.info.constant_buffer_used_sizes[0] = 16;
    program.info.constant_buffer_used_sizes[3] = used_size;
    Shader::Backend::Bindings bindings{};
    Shader::Backend::SPIRV::EmitContext context(profile, runtime, program, bindings);
    auto words = context.Assemble();
    std::map<u32, std::string> names;
    std::map<u32, u32> variables, pointers, structs, lengths, constants;
    for (size_t i = 5; i < words.size();) {
        const auto count = words[i] >> 16;
        assert(count != 0 && i + count <= words.size());
        const auto op = static_cast<spv::Op>(words[i] & 0xffff);
        const auto* p = words.data() + i + 1;
        switch (op) {
        case spv::Op::OpName: names[p[0]] = reinterpret_cast<const char*>(p + 1); break;
        case spv::Op::OpVariable: variables[p[1]] = p[0]; break;
        case spv::Op::OpTypePointer: pointers[p[0]] = p[2]; break;
        case spv::Op::OpTypeStruct: if (count == 3) structs[p[0]] = p[1]; break;
        case spv::Op::OpTypeArray: lengths[p[0]] = p[2]; break;
        case spv::Op::OpConstant: if (count == 4) constants[p[1]] = p[2]; break;
        default: break;
        }
        i += count;
    }
    std::map<std::string, u32> result;
    for (auto [id, type] : variables) {
        if (names[id] == "c0" || names[id] == "c3") {
            // MoltenVK uses a canonical uvec4 view: each element is 16 bytes.
            result[names[id]] = constants.at(lengths.at(structs.at(pointers.at(type)))) * 16;
        }
    }
    return result;
}

// Ace Combat's indirect parameter reads target a tracked descriptor at c0[0x290],
// outside the compute NVN table (0x310). These helpers used to return only zero.
static void GlobalMemoryLoads(bool aliasing, bool molten,
                              Shader::Stage stage = Shader::Stage::Compute) {
    Shader::Profile profile{};
    profile.support_int64 = true;
    profile.support_descriptor_aliasing = aliasing;
    profile.disable_uniform_buffer_descriptor_aliasing = molten;
    profile.avoid_global_memory_helper_inlining = molten;
    profile.min_ssbo_alignment = 16;
    Shader::RuntimeInfo runtime{};
    Shader::IR::Program program{};
    program.stage = stage;
    auto& info = program.info;
    info.uses_global_memory = true;
    info.nvn_buffer_base = 0x310;
    info.used_constant_buffer_types = Shader::IR::Type::U32 | Shader::IR::Type::U32x2;
    info.used_storage_buffer_types = Shader::IR::Type::U32 | Shader::IR::Type::U32x2 |
                                     Shader::IR::Type::U32x4;
    info.constant_buffer_descriptors.push_back({.index = 0, .count = 1});
    info.storage_buffers_descriptors.push_back(
        {.cbuf_index = 0, .cbuf_offset = 0x290, .count = 1, .is_written = false});
    assert(!info.nvn_buffer_used.any());
    Shader::Backend::Bindings bindings{};
    Shader::Backend::SPIRV::EmitContext context(profile, runtime, program, bindings);
    const auto words = context.Assemble();
    std::map<u32, unsigned> loads;
    std::map<u32, u32> function_controls;
    u32 function = 0;
    for (size_t i = 5; i < words.size();) {
        const auto count = words[i] >> 16;
        assert(count != 0 && i + count <= words.size());
        const auto op = static_cast<spv::Op>(words[i] & 0xffff);
        if (op == spv::Op::OpFunction) {
            function = words[i + 2];
            function_controls[function] = words[i + 3];
        }
        if (op == spv::Op::OpLoad) ++loads[function];
        if (op == spv::Op::OpFunctionEnd) function = 0;
        i += count;
    }
    // Each helper must read both its descriptor and the selected storage buffer.
    assert(loads[context.load_global_func_u32.value] >= 3);
    assert(loads[context.load_global_func_u32x2.value] >= 3);
    assert(loads[context.load_global_func_u32x4.value] >= 3);
    // Graphics shaders keep inlined helpers: DontInline there made TOTK's Link flicker.
    const bool dont_inline = molten && stage == Shader::Stage::Compute;
    for (const auto function : {context.load_global_func_u32, context.load_global_func_u32x2,
                                context.load_global_func_u32x4, context.write_global_func_u32,
                                context.write_global_func_u32x2, context.write_global_func_u32x4}) {
        assert(function_controls.at(function.value) == static_cast<u32>(
            dont_inline ? spv::FunctionControlMask::DontInline : spv::FunctionControlMask::MaskNone));
    }
}

void TestRuntimeTextureFetch();

static void PushConstantLayout() {
    using namespace Shader::Backend::SPIRV;
    for (auto stage : {Shader::Stage::VertexB, Shader::Stage::Fragment}) {
        for (bool scaling : {false, true}) {
            Shader::Profile profile{};
            profile.unified_descriptor_binding = true;
            Shader::IR::Program program{};
            program.stage = stage;
            program.info.uses_render_area = true;
            program.info.uses_rescaling_uniform = scaling;
            Shader::Backend::Bindings bindings{};
            EmitContext context(profile, {}, program, bindings);
            const auto words = context.Assemble();
            using Member = std::pair<u32, u32>;
            std::map<Member, std::string> names;
            std::map<Member, u32> offsets;
            unsigned push_variables{};
            for (size_t i = 5; i < words.size();) {
                const auto count = words[i] >> 16;
                const auto op = static_cast<spv::Op>(words[i] & 0xffff);
                const auto* args = words.data() + i + 1;
                if (op == spv::Op::OpMemberName)
                    names[{args[0], args[1]}] = reinterpret_cast<const char*>(args + 2);
                if (op == spv::Op::OpMemberDecorate && args[2] == u32(spv::Decoration::Offset))
                    offsets[{args[0], args[1]}] = args[3];
                if (op == spv::Op::OpVariable && args[2] == u32(spv::StorageClass::PushConstant))
                    ++push_variables;
                assert(count != 0);
                i += count;
            }
            assert(push_variables == 1); // Vulkan allows one push-constant block per entry point.
            std::map<std::string, u32> named_offsets;
            for (const auto& [member, name] : names) named_offsets[name] = offsets.at(member);
            assert(named_offsets.at("render_area") == RENDERAREA_LAYOUT_OFFSET);
            if (scaling) {
                assert(named_offsets.at("rescaling_textures") == RESCALING_LAYOUT_WORDS_OFFSET);
                assert(named_offsets.at("render_area") >= sizeof(RescalingLayout));
            }
            for (u32 factor : {1U, 2U, 3U}) {
                std::array<u32, NUM_TEXTURE_AND_IMAGE_SCALING_WORDS> masks{};
                for (size_t i = 0; i < masks.size(); ++i) masks[i] = factor == 1 ? 0 : 0xabcdef01U + u32(i);
                const std::array<float, 4> area{128, 192, 0, 0};
                auto data = MakeGraphicsPushConstants(masks, 1.f / factor, 0xdeadbeefU, area);
                const auto* bytes = reinterpret_cast<const unsigned char*>(&data);
                std::array<float, 4> actual_area;
                std::memcpy(actual_area.data(), bytes + named_offsets.at("render_area"), sizeof(actual_area));
                assert(actual_area == area);
                for (size_t i = 0; i < NUM_TEXTURE_SCALING_WORDS; ++i)
                    assert(data.scaling.rescaling_textures[i] == masks[i]);
                for (size_t i = 0; i < NUM_IMAGE_SCALING_WORDS; ++i)
                    assert(data.scaling.rescaling_images[i] == masks[NUM_TEXTURE_SCALING_WORDS + i]);
                assert(std::bit_cast<float>(data.scaling.down_factor) == 1.f / factor);
                assert(data.scaling.attachment_reads == 0xdeadbeefU);
            }
        }
    }
    std::puts("PASS: non-overlapping render-area/scaling payload, one SPIR-V block, x1/x2/x3");
}

int main() {
    PushConstantLayout();
    TestRuntimeTextureFetch();
    auto sizes = Layout(true, 14080);
    assert(sizes.at("c0") == 16 && sizes.at("c3") == 14080);
    // Regression: offset 116992 within a 131072-byte allocation.
    assert(116992 + sizes.at("c3") <= 131072);
    assert(Layout(true, 17).at("c3") == 32);
    assert(Layout(true, 0).at("c3") == 16);
    assert(Layout(true, 65536).at("c3") == 65536); // indirect accesses
    assert(Layout(true, 16, true).at("c3") == 65536); // global-memory helpers
    assert(Layout(false, 16).at("c3") == 65536); // other driver profiles
    GlobalMemoryLoads(true, false);
    GlobalMemoryLoads(false, false);
    GlobalMemoryLoads(true, true);
    GlobalMemoryLoads(true, true, Shader::Stage::Fragment);
    std::puts("PASS: SPIR-V constant-buffer layouts and non-NVN global-memory loads");
}
