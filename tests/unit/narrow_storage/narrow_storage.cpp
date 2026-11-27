// SPDX-FileCopyrightText: Copyright 2026 NXEmu Contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// 8-bit and 16-bit storage buffer accesses must use native narrow types only when the device
// supports them in storage buffers, and otherwise go through the 32-bit alias and the CAS
// helper. Every device feature combination is emitted and checked for a valid helper call and
// for descriptor aliases that exist. If NXEMU_SPIRV_VAL names spirv-val, each module is also
// validated with it.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "nxemu-video/video_settings.h"
#include "yuzu_shader_recompiler/backend/bindings.h"
#include "yuzu_shader_recompiler/backend/spirv/emit_spirv.h"
#include "yuzu_shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "yuzu_shader_recompiler/frontend/ir/basic_block.h"
#include "yuzu_shader_recompiler/frontend/ir/ir_emitter.h"
#include "yuzu_shader_recompiler/frontend/ir/program.h"
#include "yuzu_shader_recompiler/object_pool.h"

VideoSettings videoSettings{};
namespace IR = Shader::IR;

namespace {
struct Features {
    bool int8, int16, storage8, storage16, aliasing;
};

struct Access {
    bool u8, u16, load;
};

std::vector<u32> Compile(const Features& features, const Access& access) {
    Shader::ObjectPool<IR::Inst> inst_pool{64};
    Shader::ObjectPool<IR::Block> block_pool{4};
    IR::Block* const block = block_pool.Create(inst_pool);
    IR::IREmitter ir{*block};
    const IR::U32 binding = ir.Imm32(0);
    const auto emit = [&](IR::Opcode op, std::initializer_list<IR::Value> args) {
        return IR::Value{&*block->PrependNewInst(block->end(), op, args)};
    };
    const auto narrow = [&](IR::Opcode load, IR::Opcode store, u32 offset) {
        const IR::Value value = access.load ? emit(load, {binding, ir.Imm32(offset)})
                                            : IR::Value{ir.Imm32(0x1234567U)};
        emit(store, {binding, ir.Imm32(offset + 2), value});
    };
    if (access.u8) {
        narrow(IR::Opcode::LoadStorageU8, IR::Opcode::WriteStorageU8, 5);
        narrow(IR::Opcode::LoadStorageS8, IR::Opcode::WriteStorageS8, 9);
    }
    if (access.u16) {
        narrow(IR::Opcode::LoadStorageU16, IR::Opcode::WriteStorageU16, 4);
        narrow(IR::Opcode::LoadStorageS16, IR::Opcode::WriteStorageS16, 12);
    }

    IR::Program program{};
    program.stage = Shader::Stage::Compute;
    program.workgroup_size = {1, 1, 1};
    program.blocks.push_back(block);
    program.post_order_blocks.push_back(block);
    program.info.uses_int8 = access.u8;
    program.info.uses_int16 = access.u16;
    if (access.u8) {
        program.info.used_storage_buffer_types |= IR::Type::U8;
    }
    if (access.u16) {
        program.info.used_storage_buffer_types |= IR::Type::U16;
    }
    program.info.storage_buffers_descriptors.push_back({
        .cbuf_index = 0,
        .cbuf_offset = 0,
        .count = 1,
        .is_written = true,
    });
    program.syntax_list.push_back(
        {.data{.block = block}, .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});

    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.support_int8 = features.int8;
    profile.support_int16 = features.int16;
    profile.support_storage_buffer_8bit = features.storage8;
    profile.support_storage_buffer_16bit = features.storage16;
    profile.support_descriptor_aliasing = features.aliasing;
    Shader::RuntimeInfo runtime{};
    Shader::Backend::Bindings bindings{};
    return Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
}

// Checks the ids that the narrow storage paths can leave undefined.
std::string Inspect(const std::vector<u32>& words, bool& has_narrow_types) {
    std::set<u32> functions, variables, defined;
    std::vector<std::pair<u32, u32>> calls, chains;
    has_narrow_types = false;
    for (size_t i = 5; i < words.size();) {
        const u32 count = words[i] >> 16;
        if (count == 0 || i + count > words.size()) {
            return "malformed instruction stream";
        }
        const auto op = static_cast<spv::Op>(words[i] & 0xffff);
        const u32* const w = words.data() + i;
        switch (op) {
        case spv::Op::OpFunction:
            functions.insert(w[2]);
            break;
        case spv::Op::OpVariable:
            variables.insert(w[2]);
            break;
        case spv::Op::OpFunctionCall:
            calls.emplace_back(w[3], static_cast<u32>(i));
            break;
        case spv::Op::OpAccessChain:
            chains.emplace_back(w[3], static_cast<u32>(i));
            break;
        case spv::Op::OpTypeInt:
            has_narrow_types = has_narrow_types || w[2] < 32;
            break;
        default:
            break;
        }
        i += count;
    }
    for (const auto& [callee, at] : calls) {
        if (!functions.contains(callee)) {
            return "call to undefined function %" + std::to_string(callee);
        }
    }
    for (const auto& [base, at] : chains) {
        if (!variables.contains(base)) {
            return "access chain on undefined variable %" + std::to_string(base);
        }
    }
    return {};
}

bool Validate(const std::vector<u32>& words, const std::string& name) {
    const char* const validator = std::getenv("NXEMU_SPIRV_VAL");
    if (validator == nullptr || *validator == '\0') {
        return true;
    }
    const std::string path = name + ".spv";
    {
        std::ofstream file{path, std::ios::binary};
        file.write(reinterpret_cast<const char*>(words.data()),
                   static_cast<std::streamsize>(words.size() * sizeof(u32)));
    }
    const std::string command =
        std::string{"\""} + validator + "\" --target-env vulkan1.1 \"" + path + "\"";
#ifdef _WIN32
    const int result = std::system(("\"" + command + "\"").c_str());
#else
    const int result = std::system(command.c_str());
#endif
    std::remove(path.c_str());
    return result == 0;
}
} // namespace

int main() {
    int failures = 0;
    int checked = 0;
    for (u32 bits = 0; bits < 32; ++bits) {
        const Features features{(bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0,
                                (bits & 8) != 0, (bits & 16) != 0};
        for (u32 access_bits = 1; access_bits < 8; ++access_bits) {
            const Access access{(access_bits & 1) != 0, (access_bits & 2) != 0,
                                (access_bits & 4) != 0};
            const bool native8 = features.int8 && features.storage8 && features.aliasing;
            const bool native16 = features.int16 && features.storage16 && features.aliasing;
            const std::string name = "narrow_storage_f" + std::to_string(bits) + "_a" +
                                     std::to_string(access_bits);
            std::string error;
            bool has_narrow_types = false;
            try {
                const std::vector<u32> words = Compile(features, access);
                error = Inspect(words, has_narrow_types);
                if (error.empty() && !Validate(words, name)) {
                    error = "spirv-val rejected the module";
                }
            } catch (const std::exception& e) {
                error = std::string{"emission threw: "} + e.what();
            }
            // Narrow integer types are only declared for accesses done natively.
            if (error.empty() && has_narrow_types && !((access.u8 && features.int8) ||
                                                       (access.u16 && features.int16))) {
                error = "declares narrow types the device lacks";
            }
            ++checked;
            if (!error.empty()) {
                ++failures;
                std::fprintf(stderr,
                             "FAIL %s: int8=%d int16=%d storage8=%d storage16=%d aliasing=%d "
                             "u8=%d u16=%d load=%d native8=%d native16=%d: %s\n",
                             name.c_str(), features.int8, features.int16, features.storage8,
                             features.storage16, features.aliasing, access.u8, access.u16,
                             access.load, native8, native16, error.c_str());
            }
        }
    }
    std::fprintf(stderr, "%d/%d narrow storage modules failed\n", failures, checked);
    return failures == 0 ? 0 : 1;
}
