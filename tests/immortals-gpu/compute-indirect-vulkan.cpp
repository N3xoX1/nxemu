// Real GPU conversion of Maxwell QMD fields, followed by an indirect consumer.
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
using u64=uint64_t;
using u32=uint32_t;
void Check(VkResult result) {
    if(result!=VK_SUCCESS) throw std::runtime_error("Vulkan error "+std::to_string(result));
}
#include "indirect-fixture.inc"

struct Test : Fixture {
    VkShaderModule consumer_module{};
    VkPipeline consumer{};
    VkDescriptorSet consumer_set{};
    Test(const char* convert, const char* count) : Fixture(convert) {
        buffers.push_back(MakeBuffer());
        std::ifstream stream(count,std::ios::binary|std::ios::ate);
        if(!stream) throw std::runtime_error("Missing counter shader");
        auto size=stream.tellg();std::vector<u32> code(static_cast<size_t>(size)/4);
        stream.seekg(0);stream.read(reinterpret_cast<char*>(code.data()),size);
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.codeSize=code.size()*4;sm.pCode=code.data();
        Check(vkCreateShaderModule(device,&sm,nullptr,&consumer_module));
        VkComputePipelineCreateInfo pc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pc.layout=pipeline_layout;pc.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pc.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pc.stage.module=consumer_module;pc.stage.pName="main";
        Check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pc,nullptr,&consumer));
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool=descriptor_pool;allocate.descriptorSetCount=1;allocate.pSetLayouts=&layout;
        Check(vkAllocateDescriptorSets(device,&allocate,&consumer_set));
        VkDescriptorBufferInfo info{buffers[3].buffer,0,4};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet=consumer_set;write.dstBinding=0;write.descriptorCount=1;
        write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&info;
        vkUpdateDescriptorSets(device,1,&write,0,nullptr);
    }
    ~Test(){vkDeviceWaitIdle(device);vkDestroyPipeline(device,consumer,nullptr);
            vkDestroyShaderModule(device,consumer_module,nullptr);}
    void Convert(VkCommandBuffer cmd,const std::array<u32,5>& params) {
#include "indirect-barriers.inc"
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0,1,&input,0,nullptr,0,nullptr);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,nullptr);
        vkCmdPushConstants(cmd,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,20,params.data());
        vkCmdDispatch(cmd,1,1,1);
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                             0,1,&output,0,nullptr,0,nullptr);
    }
    void Count(VkCommandBuffer cmd) {
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,consumer);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&consumer_set,0,nullptr);
        vkCmdDispatchIndirect(cmd,buffers[2].buffer,0);
    }
    bool Case(const char* name,u32 x,u32 yz,std::array<u32,5> params,
              std::array<u32,3> expected,bool consume=true) {
        std::fill_n(buffers[0].mapped,4,0xdeadbeef);
        std::fill_n(buffers[1].mapped,4,0xfefefefe);
        std::fill_n(buffers[2].mapped,4,0xffffffff);
        buffers[3].mapped[0]=0;
        Begin();auto cmd=commands[1];
        // GPU writes the source fields, including nonzero offsets within descriptors.
        vkCmdFillBuffer(cmd,buffers[0].buffer,params[0]*4,4,x);
        vkCmdFillBuffer(cmd,buffers[1].buffer,params[1]*4,4,yz);
        Convert(cmd,params);if(consume)Count(cmd);
        Barrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
        Submit();
        const bool dimensions=std::equal(expected.begin(),expected.end(),buffers[2].mapped);
        const u32 count=expected[0]*expected[1]*expected[2];
        const bool counter=!consume||buffers[3].mapped[0]==count;
        std::cout<<(dimensions&&counter?"PASS ":"FAIL ")<<name<<" observed=("
                 <<buffers[2].mapped[0]<<','<<buffers[2].mapped[1]<<','<<buffers[2].mapped[2]
                 <<") invocations="<<buffers[3].mapped[0]<<'\n';
        return dimensions&&counter;
    }
    void Benchmark(bool convert) {
        buffers[0].mapped[0]=1;buffers[1].mapped[0]=0x00010001;
        std::fill_n(buffers[2].mapped,3,1);buffers[3].mapped[0]=0;
        std::vector<double> samples;constexpr u32 iterations=1000;
        for(u32 run=0;run<12;++run) {
            Begin();auto cmd=commands[1];vkCmdResetQueryPool(cmd,queries,0,2);
            vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,0);
            for(u32 i=0;i<iterations;++i) {
                if(convert)Convert(cmd,{0,0,1,0x00010001,3});
                Count(cmd);
                // Match the guest compute dependency after every launch.
                Barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
            }
            vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,1);
            Submit();std::array<u64,2> result{};
            Check(vkGetQueryPoolResults(device,queries,0,2,sizeof(result),result.data(),8,
                                       VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
            if(run>=3)samples.push_back((result[1]-result[0])*properties.limits.timestampPeriod/1000.0/iterations);
        }
        std::sort(samples.begin(),samples.end());
        std::cout<<"GPU_BENCH "<<(convert?"QMD_conversion_and_indirect":"native_indirect")
                 <<" median_us_per_launch="<<samples[samples.size()/2]<<" samples="<<samples.size()
                 <<" batch="<<iterations<<'\n';
    }
};
int main(int argc,char** argv) {
    try {
        if(argc<3)throw std::runtime_error("converter.spv counter.spv [bench]");
        Test f(argv[1],argv[2]);
        if(argc>3){f.Benchmark(false);f.Benchmark(true);return 0;}
        int failures=0,tests=0;
        auto check=[&](const char* name,u32 x,u32 yz,std::array<u32,5> p,std::array<u32,3> expected,bool consume=true){
            ++tests;failures+=!f.Case(name,x,yz,p,expected,consume);
        };
        check("GPU_X_only_keeps_static_YZ",693,0,{0,0,0,0x00010001,1},{693,1,1});
        check("GPU_YZ_only_keeps_static_X",0,0x00030002,{0,0,7,0,2},{7,2,3});
        check("packed_YZ_does_not_read_a_third_source_word",1,1,{0,0,99,99,3},{1,1,0});
        check("GPU_zero_X_does_not_run",0,0x00010001,{0,0,99,99,3},{0,1,1});
        check("GPU_zero_Y_does_not_run",1,0x00010000,{0,0,99,99,3},{1,0,1});
        check("GPU_zero_Z_does_not_run",1,1,{0,0,99,99,3},{1,1,0});
        check("X_flag_is_masked",0x80000005,0x00020003,{0,0,0,0,3},{5,3,2});
        check("independent_descriptor_offsets",2,0x00050003,{1,2,0,0,3},{2,3,5});
        check("static_fields_without_dynamic_mask",999,999,{0,0,4,0x00020003,0},{4,3,2});
        check("maximum_field_widths",0xffffffff,0xffffffff,{0,0,0,0,3},{0x7fffffff,65535,65535},false);
        for(u32 i=1;i<=10;++i)check("varying_dimensions",i,(i+1)|((i+2)<<16),{0,0,0,0,3},{i,i+1,i+2});
        std::cout<<"tests="<<tests<<" failures="<<failures<<'\n';return failures?1:0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
}
