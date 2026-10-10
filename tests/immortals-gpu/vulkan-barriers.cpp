// Real GPU tests, with masks extracted from the production renderer.
#define main PriorTestMain
#include "../pr396/vulkan-synthetic.cpp"
#undef main
#include "barrier-values.inc"
struct BarrierFixture : Fixture {
    VkEvent event{};
    explicit BarrierFixture(const char* shader) : Fixture(shader) {
        VkEventCreateInfo ci{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        Check(vkCreateEvent(device,&ci,nullptr,&event));
    }
    ~BarrierFixture(){vkDestroyEvent(device,event,nullptr);}
    void Wfi(VkCommandBuffer cb,bool fixed) {
        if(fixed) Barrier(cb,wfi_stages,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,wfi_src,wfi_dst);
        else {
            vkCmdSetEvent(cb,event,wfi_stages);
            vkCmdWaitEvents(cb,1,&event,wfi_stages,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            0,nullptr,0,nullptr,0,nullptr);
        }
    }
    void Dispatch(VkCommandBuffer cb) {
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,nullptr);
        vkCmdDispatch(cb,1,1,1);
    }
    void BufferCase(const std::string& kind,bool fixed) {
        buffers[0].mapped[0]=42;buffers[1].mapped[0]=7;buffers[2].mapped[0]=0;
        Begin();auto cb=commands[1];VkBufferCopy copy{0,0,4};
        if(kind=="query") {
            Dispatch(cb);
            // Protect the test output separately; query barriers must protect the input.
            VkBufferMemoryBarrier output{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            output.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;output.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
            output.srcQueueFamilyIndex=output.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            output.buffer=buffers[2].buffer;output.size=4;
            vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0,0,nullptr,1,&output,0,nullptr);
            if(fixed) Barrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                              query_pre_src,query_pre_dst);
        }
        vkCmdCopyBuffer(cb,buffers[0].buffer,buffers[1].buffer,1,&copy);
        if(kind=="wfi") Wfi(cb,fixed);
        else if(fixed) Barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               query_post_src,query_post_dst);
        Dispatch(cb);
        Barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
        Submit();
        if(fixed&&buffers[2].mapped[0]!=42)throw std::runtime_error("Incorrect GPU result");
        std::cout<<"RESULT "<<kind<<" fixed="<<fixed<<" expected=42 observed="<<buffers[2].mapped[0]<<'\n';
    }
    void ImageCase(bool fixed) {
        VkImage image{};VkDeviceMemory memory{};
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_R8G8B8A8_UNORM;ci.extent={4,4,1};
        ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
        ci.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        Check(vkCreateImage(device,&ci,nullptr,&image));
        VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,image,&req);
        VkPhysicalDeviceMemoryProperties mp{};vkGetPhysicalDeviceMemoryProperties(physical,&mp);
        uint32_t type=0;while(!(req.memoryTypeBits&(1U<<type)))++type;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
        Check(vkAllocateMemory(device,&ai,nullptr,&memory));Check(vkBindImageMemory(device,image,memory,0));
        Begin();auto cb=commands[1];
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;
        b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        VkClearColorValue clear{{1.0f,0.0f,0.0f,1.0f}};
        vkCmdClearColorImage(cb,image,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&b.subresourceRange);
        b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout=VK_IMAGE_LAYOUT_GENERAL;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={4,4,1};
        vkCmdCopyImageToBuffer(cb,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffers[0].buffer,1,&copy);
        b.srcAccessMask=0;b.dstAccessMask=fixed?download_dst:0;
        b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        // CopyImage restores its source without a global memory barrier. Test the layout
        // transition's own dependency, which DownloadMemory also needs for later reads.
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
        vkCmdCopyImageToBuffer(cb,image,VK_IMAGE_LAYOUT_GENERAL,buffers[2].buffer,1,&copy);
        Barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
        Submit();
        if(fixed)for(int i=0;i<16;++i)if(buffers[2].mapped[i]!=0xff0000ff)throw std::runtime_error("Incorrect downloaded pixel");
        std::cout<<"RESULT image fixed="<<fixed<<" observed="<<std::hex<<buffers[2].mapped[0]<<std::dec<<'\n';
        vkDestroyImage(device,image,nullptr);vkFreeMemory(device,memory,nullptr);
    }
    void ComputeCase(bool fixed) {
        VkDescriptorPool second_pool{};VkDescriptorSet second_set{};
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets=1;pci.poolSizeCount=1;pci.pPoolSizes=&size;
        Check(vkCreateDescriptorPool(device,&pci,nullptr,&second_pool));
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool=second_pool;ai.descriptorSetCount=1;ai.pSetLayouts=&layout;
        Check(vkAllocateDescriptorSets(device,&ai,&second_set));
        std::array<VkDescriptorBufferInfo,2> infos{{{buffers[2].buffer,0,4},{buffers[0].buffer,0,4}}};
        std::array<VkWriteDescriptorSet,2> writes{};
        for(uint32_t i=0;i<2;++i){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet=second_set;writes[i].dstBinding=i;writes[i].descriptorCount=1;
            writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&infos[i];}
        vkUpdateDescriptorSets(device,2,writes.data(),0,nullptr);
        buffers[0].mapped[0]=0;buffers[1].mapped[0]=42;buffers[2].mapped[0]=0;
        Begin();auto cb=commands[1];Dispatch(cb);
        if(fixed)Barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,compute_src,compute_dst);
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&second_set,0,nullptr);
        vkCmdDispatch(cb,1,1,1);
        Barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
        Submit();
        if(fixed&&buffers[0].mapped[0]!=42)throw std::runtime_error("Incorrect chained compute result");
        std::cout<<"RESULT compute fixed="<<fixed<<" expected=42 observed="<<buffers[0].mapped[0]<<'\n';
        vkDestroyDescriptorPool(device,second_pool,nullptr);
    }
    void Benchmark(bool fixed,bool compute=false) {
        std::vector<double> samples;
        for(int run=0;run<25;++run){
            Begin();auto cb=commands[1];vkCmdResetQueryPool(cb,queries,0,2);
            vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,0);
            for(int i=0;i<512;++i){
                if(compute){Dispatch(cb);if(fixed)Barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,compute_src,compute_dst);}
                else Wfi(cb,fixed);
            }
            vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,1);Submit();
            std::array<uint64_t,2> times{};
            Check(vkGetQueryPoolResults(device,queries,0,2,sizeof(times),times.data(),8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
            if(run>=4)samples.push_back((times[1]-times[0])*properties.limits.timestampPeriod/1000.0);
        }
        std::sort(samples.begin(),samples.end());
        std::cout<<"GPU_BENCH "<<(compute?"compute":"wfi")<<" fixed="<<fixed<<" count=512 median_us="<<samples[samples.size()/2]<<" samples=21\n";
    }
};
int main(int argc,char** argv)try{
    if(argc!=4)return 2;
    BarrierFixture f(argv[1]);std::string kind=argv[2];bool fixed=std::string(argv[3])=="fixed";
    if(kind=="bench")f.Benchmark(fixed);
    else if(kind=="compute-bench")f.Benchmark(fixed,true);
    else if(kind=="compute")f.ComputeCase(fixed);
    else if(kind=="image")f.ImageCase(fixed);
    else f.BufferCase(kind,fixed);
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
