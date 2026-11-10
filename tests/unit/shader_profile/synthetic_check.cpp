// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile_reuse.h"
#include "yuzu_video_core/surface.h"
#include "video_settings.h"
#include "yuzu_common/logging/log.h"
#include <iostream>
#include <stdexcept>
#include <string>
VideoSettings videoSettings{};
namespace Common::Log {
LogLevel g_classLevel[static_cast<uint8_t>(LogClass::Count)]{};
void FmtLogMessageImpl(LogClass, LogLevel, const char*, unsigned int, const char*, const char*, const fmt::format_args&) {}
}
void assert_fail_impl() {throw std::runtime_error("renderer assertion");}
[[noreturn]] void unreachable_impl() {throw std::runtime_error("renderer unreachable");}
using namespace VideoCommon::ShaderProfile;
void Check(bool value){if(!value)throw std::runtime_error("Synthetic first-draw check regression");}
// A live draw: the constant buffers, texture descriptors and state a synthetic environment's questions read.
struct Live final : Shader::Environment {
    u32 local{},bound{2},viewport{1};bool hle{};
    std::unordered_map<u64,u32> constants;
    std::unordered_map<u32,Shader::TextureType> types;
    std::unordered_map<u32,Shader::TexturePixelFormat> formats;
    std::unordered_map<u64,Shader::ReplaceConstant> replacements;
    explicit Live(Shader::Stage value,u32 start=0x30){stage=value;start_address=start;}
    u64 ReadInstruction(u32) override {throw std::runtime_error("Check read instructions");}
    u32 ReadCbufValue(u32 b,u32 o) override {return constants.at((u64{b}<<32)|o);}
    u32 ReadCbufSize(u32 b) override {const auto it=constants.find((u64{b}<<32)|UINT32_MAX);return it==constants.end()?256:it->second;}
    Shader::TextureType ReadTextureType(u32 h) override {return types.at(h);}
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 h) override {return formats.at(h);}
    bool IsTexturePixelFormatInteger(u32 h) override {
        return VideoCore::Surface::IsPixelFormatInteger(static_cast<VideoCore::Surface::PixelFormat>(formats.at(h)));
    }
    u32 ReadViewportTransformState() override {return viewport;}
    u32 TextureBoundBuffer() const override {return bound;}
    u32 LocalMemorySize() const override {return local;}
    u32 SharedMemorySize() const override {return 0;}
    std::array<u32,3> WorkgroupSize() const override {return {1,1,1};}
    bool HasHLEMacroState() const override {return hle;}
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank,u32 offset) override {
        const auto it=replacements.find((u64{bank}<<32)|offset);
        return it==replacements.end()?std::nullopt:std::optional{it->second};
    }
    void Dump(u64,u64) override {}
};
int main() try {
    using T=Shader::TextureType;using F=Shader::TexturePixelFormat;using K=Shader::TextureQueryKind;
    constexpr u64 Slot0=(u64{2}<<32)|0x20, Slot1=(u64{2}<<32)|0x28, Constant=(u64{3}<<32)|0x10, Size=(u64{3}<<32)|UINT32_MAX;
    // As the shader list tool writes them: handles of its own, read from the texture bank's slots.
    constexpr u32 Generated0=0x00100001, Generated1=0x00200002, Real0=0x00500017, Real1=0x00600021;
    const std::unordered_map<u64,u32> constants{{Slot0,Generated0},{Slot1,Generated1},{Constant,7},{Size,256},{(u64{2}<<32)|0x30,99}};
    const std::unordered_map<u32,T> types{{Generated0,T::Color2D},{Generated1,T::ColorArray2D},{0x00300003,T::Color3D}};
    const std::unordered_map<u32,F> formats{{Generated1,F::D32_FLOAT}};
    const std::unordered_map<u64,Shader::ReplaceConstant> no_replacements;
    const Live header(Shader::Stage::Fragment);
    const DependencyProof fragment{Shader::Stage::Fragment,0x30,0,0,2,1,{1,1,1},header.SPH(),header.GpPassthroughMask(),types,formats,constants,no_replacements};
    const auto source=[](u64 key) {
        Shader::TextureHandleSource result{};
        result.cbuf_index=static_cast<u32>(key>>32);result.cbuf_offset=static_cast<u32>(key);result.count=1;
        return result;
    };
    // What the translation asked: the code read one constant and a size; two textures were queried. The third
    // texture slot the environment answers, as the tool answers every slot, was never asked.
    AskedQuestions asked;
    asked.constants={Constant,Size};
    asked.Texture({source(Slot0),K::Type,Generated0,static_cast<u32>(T::Color2D)});
    asked.Texture({source(Slot1),K::Type,Generated1,static_cast<u32>(T::ColorArray2D)});
    asked.Texture({source(Slot1),K::Format,Generated1,static_cast<u32>(F::D32_FLOAT)});
    asked.Texture({source(Slot1),K::Type,Generated1,static_cast<u32>(T::ColorArray2D)});
    Check(asked.textures.size()==3);
    const auto live=[&](Shader::Stage stage=Shader::Stage::Fragment) {
        Live result(stage);
        result.constants={{Slot0,Real0},{Slot1,Real1},{Constant,7}};
        result.types={{Real0,T::Color2D},{Real1,T::ColorArray2D}};
        result.formats={{Real1,F::D32_FLOAT}};
        return result;
    };
    unsigned cases{};
    const auto answer=[&](const DependencyProof& q,const AskedQuestions& questions,Live l,bool relocatable=false) {
        const char* mismatch{};
        auto result=AnswerSynthetic(q,questions,l,relocatable,mismatch);
        Check(result.has_value()==(mismatch==nullptr));
        ++cases;
        return std::pair{std::move(result),std::string(mismatch?mismatch:"")};
    };
    {
        // Matching meaning: the generated handles become the live ones, and only asked questions are answered.
        const auto [answers,why]=answer(fragment,asked,live());
        Check(answers && answers->constants==std::unordered_map<u64,u32>{{Slot0,Real0},{Slot1,Real1},{Constant,7},{Size,256}} &&
              answers->texture_types==std::unordered_map<u32,T>{{Real0,T::Color2D},{Real1,T::ColorArray2D}} &&
              answers->pixel_formats.size()==1 && answers->pixel_formats.at(Real1)==F::D32_FLOAT);
    }
    const auto differs=[&](const DependencyProof& q,const AskedQuestions& questions,Live l,const std::string& expected,bool relocatable=false) {
        const auto [answers,why]=answer(q,questions,std::move(l),relocatable);
        if(answers || why!=expected)std::cerr<<"expected "<<expected<<", got "<<(answers?"a match":why)<<'\n';
        Check(!answers && why==expected);
    };
    auto changed=live();changed.types[Real0]=T::Color1D;differs(fragment,asked,changed,"texture type");
    changed=live();changed.bound=3;differs(fragment,asked,changed,"texture bank");
    changed=live();changed.constants[Constant]=8;differs(fragment,asked,changed,"constant value");
    changed=live();changed.constants[Size]=128;differs(fragment,asked,changed,"constant buffer size");
    changed=live();changed.local=16;differs(fragment,asked,changed,"local memory");
    changed=live();changed.hle=true;changed.replacements[Slot0]=Shader::ReplaceConstant::BaseInstance;differs(fragment,asked,changed,"HLE replacement");
    changed=Live(Shader::Stage::VertexB);differs(fragment,asked,changed,"stage");
    // A constant the code also uses itself, beside finding a texture, must keep its value: a texture of the
    // same kind is not enough (SemanticTextureProof::OnlyTextureConstant).
    {
        auto both=asked;both.constants.insert(Slot0);
        differs(fragment,both,live(),"constant value");
        changed=live();changed.constants[Slot0]=Generated0;changed.types[Generated0]=T::Color2D;
        const auto [answers,why]=answer(fragment,both,changed);
        Check(answers.has_value());
    }
    // A type or format asked by handle, outside a query, holds a generated handle: never confirmed elsewhere.
    {
        auto direct=asked;direct.texture_types.insert(Generated0);
        differs(fragment,direct,live(),"live read");
    }
    // A format query needs the very format: raw image loads specialize on it (RawImageReadFormat), so even
    // formats of one texel size and class translate differently.
    changed=live();changed.formats[Real1]=F::A8B8G8R8_UNORM;differs(fragment,asked,changed,"texture format");
    {
        const std::unordered_map<u32,F> wide{{Generated1,F::R32G32_FLOAT}};
        const DependencyProof q{Shader::Stage::Fragment,0x30,0,0,2,1,{1,1,1},header.SPH(),header.GpPassthroughMask(),types,wide,constants,no_replacements};
        AskedQuestions format;
        format.Texture({source(Slot1),K::Format,Generated1,static_cast<u32>(F::R32G32_FLOAT)});
        changed=live();changed.formats[Real1]=F::R16G16B16A16_FLOAT;differs(q,format,changed,"texture format");
        changed=live();changed.formats[Real1]=F::R32G32_FLOAT;{const auto [a,w]=answer(q,format,changed);Check(a.has_value());}
    }
    {
        AskedQuestions integer;
        integer.Texture({source(Slot1),K::Integer,Generated1,0});
        {const auto [a,w]=answer(fragment,integer,live());Check(a.has_value() && a->pixel_formats.at(Real1)==F::D32_FLOAT);}
        changed=live();changed.formats[Real1]=F::R32_UINT;differs(fragment,integer,changed,"texture format");
    }
    // The start address matters only for a program that cannot be relocated.
    changed=Live(Shader::Stage::Fragment,0x1000);changed.constants=live().constants;changed.types=live().types;changed.formats=live().formats;
    differs(fragment,asked,changed,"start address");
    {const auto [a,w]=answer(fragment,asked,changed,true);Check(a.has_value());}
    // Fragment stages never read the viewport transform; a stage before rasterization that asked depends on it.
    {
        auto viewport=asked;viewport.viewport=true;
        changed=live();changed.viewport=0;{const auto [a,w]=answer(fragment,viewport,changed);Check(a.has_value());}
        const Live vertex_header(Shader::Stage::VertexB);
        const DependencyProof vertex{Shader::Stage::VertexB,0x30,0,0,2,1,{1,1,1},vertex_header.SPH(),vertex_header.GpPassthroughMask(),types,formats,constants,no_replacements};
        changed=live(Shader::Stage::VertexB);changed.viewport=0;differs(vertex,viewport,changed,"viewport transform");
        {const auto [a,w]=answer(vertex,viewport,live(Shader::Stage::VertexB));Check(a.has_value());}
        {const auto [a,w]=answer(vertex,asked,changed);Check(a.has_value());}
    }
    // HLE replacements: a translation for an HLE draw asked about every constant buffer load, ordinary runtime
    // loads included, and was told none is replaced. The live draw must replace none of them.
    {
        constexpr u64 Load=(u64{1}<<32)|0x40; // A runtime load: neither a constant the code read nor a texture source.
        AskedQuestions hle=asked;
        hle.hle=true;
        hle.replacements={{Load,std::nullopt},{Slot0,std::nullopt}};
        changed=live();changed.hle=true;
        {const auto [a,w]=answer(fragment,hle,changed);Check(a.has_value());}
        changed.replacements[Load]=Shader::ReplaceConstant::DrawID;differs(fragment,hle,changed,"HLE replacement");
        changed=live();changed.hle=true;changed.replacements[Load]=Shader::ReplaceConstant::BaseVertex;differs(fragment,hle,changed,"HLE replacement");
        // A load replaced by the draw that the translation was told about, with the same answer, still matches.
        auto replaced=hle;replaced.replacements[Load]=Shader::ReplaceConstant::BaseInstance;
        changed=live();changed.hle=true;changed.replacements[Load]=Shader::ReplaceConstant::BaseInstance;
        {const auto [a,w]=answer(fragment,replaced,changed);Check(a.has_value());}
        // A plain draw cannot prove what an HLE draw would replace.
        differs(fragment,hle,live(),"HLE draw");
    }
    // A live read that fails (a handle the draw does not bind) is a difference, never a crash.
    changed=live();changed.types.erase(Real0);differs(fragment,asked,changed,"live read");
    std::cout<<cases<<" synthetic first-draw cases passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr<<error.what()<<'\n';
    return 1;
}
