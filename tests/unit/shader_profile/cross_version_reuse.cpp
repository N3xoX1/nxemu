// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile_reuse.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace VideoCommon::ShaderProfile;
void Check(bool value){if(!value)throw std::runtime_error("Cross-version proof regression");}
struct Live final : Shader::Environment {
    u32 local{},shared{},bound{},viewport{1},table{7};bool hle{};
    std::array<u32,3> group{1,1,1};
    std::unordered_map<u64,u32> constants{{(u64{2}<<32)|32,42},{(u64{3}<<32)|UINT32_MAX,64}};
    std::unordered_map<u32,Shader::TextureType> types{{9,static_cast<Shader::TextureType>(0)}};
    std::unordered_map<u32,Shader::TexturePixelFormat> formats{{9,static_cast<Shader::TexturePixelFormat>(1)}};
    Shader::TexturePoolInfo pool{10,12,false};unsigned reads{};
    std::unordered_map<u64,Shader::ReplaceConstant> live_replacements;
    Live(){stage=Shader::Stage::Compute;}
    void Stage(Shader::Stage value){stage=value;}
    void ChangeHeader(){reinterpret_cast<unsigned char*>(&sph)[0]^=1;}
    void ChangeMask(){gp_passthrough_mask[0]^=1;}
    void Start(u32 value){start_address=value;}
    u64 ReadInstruction(u32) override {throw std::runtime_error("Proof reread instructions");}
    u32 ReadCbufValue(u32 b,u32 o) override {++reads;return constants.at((u64{b}<<32)|o);}
    u32 ReadCbufSize(u32 b) override {++reads;const auto it=constants.find((u64{b}<<32)|UINT32_MAX);return it==constants.end()?128:it->second;}
    u32 ReadTextureTableSize() override {++reads;return table;}
    std::optional<Shader::TexturePoolInfo> ReadTexturePoolInfo() override {++reads;return pool;}
    Shader::TextureType ReadTextureType(u32 h) override {++reads;return types.at(h);}
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 h) override {++reads;return formats.at(h);}
    bool IsTexturePixelFormatInteger(u32) override {throw std::runtime_error("Unexpected format query");}
    u32 ReadViewportTransformState() override {return viewport;}
    u32 TextureBoundBuffer() const override {return bound;}
    u32 LocalMemorySize() const override {return local;}
    u32 SharedMemorySize() const override {return shared;}
    std::array<u32,3> WorkgroupSize() const override {return group;}
    bool HasHLEMacroState() const override {return hle;}
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank,u32 offset) override {
        ++reads;if(!hle)return {};
        const auto it=live_replacements.find((u64{bank}<<32)|offset);
        return it==live_replacements.end()?std::nullopt:std::optional{it->second};
    }
    void Dump(u64,u64) override {}
};
int main()try {
    Live source;
    source.constants[UINT64_MAX]=7;
    source.constants[UINT64_MAX-1]=10;
    source.constants[UINT64_MAX-2]=12;
    const std::unordered_map<u64,Shader::ReplaceConstant> replacements;
    DependencyProof proof{Shader::Stage::Compute,0,0,0,0,1,{1,1,1},source.SPH(),source.GpPassthroughMask(),source.types,source.formats,source.constants,replacements};
    unsigned cases{};
    {
        // The same cbuf is safe to relocate only when it was used exclusively
        // to classify a texture. No literal/arithmetic use may be weakened.
        Live before;
        before.constants[(u64{2}<<32)|32]=9;
        const std::unordered_map<u64,u32> constants{{(u64{2}<<32)|32,9}};
        const std::unordered_map<u32,Shader::TexturePixelFormat> no_formats;
        std::unordered_map<u64,std::optional<Shader::ReplaceConstant>> no_replacements;
        Shader::TextureHandleSource handle{};
        handle.cbuf_index=2;handle.cbuf_offset=32;handle.count=1;
        SemanticTextureProof semantic;
        semantic.Add({handle,Shader::TextureQueryKind::Type,9,0});
        auto relocation=DependencyProof{Shader::Stage::Compute,0,0,0,0,1,{1,1,1},before.SPH(),before.GpPassthroughMask(),before.types,no_formats,constants,replacements,
            &no_replacements,true,false,false,false,&semantic};
        Live after=before;
        after.constants[(u64{2}<<32)|32]=77;
        after.types.erase(9);after.types[77]=static_cast<Shader::TextureType>(0);
        Check(relocation.Matches(after));++cases;
        after.types[77]=static_cast<Shader::TextureType>(1);
        Check(!relocation.Matches(after));++cases;
        after.types[77]=static_cast<Shader::TextureType>(0);
        semantic.Strict(0,(u64{2}<<32)|32);
        Check(!relocation.Matches(after));++cases;
        semantic.strict_reads.clear();semantic.Strict(1,9);
        after.types[9]=static_cast<Shader::TextureType>(1);
        Check(!relocation.Matches(after));++cases;
        semantic.strict_reads.clear();semantic.complete=false;
        Check(!relocation.Matches(after));++cases;
        semantic.complete=true;
        after.constants[(u64{2}<<32)|UINT32_MAX]=0;
        Check(!relocation.Matches(after));++cases;
        // Separate TIC/TSC sources, shifts and constant OR bits are replayed.
        after.constants.erase((u64{2}<<32)|UINT32_MAX);
        after.constants[(u64{3}<<32)|40]=2;
        semantic.queries[0].source.has_secondary=true;
        semantic.queries[0].source.secondary_cbuf_index=3;
        semantic.queries[0].source.secondary_cbuf_offset=40;
        semantic.queries[0].source.secondary_shift_left=20;
        after.types[77|(2U<<20)]=static_cast<Shader::TextureType>(0);
        Check(relocation.Matches(after));++cases;
        semantic.queries[0].source.shift_left=32;
        Check(!relocation.Matches(after));++cases;
        semantic.queries[0].source.shift_left=0;
        for(unsigned i=0;i<=CandidateWarmupBudget::MaxStrictTextureReads;++i)semantic.Strict(0,i);
        Check(!semantic.complete && !relocation.Matches(after));++cases;
    }
    const auto valid=[&](Live live){Check(proof.Matches(live));++cases;};
    const auto invalid=[&](Live live){Check(!proof.Matches(live));++cases;};
    valid(Live{});
    auto changed=Live{};changed.local=4;invalid(changed);
    changed=Live{};changed.shared=16;invalid(changed);
    changed=Live{};changed.bound=2;invalid(changed);
    changed=Live{};changed.viewport=0;invalid(changed);
    changed=Live{};changed.table=8;invalid(changed);
    changed=Live{};changed.group[0]=2;invalid(changed);
    changed=Live{};changed.Start(8);invalid(changed);
    changed=Live{};changed.Stage(Shader::Stage::Fragment);invalid(changed);
    changed=Live{};changed.constants[(u64{2}<<32)|32]=43;invalid(changed);
    changed=Live{};changed.constants[(u64{2}<<32)|UINT32_MAX]=0;invalid(changed);
    changed=Live{};changed.constants[(u64{3}<<32)|UINT32_MAX]=128;invalid(changed);
    changed=Live{};changed.types[9]=static_cast<Shader::TextureType>(1);invalid(changed);
    changed=Live{};changed.formats[9]=static_cast<Shader::TexturePixelFormat>(2);invalid(changed);
    changed=Live{};changed.pool.num_textures=11;invalid(changed);
    changed=Live{};changed.pool.num_samplers=13;invalid(changed);
    changed=Live{};changed.pool.linked_samplers=true;invalid(changed);
    changed=Live{};changed.hle=true;invalid(changed);
    // Unqueried inputs are irrelevant, not a reason to reject an update.
    changed=Live{};changed.constants[(u64{1}<<32)|64]=999;changed.types[99]=static_cast<Shader::TextureType>(1);valid(changed);
    auto bad_constants=source.constants;
    bad_constants[(u64{1000}<<32)|UINT32_MAX]=1;
    DependencyProof bad{proof.stage,proof.start,proof.local_memory,proof.shared_memory,proof.texture_bound,proof.viewport,proof.workgroup,proof.header,proof.passthrough,proof.texture_types,proof.pixel_formats,bad_constants,replacements};
    Check(!bad.Matches(changed));++cases;
    auto geometry=proof;geometry.stage=Shader::Stage::Geometry;
    auto geometric=Live{};geometric.Stage(Shader::Stage::Geometry);
    Check(geometry.Matches(geometric));++cases;
    geometric.ChangeHeader();Check(!geometry.Matches(geometric));++cases;
    geometric.ChangeHeader();geometric.ChangeMask();Check(!geometry.Matches(geometric));++cases;
    // Positive AND negative HLE queries are learned while preparing the local
    // pipeline, so shared files need no new fields or assumed-empty tables.
    std::unordered_map<u64,std::optional<Shader::ReplaceConstant>> observed{
        {(u64{2}<<32)|64,std::nullopt},
        {(u64{2}<<32)|68,Shader::ReplaceConstant::BaseVertex}};
    auto hle_proof=geometry;hle_proof.complete=true;hle_proof.hle=true;
    hle_proof.replacement_queries=&observed;hle_proof.viewport_queried=false;
    auto hle_live=Live{};hle_live.Stage(Shader::Stage::Geometry);hle_live.hle=true;
    hle_live.viewport=0; // Not queried by this prepared shader.
    hle_live.live_replacements[(u64{2}<<32)|68]=Shader::ReplaceConstant::BaseVertex;
    Check(hle_proof.Matches(hle_live));++cases;
    hle_live.live_replacements[(u64{2}<<32)|72]=Shader::ReplaceConstant::DrawID;
    Check(hle_proof.Matches(hle_live));++cases; // Unqueried, irrelevant.
    hle_live.live_replacements[(u64{2}<<32)|64]=Shader::ReplaceConstant::DrawID;
    Check(!hle_proof.Matches(hle_live));++cases; // A formerly negative query changed.
    hle_live.live_replacements.erase((u64{2}<<32)|64);
    hle_live.live_replacements[(u64{2}<<32)|68]=Shader::ReplaceConstant::BaseInstance;
    Check(!hle_proof.Matches(hle_live));++cases;
    hle_live.live_replacements.erase((u64{2}<<32)|68);
    Check(!hle_proof.Matches(hle_live));++cases;
    hle_live.live_replacements[(u64{2}<<32)|68]=Shader::ReplaceConstant::BaseVertex;
    hle_proof.viewport_queried=true;
    Check(!hle_proof.Matches(hle_live));++cases;
    hle_live.viewport=1;Check(hle_proof.Matches(hle_live));++cases;
    hle_proof.complete=false;Check(!hle_proof.Matches(hle_live));++cases;
    auto missing_pool_proof=proof;missing_pool_proof.missing_pool=true;
    auto pool_live=Live{};Check(!missing_pool_proof.Matches(pool_live));++cases;
    ProgramEquivalence programs;
    auto original=std::make_shared<const std::vector<u64>>(8192,918);
    Check(programs.CanRebase(original,Shader::Stage::Compute,0,256));
    Check(!programs.CanRebase(original,Shader::Stage::Compute,0,8));
    Check(!programs.CanRebase(original,Shader::Stage::Compute,0,UINT32_MAX-7));
    for(u64 opcode:{0xe200000000000000ULL,0xe210000000000000ULL,0xe220000000000000ULL,0x50d0000000000000ULL}) {
        auto absolute=std::make_shared<std::vector<u64>>(16,918);(*absolute)[1]=opcode;
        Check(!programs.CanRebase(absolute,Shader::Stage::Compute,0,256));
    }
    auto relocated=Live{};relocated.Start(256);
    Check(!proof.Matches(relocated));Check(proof.Matches(relocated,true));++cases;
    auto identical=std::make_shared<const std::vector<u64>>(*original);
    auto modified=std::make_shared<std::vector<u64>>(*original);modified->back()^=1;
    const auto digest=ResourceSha256({reinterpret_cast<const char*>(original->data()),original->size()*8});
    Check(programs.MatchDigest(digest,original->size()*8,identical));
    Check(!programs.MatchDigest(digest,original->size()*8,modified));
    Check(!programs.MatchDigest(digest,8,identical));
    Check(!programs.MatchDigest("",original->size()*8,identical));
    Check(!programs.MatchDigest(digest,original->size()*8,{}));
    // Simulate base -> update: only exact code plus every relevant query may
    // activate the prepared pipeline. Missing/new/changed inputs compile anew.
    unsigned reused{},compiled{};
    for(unsigned i=0;i<1000;++i) {
        auto current=Live{};
        if(i%4==1)current.constants[(u64{2}<<32)|32]^=1;
        const std::shared_ptr<const std::vector<u64>> code=i%4>=2?modified:identical;
        if(programs.MatchDigest(digest,original->size()*8,code) && proof.Matches(current))++reused;
        else ++compiled;
    }
    Check(reused==250 && compiled==750);
    // Expired owners and address reuse cannot turn a different program equal.
    for(unsigned i=0;i<1000;++i)Check(!programs.MatchDigest(digest,65536,std::make_shared<const std::vector<u64>>(8192,i)) || i==918);
    auto small=std::make_shared<const std::vector<u64>>(16,918);
    const auto small_digest=ResourceSha256({reinterpret_cast<const char*>(small->data()),128});
    for(unsigned i=0;i<17000;++i)
        Check(!programs.MatchDigest(small_digest,128,std::make_shared<const std::vector<u64>>(16,i)) || i==918);
    Check(programs.MatchDigest(small_digest,128,std::make_shared<const std::vector<u64>>(16,918)));
    Check(!programs.MatchDigest(small_digest,128,{}));
    const auto cold_begin=std::chrono::steady_clock::now();
    for(unsigned i=0;i<1000;++i) {
        ProgramEquivalence cold;Check(cold.MatchDigest(digest,65536,identical));
    }
    const auto cold_ns=std::chrono::duration<double,std::nano>(
        std::chrono::steady_clock::now()-cold_begin).count()/1000;
    constexpr unsigned repetitions=100000;
    auto live=Live{};
    const auto begin=std::chrono::steady_clock::now();
    for(unsigned i=0;i<repetitions;++i)Check(programs.MatchDigest(digest,original->size()*8,identical) && proof.Matches(live));
    const auto ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-begin).count()/repetitions;
    std::cout<<"{\"dependency_mutation_cases\":"<<cases<<",\"update_candidates\":1000,\"reused\":"<<reused
        <<",\"recompiled\":"<<compiled<<",\"expired_owner_cases\":18000,\"fake_memory_proof_ns\":"<<ns
        <<",\"first_64k_program_digest_ns\":"<<cold_ns<<",\"note\":\"Fake memory queries; no GPU memory synchronization or Vulkan calls\"}\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
