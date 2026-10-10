// Real Vulkan fixture for the PR's upload/main ordering and sparse copy-back cost.
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include <cstring>
using u64=uint64_t;
struct HazardTracker {
    std::vector<VkBuffer> written_buffers;
    bool all_buffers_written{};
    void MarkBufferWrite(VkBuffer);
    bool IsBufferWritten(VkBuffer) const noexcept;
};
#include "tracking.inc"
#include "query-integration.inc"
#include "integration-plan.inc"
void Check(VkResult result) { if(result!=VK_SUCCESS) throw std::runtime_error("Vulkan error "+std::to_string(result)); }
namespace VideoCommon { struct BufferCopy {u64 dst_offset,size;}; }
bool ReorderUpload(bool used) {
    struct {bool disable_buffer_reorder{};} videoSettings;
    struct {bool used;bool IsRegionUsed(u64,u64) const{return used;}} buffer{used};
    std::array<VideoCommon::BufferCopy,1> copies{{{0,4}}};
#include "reorder-upload-body.inc"
}
struct HostBuffer { VkBuffer buffer{}; VkDeviceMemory memory{}; uint32_t* mapped{}; };
struct Fixture {
    VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};uint32_t family{};
    VkPhysicalDeviceProperties properties{};VkCommandPool pool{};std::array<VkCommandBuffer,2> commands{};
    VkDescriptorSetLayout layout{};VkDescriptorPool descriptor_pool{};VkDescriptorSet set{};
    VkPipelineLayout pipeline_layout{};VkPipeline pipeline{};VkShaderModule shader{};VkQueryPool queries{};
    std::vector<HostBuffer> buffers;
    static constexpr u64 bytes=2ULL<<20;
    Fixture(const char* shader_path, bool storage_array=false) {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="PR396 synthetic";app.apiVersion=VK_API_VERSION_1_3;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&app;Check(vkCreateInstance(&ici,nullptr,&instance));
        uint32_t count{};Check(vkEnumeratePhysicalDevices(instance,&count,nullptr));std::vector<VkPhysicalDevice> devices(count);Check(vkEnumeratePhysicalDevices(instance,&count,devices.data()));
        for(auto pd:devices) {
            VkPhysicalDeviceProperties prop{};vkGetPhysicalDeviceProperties(pd,&prop);
            uint32_t n{};vkGetPhysicalDeviceQueueFamilyProperties(pd,&n,nullptr);std::vector<VkQueueFamilyProperties> families(n);vkGetPhysicalDeviceQueueFamilyProperties(pd,&n,families.data());
            for(uint32_t i=0;i<n;++i) if((families[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)&&families[i].timestampValidBits) {
                if(!physical || prop.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {physical=pd;family=i;properties=prop;}break;
            }
        }
        if(!physical)throw std::runtime_error("No graphics device with timestamps");
        std::cout<<"GPU "<<properties.deviceName<<" driver="<<properties.driverVersion<<" timestampPeriod="<<properties.limits.timestampPeriod<<'\n';
        float priority=1;VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qci.queueFamilyIndex=family;qci.queueCount=1;qci.pQueuePriorities=&priority;
        VkPhysicalDeviceFeatures features{};vkGetPhysicalDeviceFeatures(physical,&features);
        VkPhysicalDeviceDescriptorIndexingFeatures indexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
        VkPhysicalDeviceRobustness2FeaturesEXT robustness{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
        if(storage_array){
            indexing.pNext=&robustness;
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};query.pNext=&indexing;
            vkGetPhysicalDeviceFeatures2(physical,&query);
            if(!indexing.shaderStorageImageArrayNonUniformIndexing)throw std::runtime_error("No storage image nonuniform indexing");
            if(!robustness.nullDescriptor)throw std::runtime_error("No null descriptors");
        }
        const char* extension=VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.pNext=storage_array?&indexing:nullptr;
        dci.enabledExtensionCount=storage_array?1:0;dci.ppEnabledExtensionNames=&extension;
        dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;dci.pEnabledFeatures=&features;Check(vkCreateDevice(physical,&dci,nullptr,&device));vkGetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pci.queueFamilyIndex=family;pci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;Check(vkCreateCommandPool(device,&pci,nullptr,&pool));
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=pool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=2;Check(vkAllocateCommandBuffers(device,&cai,commands.data()));
        for(int i=0;i<3;++i)buffers.push_back(MakeBuffer());
        std::array<VkDescriptorSetLayoutBinding,2> bindings{{{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};lci.bindingCount=2;lci.pBindings=bindings.data();Check(vkCreateDescriptorSetLayout(device,&lci,nullptr,&layout));
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};plci.setLayoutCount=1;plci.pSetLayouts=&layout;Check(vkCreatePipelineLayout(device,&plci,nullptr,&pipeline_layout));
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpci.maxSets=1;dpci.poolSizeCount=1;dpci.pPoolSizes=&ps;Check(vkCreateDescriptorPool(device,&dpci,nullptr,&descriptor_pool));
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};dsai.descriptorPool=descriptor_pool;dsai.descriptorSetCount=1;dsai.pSetLayouts=&layout;Check(vkAllocateDescriptorSets(device,&dsai,&set));
        std::array<VkDescriptorBufferInfo,2> infos{{{buffers[1].buffer,0,4},{buffers[2].buffer,0,4}}};std::array<VkWriteDescriptorSet,2> writes{};
        for(uint32_t i=0;i<2;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&infos[i];}vkUpdateDescriptorSets(device,2,writes.data(),0,nullptr);
        std::ifstream stream(shader_path,std::ios::binary|std::ios::ate);if(!stream)throw std::runtime_error("Missing shader");auto size=stream.tellg();std::vector<uint32_t> code(static_cast<size_t>(size)/4);stream.seekg(0);stream.read(reinterpret_cast<char*>(code.data()),size);
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};smci.codeSize=code.size()*4;smci.pCode=code.data();Check(vkCreateShaderModule(device,&smci,nullptr,&shader));
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cpci.layout=pipeline_layout;cpci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};cpci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;cpci.stage.module=shader;cpci.stage.pName="main";Check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cpci,nullptr,&pipeline));
        VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qpci.queryType=VK_QUERY_TYPE_TIMESTAMP;qpci.queryCount=2;Check(vkCreateQueryPool(device,&qpci,nullptr,&queries));
    }
    HostBuffer MakeBuffer() {
        HostBuffer b;VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=bytes;ci.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;Check(vkCreateBuffer(device,&ci,nullptr,&b.buffer));
        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,b.buffer,&req);VkPhysicalDeviceMemoryProperties mp{};vkGetPhysicalDeviceMemoryProperties(physical,&mp);uint32_t type=~0U;
        for(uint32_t i=0;i<mp.memoryTypeCount;++i) if((req.memoryTypeBits&(1U<<i))&&(mp.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){type=i;break;}
        if(type==~0U)throw std::runtime_error("No coherent host memory");VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;Check(vkAllocateMemory(device,&ai,nullptr,&b.memory));Check(vkBindBufferMemory(device,b.buffer,b.memory,0));Check(vkMapMemory(device,b.memory,0,bytes,0,reinterpret_cast<void**>(&b.mapped)));return b;
    }
    void Begin() { Check(vkResetCommandPool(device,pool,0));for(auto cmd:commands){VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;Check(vkBeginCommandBuffer(cmd,&bi));} }
    static void Barrier(VkCommandBuffer cmd,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkAccessFlags src_access,VkAccessFlags dst_access) {
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=src_access;b.dstAccessMask=dst_access;vkCmdPipelineBarrier(cmd,src,dst,0,1,&b,0,nullptr,0,nullptr);
    }
    void Submit() {
        for(auto cmd:commands)Check(vkEndCommandBuffer(cmd));VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=2;si.pCommandBuffers=commands.data();Check(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE));Check(vkQueueWaitIdle(queue));
    }
    uint32_t ReorderCase(bool enable_reorder,bool mark_producer) {
        buffers[0].mapped[0]=42;buffers[1].mapped[0]=7;buffers[2].mapped[0]=0xffffffff;
        Begin();auto upload=commands[0],main=commands[1];
        HazardTracker hazards;if(mark_producer)hazards.MarkBufferWrite(buffers[1].buffer);
        bool reorder=enable_reorder&&!hazards.IsBufferWritten(buffers[1].buffer);
        VkBufferCopy copy{0,0,4};vkCmdCopyBuffer(main,buffers[0].buffer,buffers[1].buffer,1,&copy);
        Barrier(main,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
        auto consumer=reorder?upload:main;vkCmdBindPipeline(consumer,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(consumer,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,nullptr);vkCmdDispatch(consumer,1,1,1);
        Barrier(consumer,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
        Submit();return buffers[2].mapped[0];
    }
    void CopyBenchmark(uint32_t duplicates) {
        std::fill_n(buffers[0].mapped,bytes/4,42);std::vector<double> samples;
        for(int run=0;run<25;++run) {
            Begin();auto cmd=commands[1];vkCmdResetQueryPool(cmd,queries,0,2);vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,0);
            for(uint32_t i=0;i<duplicates;++i) {
                Barrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
                VkBufferCopy copy{0,0,bytes};vkCmdCopyBuffer(cmd,buffers[0].buffer,buffers[1].buffer,1,&copy);
                Barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
            }
            vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,1);Submit();std::array<uint64_t,2> result{};
            Check(vkGetQueryPoolResults(device,queries,0,2,sizeof(result),result.data(),8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
            if(run>=4)samples.push_back((result[1]-result[0])*properties.limits.timestampPeriod/1000.0);
        }
        std::sort(samples.begin(),samples.end());std::cout<<"GPU_BENCH sparse_copyback duplicates="<<duplicates<<" sizeMiB=2 median_us="<<samples[samples.size()/2]<<" min_us="<<samples.front()<<" max_us="<<samples.back()<<" samples="<<samples.size()<<'\n';
    }
    uint32_t CopybackThenCpuWrite(bool mark_usage) {
        buffers[0].mapped[0]=42;buffers[1].mapped[0]=99;buffers[2].mapped[0]=7;
        Begin();auto upload=commands[0],main=commands[1];VkBufferCopy copy{0,0,4};
        vkCmdCopyBuffer(main,buffers[0].buffer,buffers[2].buffer,1,&copy);
        Barrier(main,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
        auto cpu_upload=ReorderUpload(mark_usage)?upload:main;
        vkCmdCopyBuffer(cpu_upload,buffers[1].buffer,buffers[2].buffer,1,&copy);
        Barrier(main,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
        Submit();return buffers[2].mapped[0];
    }
    ~Fixture() {
        if(device){vkDeviceWaitIdle(device);for(auto b:buffers){vkUnmapMemory(device,b.memory);vkDestroyBuffer(device,b.buffer,nullptr);vkFreeMemory(device,b.memory,nullptr);}vkDestroyQueryPool(device,queries,nullptr);vkDestroyPipeline(device,pipeline,nullptr);vkDestroyShaderModule(device,shader,nullptr);vkDestroyPipelineLayout(device,pipeline_layout,nullptr);vkDestroyDescriptorPool(device,descriptor_pool,nullptr);vkDestroyDescriptorSetLayout(device,layout,nullptr);vkDestroyCommandPool(device,pool,nullptr);vkDestroyDevice(device,nullptr);}if(instance)vkDestroyInstance(instance,nullptr);
    }
};
int main(int argc,char** argv) {
    try {
        Fixture f(argc>1?argv[1]:"read.spv");int failures=0;
        for(auto [name,reorder,mark,expected]:std::array<std::tuple<const char*,bool,bool,uint32_t>,3>{{{"master_main_order",false,false,42},{"original_pr_query_regression_reproduced",true,false,7},{"corrected_query_producer",true,FIXED_QUERY_MARK,42}}}) {
            auto observed=f.ReorderCase(reorder,mark);bool ok=observed==expected;failures+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<name<<" expected="<<expected<<" observed="<<observed<<'\n';
        }
        for(bool mark:{false,true}) {
            auto observed=f.CopybackThenCpuWrite(mark?FIXED_SPARSE_USAGE:false);uint32_t expected=mark?99:42;bool ok=observed==expected;failures+=!ok;
            std::cout<<(ok?"PASS ":"FAIL ")<<(mark?"corrected_copyback_destination_usage":"original_pr_copyback_regression_reproduced")<<" expected="<<expected<<" observed="<<observed<<'\n';
        }
        f.CopyBenchmark(FIXED_SPARSE_COPIES);f.CopyBenchmark(16);return failures?1:0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
}
