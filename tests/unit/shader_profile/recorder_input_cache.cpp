// SPDX-License-Identifier: GPL-2.0-or-later
#include "recorder_fixture.h"
#include <deque>
struct CollidingHash {u64 operator()(const void*,size_t) const noexcept{return 0;}};
int main()try {
    GuestRecipeInputCache<CollidingHash> cache;
    std::deque<Journal::Entry> retained;
    auto regs=RecorderRegisters();std::array<u32,104> prefix{};
    // Force every raw and combined hash to collide. Exact comparisons, token
    // retirement and bounded eviction must preserve every distinct recipe.
    for(size_t i=0;i<300;++i){
        const u32 value=u32(i<(40)?i%4:i%9);
        regs->point_size=float(value+1);prefix[0]=u32(i/15%3);
        const auto hint=i%2?Tegra::Engines::Maxwell3D::EngineHint::None:Tegra::Engines::Maxwell3D::EngineHint::OnHLEMacro;
        const auto result=cache.Find(*regs,GuestPipelineState::All,true,Maxwell::PrimitiveTopology::Triangles,hint,63,prefix.data(),true);
        const auto words=LegacyWords(GuestPipelineState::Capture(*regs,Maxwell::PrimitiveTopology::Triangles,hint));
        Journal::Entry expected{};expected[0]=63;std::copy(words.begin(),words.end(),expected.begin()+1);std::copy(prefix.begin(),prefix.end(),expected.begin()+279);
        if(result.hit)Check(result.state->entry && *result.state->entry==expected);
        else {retained.push_back(expected);result.state->entry=&retained.back();}
    }
    Check(cache.StorageBytes()<6*1024*1024);
    std::cout<<"PASS: forced collisions, exact recipe identities, bounded eviction; bytes="<<cache.StorageBytes()<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
