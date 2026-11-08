// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "yuzu_shader_recompiler/backend/spirv/spirv_emit_context.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace IR = Shader::IR;
namespace SPIRV = Shader::Backend::SPIRV;
using Sirit::Id;
void Require(bool condition)
{
    if (!condition) throw std::runtime_error("SPIR-V Phi regression");
}
struct TypeCase
{
    IR::Type type;
    unsigned width;
    unsigned components;
    bool floating;
};
constexpr std::array<TypeCase, 20> cases{{TypeCase{IR::Type::U1, 1, 1, false}, {IR::Type::U8, 8, 1, false}, {IR::Type::U16, 16, 1, false}, {IR::Type::U32, 32, 1, false}, {IR::Type::U64, 64, 1, false}, {IR::Type::F16, 16, 1, true}, {IR::Type::F32, 32, 1, true}, {IR::Type::F64, 64, 1, true}, {IR::Type::U32x2, 32, 2, false}, {IR::Type::U32x3, 32, 3, false}, {IR::Type::U32x4, 32, 4, false}, {IR::Type::F16x2, 16, 2, true}, {IR::Type::F16x3, 16, 3, true}, {IR::Type::F16x4, 16, 4, true}, {IR::Type::F32x2, 32, 2, true}, {IR::Type::F32x3, 32, 3, true}, {IR::Type::F32x4, 32, 4, true}, {IR::Type::F64x2, 64, 2, true}, {IR::Type::F64x3, 64, 3, true}, {IR::Type::F64x4, 64, 4, true}}};

void Test(const TypeCase & test, bool available, const std::filesystem::path & output, bool force_info = false)
{
    Shader::Profile profile{};
    profile.support_int8 = available;
    profile.support_int16 = available;
    profile.support_int64 = available;
    IR::Program program{};
    program.stage = Shader::Stage::Compute;
    program.workgroup_size = {1, 1, 1};
    program.info.uses_int8 = available || force_info;
    program.info.uses_int16 = available || force_info;
    program.info.uses_int64 = available || force_info;
    program.info.uses_fp16 = available || force_info;
    program.info.uses_fp64 = available || force_info;
    Shader::Backend::Bindings bindings{};
    SPIRV::EmitContext context{profile, {}, program, bindings};
    Id scalar;
    if (test.type == IR::Type::U1)
        scalar = context.U1;
    else if (test.floating)
        scalar = test.width == 16 ? context.F16[1] : test.width == 32 ? context.F32[1]
                                                                      : context.F64[1];
    else
        scalar = test.width == 8 ? context.U8 : test.width == 16 ? context.U16
                                            : test.width == 32   ? context.U32[1]
                                                                 : context.U64;
    const Id expected = test.components == 1 || !Sirit::ValidId(scalar) ? scalar : context.TypeVector(scalar, test.components);
    const auto function = context.OpFunction(context.void_id, spv::FunctionControlMask::MaskNone,
                                             context.TypeFunction(context.void_id));
    const auto entry = context.OpLabel();
    const auto merge = context.OpLabel();
    context.AddLabel(entry);
    const Id value = Sirit::ValidId(expected) ? context.OpUndef(expected) : Id{};
    context.OpBranch(merge);
    context.AddLabel(merge);
    Shader::ObjectPool<IR::Inst> instructions{1};
    IR::Block predecessor{instructions};
    predecessor.SetDefinition(entry);
    IR::Inst phi{IR::Opcode::Phi, static_cast<u32>(test.type)};
    phi.AddPhiOperand(&predecessor, IR::Value{0U});
    const bool conditional = test.width != 32 && test.type != IR::Type::U1;
    bool rejected{};
    try
    {
        SPIRV::EmitPhi(context, &phi);
    }
    catch (const Shader::NotImplementedException &)
    {
        rejected = true;
    }
    Require(rejected == (conditional && !available));
    if (rejected) return;
    context.OpReturn();
    context.OpFunctionEnd();
    context.AddEntryPoint(spv::ExecutionModel::GLCompute, function, "main");
    context.AddExecutionMode(function, spv::ExecutionMode::LocalSize, 1, 1, 1);
    // Supply the incoming value and inspect the type selected by the production emitter.
    context.PatchDeferredPhi([value](size_t) { return value; });
    const auto words = context.Assemble();
    unsigned phis{};
    for (size_t i = 5; i < words.size();)
    {
        const auto count = words[i] >> 16;
        Require(count != 0 && i + count <= words.size());
        if (static_cast<spv::Op>(words[i] & 0xffff) == spv::Op::OpPhi)
        {
            Require(count == 5 && words[i + 1] == expected.value);
            Require(words[i + 3] == value.value && words[i + 4] == entry.value);
            ++phis;
        }
        i += count;
    }
    Require(phis == 1);
    if (!output.empty())
    {
        std::filesystem::create_directories(output);
        const auto filename = std::to_string(static_cast<u32>(test.type)) +
                              (available ? "-enabled.spv" : "-base.spv");
        std::ofstream stream{output / filename, std::ios::binary};
        stream.write(reinterpret_cast<const char *>(words.data()),
                     static_cast<std::streamsize>(words.size() * sizeof(u32)));
        Require(stream.good());
    }
}
void TestUnsupportedTypes()
{
    Shader::Profile profile{};
    IR::Program program{};
    program.stage = Shader::Stage::Compute;
    Shader::Backend::Bindings bindings{};
    SPIRV::EmitContext context{profile, {}, program, bindings};
    for (const auto type : {IR::Type::Void, IR::Type::Opaque, IR::Type::Reg,
                            IR::Type::U32 | IR::Type::F32})
    {
        IR::Inst phi{IR::Opcode::Phi, static_cast<u32>(type)};
        bool rejected{};
        try
        {
            SPIRV::EmitPhi(context, &phi);
        }
        catch (const Shader::NotImplementedException &)
        {
            rejected = true;
        }
        Require(rejected);
    }
}
int main(int argc, char ** argv)
{
    TestUnsupportedTypes();
    const std::filesystem::path output = argc > 1 ? argv[1] : "";
    for (const auto & test : cases)
    {
        Test(test, true, output);
        Test(test, false, output);
        if (!test.floating && test.width != 32 && test.type != IR::Type::U1)
        {
            Test(test, false, {}, true); // Requested integer type, unsupported by this profile.
        }
    }
}
