// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
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
static void GlobalMemoryLoads(bool aliasing, bool molten) {
    Shader::Profile profile{};
    profile.support_int64 = true;
    profile.support_descriptor_aliasing = aliasing;
    profile.disable_uniform_buffer_descriptor_aliasing = molten;
    profile.min_ssbo_alignment = 16;
    Shader::RuntimeInfo runtime{};
    Shader::IR::Program program{};
    program.stage = Shader::Stage::Compute;
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
    u32 function = 0;
    for (size_t i = 5; i < words.size();) {
        const auto count = words[i] >> 16;
        assert(count != 0 && i + count <= words.size());
        const auto op = static_cast<spv::Op>(words[i] & 0xffff);
        if (op == spv::Op::OpFunction) function = words[i + 2];
        if (op == spv::Op::OpLoad) ++loads[function];
        if (op == spv::Op::OpFunctionEnd) function = 0;
        i += count;
    }
    // Each helper must read both its descriptor and the selected storage buffer.
    assert(loads[context.load_global_func_u32.value] >= 3);
    assert(loads[context.load_global_func_u32x2.value] >= 3);
    assert(loads[context.load_global_func_u32x4.value] >= 3);
}

void TestRuntimeTextureFetch();

int main() {
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
    std::puts("PASS: SPIR-V constant-buffer layouts and non-NVN global-memory loads");
}
