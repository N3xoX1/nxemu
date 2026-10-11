// Exercise the captured lighting volume's dimensions/format and production barriers.
#define main PriorTestMain
#include "../pr396/vulkan-synthetic.cpp"
#undef main
#include "barrier-values.inc"
struct VolumeFixture : Fixture {
    static constexpr uint32_t width=32,height=32,depth=136,count=width*height*depth;
    VkImage image{};VkDeviceMemory memory{};VkImageView view{};VkSampler sampler{};
    VkDescriptorSetLayout volume_layout{};VkDescriptorPool volume_pool{};VkDescriptorSet volume_set{};
    VkPipelineLayout volume_pipeline_layout{};VkShaderModule volume_shader{};VkPipeline volume_pipeline{};
    bool initialized{};
    VolumeFixture(const char* read_shader,const char* volume_path):Fixture(read_shader) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_3D;
        ci.format=VK_FORMAT_R16G16B16A16_SFLOAT;ci.extent={width,height,depth};
        ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;
        ci.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
        Check(vkCreateImage(device,&ci,nullptr,&image));VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device,image,&req);uint32_t type=0;while(!(req.memoryTypeBits&(1u<<type)))++type;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
        Check(vkAllocateMemory(device,&ai,nullptr,&memory));Check(vkBindImageMemory(device,image,memory,0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_3D;
        vi.format=ci.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};Check(vkCreateImageView(device,&vi,nullptr,&view));
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sci.magFilter=sci.minFilter=VK_FILTER_LINEAR;
        sci.addressModeU=sci.addressModeV=sci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        Check(vkCreateSampler(device,&sci,nullptr,&sampler));
        std::array<VkDescriptorSetLayoutBinding,3> bindings{{
            {0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};lci.bindingCount=3;lci.pBindings=bindings.data();
        Check(vkCreateDescriptorSetLayout(device,&lci,nullptr,&volume_layout));
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,8};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};plci.setLayoutCount=1;plci.pSetLayouts=&volume_layout;
        plci.pushConstantRangeCount=1;plci.pPushConstantRanges=&push;Check(vkCreatePipelineLayout(device,&plci,nullptr,&volume_pipeline_layout));
        std::array<VkDescriptorPoolSize,3> sizes{{{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pci.maxSets=1;pci.poolSizeCount=3;pci.pPoolSizes=sizes.data();
        Check(vkCreateDescriptorPool(device,&pci,nullptr,&volume_pool));
        VkDescriptorSetAllocateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};si.descriptorPool=volume_pool;si.descriptorSetCount=1;si.pSetLayouts=&volume_layout;
        Check(vkAllocateDescriptorSets(device,&si,&volume_set));
        std::array<VkDescriptorImageInfo,2> infos{{{VK_NULL_HANDLE,view,VK_IMAGE_LAYOUT_GENERAL},{sampler,view,VK_IMAGE_LAYOUT_GENERAL}}};
        VkDescriptorBufferInfo output{buffers[0].buffer,0,count*sizeof(uint32_t)};
        std::array<VkWriteDescriptorSet,3> writes{};
        for(uint32_t i=0;i<3;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=volume_set;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=bindings[i].descriptorType;
            if(i<2)writes[i].pImageInfo=&infos[i];else writes[i].pBufferInfo=&output;}
        vkUpdateDescriptorSets(device,3,writes.data(),0,nullptr);
        std::ifstream file(volume_path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("Missing volume shader");
        auto length=file.tellg();std::vector<uint32_t> code(static_cast<size_t>(length)/4);file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),length);
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};smci.codeSize=code.size()*4;smci.pCode=code.data();Check(vkCreateShaderModule(device,&smci,nullptr,&volume_shader));
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cpci.layout=volume_pipeline_layout;
        cpci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};cpci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;cpci.stage.module=volume_shader;cpci.stage.pName="main";
        Check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cpci,nullptr,&volume_pipeline));
    }
    ~VolumeFixture(){vkDestroyPipeline(device,volume_pipeline,nullptr);vkDestroyShaderModule(device,volume_shader,nullptr);vkDestroyPipelineLayout(device,volume_pipeline_layout,nullptr);vkDestroyDescriptorPool(device,volume_pool,nullptr);vkDestroyDescriptorSetLayout(device,volume_layout,nullptr);vkDestroySampler(device,sampler,nullptr);vkDestroyImageView(device,view,nullptr);vkDestroyImage(device,image,nullptr);vkFreeMemory(device,memory,nullptr);}
    void Prepare(VkCommandBuffer cb){
        if(!initialized){VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);initialized=true;}
        else Barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,compute_src,compute_dst);
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,volume_pipeline);vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,volume_pipeline_layout,0,1,&volume_set,0,nullptr);
    }
    void Dispatch(VkCommandBuffer cb,uint32_t phase,uint32_t seed){std::array<uint32_t,2> params{phase,seed};vkCmdPushConstants(cb,volume_pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,8,params.data());vkCmdDispatch(cb,8,8,34);}
    void Pair(VkCommandBuffer cb,uint32_t seed,bool synced){Dispatch(cb,0,seed);if(synced)Barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,compute_src,compute_dst);Dispatch(cb,1,seed);}
    void Verify(bool synced){
        for(uint32_t seed=0;seed<16;++seed){Begin();auto cb=commands[1];Prepare(cb);Pair(cb,seed,synced);Barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);Submit();
            uint32_t mismatches=0;for(uint32_t z=0;z<depth;++z)for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)mismatches+=buffers[0].mapped[(z*height+y)*width+x]!=((x^y^z)+seed)%4+1;
            if(synced&&mismatches)throw std::runtime_error("Stale or invalid lighting volume");
            std::cout<<"VOLUME seed="<<seed<<" synced="<<synced<<" checked="<<count<<" mismatches="<<mismatches<<'\n';}
    }
    void Benchmark(){std::vector<double> samples;
        for(uint32_t run=0;run<25;++run){Begin();auto cb=commands[1];Prepare(cb);vkCmdResetQueryPool(cb,queries,0,2);vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,0);
            for(uint32_t pair=0;pair<64;++pair){Pair(cb,pair,true);Barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,compute_src,compute_dst);}
            vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,1);Submit();std::array<uint64_t,2> times{};Check(vkGetQueryPoolResults(device,queries,0,2,sizeof(times),times.data(),8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
            if(run>=4)samples.push_back((times[1]-times[0])*properties.limits.timestampPeriod/1000.0/64);}
        std::sort(samples.begin(),samples.end());std::cout<<"VOLUME_BENCH producer-consumer median_us="<<samples[samples.size()/2]<<" samples=21\n";}
};
int main(int argc,char** argv)try{if(argc!=4)return 2;VolumeFixture f(argv[1],argv[2]);std::string mode=argv[3];if(mode=="bench")f.Benchmark();else f.Verify(mode=="synced");}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
