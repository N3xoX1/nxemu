#define main OriginalGpuMain
#include "../pr396/vulkan-synthetic.cpp"
#undef main
#include "yuzu_common/differential_memory_copy.h"

int main(int argc,char** argv)try{
    if(argc!=2)return 2;
    Fixture f(argv[1]);std::vector<uint32_t> mirror(Fixture::bytes/4,0x12345678);
    const auto fill=[&](u64 offset,u64 size,uint32_t value){
        f.Begin();vkCmdFillBuffer(f.commands[1],f.buffers[0].buffer,offset,size,value);
        Fixture::Barrier(f.commands[1],VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
        f.Submit();
    };
    fill(0,Fixture::bytes,0x12345678);
    if(Common::CopyChangedMemory(mirror.data(),f.buffers[0].mapped,Fixture::bytes)!=0)throw std::runtime_error("unchanged GPU download wrote bytes");
    std::cout<<"PASS real_GPU_equal_payload_is_preserved_without_RAM_writes\n";
    fill(65536,65536,0x87654321);
    if(Common::CopyChangedMemory(mirror.data(),f.buffers[0].mapped,Fixture::bytes)!=65536)throw std::runtime_error("sparse GPU write count");
    for(size_t i=0;i<mirror.size();++i)if(mirror[i]!=(i>=65536/4&&i<131072/4?0x87654321:0x12345678))throw std::runtime_error("sparse GPU result mismatch");
    std::cout<<"PASS real_GPU_sparse_changed_chunk_matches_complete_payload\n";
    fill(0,Fixture::bytes,0xF0E1D2C3);
    Common::CopyChangedMemory(mirror.data(),f.buffers[0].mapped,Fixture::bytes);
    for(auto value:mirror)if(value!=0xF0E1D2C3)throw std::runtime_error("dense GPU result mismatch");
    std::cout<<"PASS real_GPU_dense_changes_complete_before_CPU_consumer\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
