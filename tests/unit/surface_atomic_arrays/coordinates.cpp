// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdio>
#include <stdexcept>
#include "yuzu_shader_recompiler/frontend/maxwell/translate/impl/impl.h"

namespace IR = Shader::IR;
class Environment final : public Shader::Environment {
public:
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::ColorArray2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return Shader::TexturePixelFormat::R32_UINT; }
    bool IsTexturePixelFormatInteger(u32) override { return true; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32,3> WorkgroupSize() const override { return {1,1,1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32,u32) override { return {}; }
    void Dump(u64,u64) override {}
};
unsigned failures{};
void Check(bool ok, const char* label) {
    if (!ok) { ++failures; std::fprintf(stderr,"FAIL: %s\n",label); }
}
u32 Eval(IR::Value value, const std::array<u32,256>& regs) {
    value=value.Resolve();
    if(value.IsImmediate()) return value.U32();
    const auto& inst=*value.Inst();
    switch(inst.GetOpcode()) {
    case IR::Opcode::GetRegister: return regs.at(static_cast<size_t>(inst.Arg(0).Reg()));
    case IR::Opcode::BitFieldUExtract: {
        const auto offset=Eval(inst.Arg(1),regs), bits=Eval(inst.Arg(2),regs);
        return (Eval(inst.Arg(0),regs)>>offset)&((1U<<bits)-1);
    }
    default: throw std::runtime_error("unexpected scalar coordinate opcode");
    }
}
void Verify(u64 word, bool reduction, u32 type, bool bound,
            IR::Opcode expected_opcode = IR::Opcode::Void) {
    Shader::ObjectPool<IR::Inst> pool{64}; IR::Block block{pool}; Environment env;
    Shader::Maxwell::TranslatorVisitor visitor{env,block};
    try {
        if(reduction) visitor.SURED(word); else visitor.SUATOM(word);
        const IR::Inst* atomic{};
        if (expected_opcode == IR::Opcode::Void) expected_opcode =
            bound ? IR::Opcode::BoundImageAtomicIAdd32 : IR::Opcode::BindlessImageAtomicIAdd32;
        unsigned stores{};
        for(const auto& inst:block.Instructions()) {
            if(inst.GetOpcode()==expected_opcode) atomic=&inst;
            stores+=inst.GetOpcode()==IR::Opcode::SetRegister;
        }
        Check(atomic!=nullptr,"translation emits the requested image atomic operation");
        if(!atomic)return;
        Check(stores==(reduction?0U:1U),"only SUATOM writes a result register");
        constexpr std::array types{Shader::TextureType::Color1D,Shader::TextureType::Buffer,
            Shader::TextureType::ColorArray1D,Shader::TextureType::Color2D,
            Shader::TextureType::ColorArray2D,Shader::TextureType::Color3D};
        Check(atomic->Flags<IR::TextureInstInfo>().type==types.at(type),"texture dimensionality retained");
        Check(atomic->GetOpcode()==expected_opcode, "operation and handle mode retained");
        std::array<u32,256> regs{};
        const u32 base=(word>>8)&255;
        regs[base]=0x12345678;regs[(base+1)&255]=0xabcd2345;regs[(base+2)&255]=0xfedc6789;
        const auto coords=atomic->Arg(1).Resolve();
        if(type<=1) Check(Eval(coords,regs)==regs[base],"scalar coordinate unchanged");
        else {
            const auto& vector=*coords.Inst();
            const u32 count=(type==2||type==3)?2:3;
            Check(vector.NumArgs()==count,"coordinate component count");
            for(u32 component=0;component<count;++component) {
                u32 expected=regs[base+component];
                const bool array_layer=(type==2||type==4)&&component==count-1;
                if(array_layer) expected&=0xffff;
                Check(Eval(vector.Arg(component),regs)==expected,
                      array_layer?"array layer uses only the low 16 bits":"spatial coordinate retains its full value");
            }
        }
    } catch(const std::exception& e) { ++failures;std::fprintf(stderr,"FAIL type=%u reduction=%u: %s\n",type,reduction,e.what()); }
}
int main() {
    // Exact SURED word from Pokémon Z-A shader 7863af07078f69a3, byte offset 0x370.
    Verify(0xeb5006080207000fULL,true,4,false,IR::Opcode::BindlessImageAtomicUMax32);
    for(u32 type=0;type<6;++type) for(bool reduction:{false,true}) for(bool bound:{false,true}) {
        u64 word=(u64(type)<<33)|(u64(8)<<8);
        if(reduction) word|=u64(bound)<<51;
        else word|=(u64(!bound)<<54)|2; // destination R2
        Verify(word,reduction,type,bound);
    }
    using O = IR::Opcode;
    constexpr std::array bound_ops{O::BoundImageAtomicIAdd32,O::BoundImageAtomicUMin32,
        O::BoundImageAtomicUMax32,O::BoundImageAtomicInc32,O::BoundImageAtomicDec32,
        O::BoundImageAtomicAnd32,O::BoundImageAtomicOr32,O::BoundImageAtomicXor32};
    constexpr std::array bindless_ops{O::BindlessImageAtomicIAdd32,O::BindlessImageAtomicUMin32,
        O::BindlessImageAtomicUMax32,O::BindlessImageAtomicInc32,O::BindlessImageAtomicDec32,
        O::BindlessImageAtomicAnd32,O::BindlessImageAtomicOr32,O::BindlessImageAtomicXor32};
    for(u32 op=0;op<8;++op) for(bool bound:{false,true}) {
        const u64 word=(u64(4)<<33)|(u64(8)<<8)|(u64(op)<<24)|(u64(bound)<<51);
        Verify(word,true,4,bound,bound?bound_ops[op]:bindless_ops[op]);
    }
    // Unsupported addressing, sizes, clamp modes and dimensions must still fail.
    for(u64 fields:{u64(1)<<23,u64(2)<<20,u64(1)<<49,u64(6)<<33}) {
        Shader::ObjectPool<IR::Inst> pool{32};IR::Block block{pool};Environment env;
        Shader::Maxwell::TranslatorVisitor visitor{env,block};
        bool rejected{};
        try { visitor.SURED(fields); } catch(const std::exception&) { rejected=true; }
        Check(rejected,"unsupported SURED variants are rejected");
    }
    return failures?1:0;
}
