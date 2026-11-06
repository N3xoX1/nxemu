// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>

#include "yuzu_shader_recompiler/environment.h"
#include "yuzu_shader_recompiler/exception.h"
#include "yuzu_shader_recompiler/frontend/ir/attribute.h"
#include "yuzu_shader_recompiler/frontend/ir/basic_block.h"
#include "yuzu_shader_recompiler/frontend/maxwell/translate/impl/impl.h"
#include "yuzu_shader_recompiler/object_pool.h"

namespace {
namespace IR = Shader::IR;

void Require(bool condition) {
    if (!condition) {
        throw std::runtime_error("shader surface store regression");
    }
}

class TestEnvironment final : public Shader::Environment {
public:
    u64 ReadInstruction(u32) override { throw std::runtime_error("ReadInstruction"); }
    u32 ReadCbufValue(u32, u32) override { throw std::runtime_error("ReadCbufValue"); }
    Shader::TextureType ReadTextureType(u32) override {
        throw std::runtime_error("ReadTextureType");
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        throw std::runtime_error("ReadTexturePixelFormat");
    }
    bool IsTexturePixelFormatInteger(u32) override {
        throw std::runtime_error("IsTexturePixelFormatInteger");
    }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

struct Translation {
    TestEnvironment environment;
    Shader::ObjectPool<IR::Inst> pool{64};
    IR::Block block{pool};
    Shader::Maxwell::TranslatorVisitor translator{environment, block};
};

constexpr u64 SurfaceStore2D = 0xeb28053600170200ULL;
constexpr u64 SourceRegisterMask = 0xffULL;
constexpr u64 ComponentMask = 0xfULL << 20;

u64 MakeInstruction(u32 component_mask, u32 source_register) {
    return (SurfaceStore2D & ~ComponentMask & ~SourceRegisterMask) |
           (u64{component_mask} << 20) | u64{source_register};
}

void TestSurfaceStore(u32 mask, u32 source_register, u32 count) {
    Translation t;
    t.translator.SUST(MakeInstruction(mask, source_register));

    const auto& store = t.block.back();
    Require(store.GetOpcode() == IR::Opcode::BoundImageWrite);
    const auto flags = store.Flags<IR::TextureInstInfo>();
    Require(flags.image_format == Shader::ImageFormat::Typeless);

    const auto* color = store.Arg(2).Inst();
    Require(color != nullptr);
    Require(color->GetOpcode() == IR::Opcode::CompositeConstructU32x4);

    for (u32 component = 0; component < 4; ++component) {
        const auto value = color->Arg(component);
        if (component < count) {
            Require(!value.IsImmediate());
            Require(value.Inst()->GetOpcode() == IR::Opcode::GetRegister);
            Require(static_cast<u32>(value.Inst()->Arg(0).Reg()) == source_register + component);
        } else {
            Require(value.IsImmediate() && value.U32() == 0);
        }
    }

    u32 writes{};
    u32 image_reads{};
    u32 register_reads{};
    for (const auto& inst : t.block) {
        writes += inst.GetOpcode() == IR::Opcode::BoundImageWrite;
        image_reads += inst.GetOpcode() == IR::Opcode::BoundImageRead ||
                       inst.GetOpcode() == IR::Opcode::BindlessImageRead;
        register_reads += inst.GetOpcode() == IR::Opcode::GetRegister;
    }
    Require(writes == 1);
    Require(image_reads == 0);
    Require(register_reads == count + 2); // Two coordinate registers.
}

void TestUnsupportedMasks() {
    for (u32 mask = 0; mask < 16; ++mask) {
        if (mask == 1 || mask == 3 || mask == 15) {
            continue;
        }
        Translation t;
        bool rejected = false;
        try {
            t.translator.SUST(MakeInstruction(mask, 4));
        } catch (const Shader::NotImplementedException&) {
            rejected = true;
        }
        Require(rejected);
        for (const auto& inst : t.block) {
            Require(inst.GetOpcode() != IR::Opcode::BoundImageWrite);
        }
    }
}

void TestGenericAttributes() {
    for (u32 index = 0; index < IR::NUM_GENERICS; ++index) {
        for (u32 component = 0; component < 4; ++component) {
            const auto attribute = static_cast<IR::Attribute>(
                static_cast<u32>(IR::Attribute::Generic0X) + index * 4 + component);
            Require(IR::IsGeneric(attribute));
            Require(IR::GenericAttributeIndex(attribute) == index);
            Require(IR::GenericAttributeElement(attribute) == component);
        }
    }
    Require(!IR::IsGeneric(IR::Attribute::PositionW));
    Require(!IR::IsGeneric(IR::Attribute::ColorFrontDiffuseR));
}
} // namespace

int main() {
    TestGenericAttributes();
    TestSurfaceStore(1, 254, 1);
    TestSurfaceStore(3, 252, 2);
    TestSurfaceStore(15, 4, 4);
    TestUnsupportedMasks();
}
