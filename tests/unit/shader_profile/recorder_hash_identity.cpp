// SPDX-License-Identifier: GPL-2.0-or-later
#include "recorder_fixture.h"
using Dirty = Tegra::Engines::Maxwell3D::DirtyState;
int main(int argc,char** argv)try {
    Check(argc==2);
    // The same value change on two registers of a group must change the group hash.
    // A weak mix let vertex attributes 1 and 4 going from RGBA8 SNORM to UNORM cancel out.
    constexpr size_t attributes=offsetof(Maxwell,vertex_attrib_format)/4;
    for(const auto [before,after]:{std::pair<u32,u32>{0x09400000,0x11400000},{1,2},{0x100,0x200},{0,0x80000000},{0x1234,0x4321}})
        for(size_t a=0;a<Maxwell::NumVertexAttributes;++a)
            for(size_t b=a+1;b<Maxwell::NumVertexAttributes;++b)
                Check((Dirty::ShaderProfileMix(attributes+a,before)^Dirty::ShaderProfileMix(attributes+b,before))!=
                      (Dirty::ShaderProfileMix(attributes+a,after)^Dirty::ShaderProfileMix(attributes+b,after)));

    // The same case through the engine's incremental path: both vertex formats keep their recipe.
    auto path=std::filesystem::path(argv[1]);
    Check(!std::filesystem::exists(path));
    auto regs=RecorderRegisters();
    auto dirty=std::make_unique<Dirty>();
    const DynamicFeatures features{};
    const auto topology=Maxwell::PrimitiveTopology::Triangles;
    const auto hint=Tegra::Engines::Maxwell3D::EngineHint::None;
    const auto write=[&](size_t method,u32 value){
        const u32 old=regs->reg_array[method];
        if(old==value)return;
        regs->reg_array[method]=value;
        dirty->MarkShaderProfileRegister(method,old,value);
    };
    std::vector<Journal::Entry> expected;
    Common::ThreadWorker writer(1,"hash identity test");
    {
        GuestPipelineRecorder recorder(path,writer);
        for(const u32 format:{0x09400000U,0x11400000U,0x09400000U,0x11400000U}){
            write(attributes+1,format);
            write(attributes+4,format);
            const auto guest=GuestPipelineState::Capture(*regs,topology,hint);
            GraphicsPipelineCacheKey key;key.state=guest.Project(features);
            Journal::Entry entry{};const auto words=LegacyWords(guest);
            std::copy(words.begin(),words.end(),entry.begin()+1);std::copy(key.prefix.begin(),key.prefix.end(),entry.begin()+279);
            if(std::find(expected.begin(),expected.end(),entry)==expected.end())expected.push_back(entry);
            recorder.ObserveState(*dirty,*regs,topology,hint,regs.get(),key,features);
        }
        recorder.Flush();
    }
    writer.WaitForRequests();
    Check(expected.size()==2 && Journal::Read(path).entries==expected);
    std::filesystem::remove(path);
    std::cout<<"PASS: equal value changes on two registers keep distinct recipes\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
