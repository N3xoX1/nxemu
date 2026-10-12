// Real production shader + extracted production Copy() bindings and barriers.
#define main PreviousVulkanMain
#include "../pr396/vulkan-synthetic.cpp"
#undef main
#include "yuzu_common/readback_reference.h"
#include "yuzu_common/alignment.h"
#include "yuzu_common/div_ceil.h"
#include "yuzu_common/differential_memory_copy.h"
#include "yuzu_common/parallel_readback_copy.h"
#include <chrono>
#include <span>

struct Allocation {
    Fixture& f;VkBuffer handle{};VkDeviceMemory memory{};unsigned char* mapped{};size_t size;
    Allocation(Fixture& fixture,size_t size_,bool host):f(fixture),size(size_) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=size;bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;Check(vkCreateBuffer(f.device,&bi,nullptr,&handle));
        VkMemoryRequirements req;vkGetBufferMemoryRequirements(f.device,handle,&req);VkPhysicalDeviceMemoryProperties mp;vkGetPhysicalDeviceMemoryProperties(f.physical,&mp);
        const VkMemoryPropertyFlags required=host?VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT:VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        unsigned type=~0u;int score=-1;
        for(unsigned i=0;i<mp.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i))&&(mp.memoryTypes[i].propertyFlags&required)==required){int n=host?((mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)?2:0):((mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)?0:2);if(n>score){score=n;type=i;}}
        if(type==~0u)throw std::runtime_error("missing delta allocation type");VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;Check(vkAllocateMemory(f.device,&ai,nullptr,&memory));Check(vkBindBufferMemory(f.device,handle,memory,0));if(host)Check(vkMapMemory(f.device,memory,0,size,0,reinterpret_cast<void**>(&mapped)));
    }
    ~Allocation(){if(mapped)vkUnmapMemory(f.device,memory);vkDestroyBuffer(f.device,handle,nullptr);vkFreeMemory(f.device,memory,nullptr);}
};
template<class T>struct Handle{T value{};T operator*()const{return value;}};
namespace vk {
struct CommandBuffer {
    VkCommandBuffer cb;
    void PipelineBarrier(VkPipelineStageFlags a,VkPipelineStageFlags b,VkDependencyFlags d,const VkMemoryBarrier& barrier){vkCmdPipelineBarrier(cb,a,b,d,1,&barrier,0,nullptr,0,nullptr);}
    void BindPipeline(VkPipelineBindPoint p,VkPipeline v){vkCmdBindPipeline(cb,p,v);}
    void BindDescriptorSets(VkPipelineBindPoint p,VkPipelineLayout l,uint32_t first,VkDescriptorSet s,std::span<const uint32_t>){vkCmdBindDescriptorSets(cb,p,l,first,1,&s,0,nullptr);}
    void PushConstants(VkPipelineLayout l,VkShaderStageFlags s,uint32_t o,uint32_t n,const void* d){vkCmdPushConstants(cb,l,s,o,n,d);}
    void Dispatch(uint32_t a,uint32_t b,uint32_t c){vkCmdDispatch(cb,a,b,c);}
};
}
struct NativeQueue {
    std::array<VkDescriptorBufferInfo,4> buffers;size_t count{};
    void Acquire(size_t n){if(n!=4)throw std::runtime_error("wrong native descriptor count");count=0;}
    void AddBuffer(VkBuffer b,u64 offset,u64 size){buffers.at(count++)={b,offset,size};}
    const void* UpdateData(){return buffers.data();}
};
struct NativeDevice {
    Fixture& f;
    u64 GetStorageBufferAlignment()const{return f.properties.limits.minStorageBufferOffsetAlignment;}
    const NativeDevice& GetLogical()const{return *this;}
    void UpdateDescriptorSet(VkDescriptorSet set,int,const void* data)const{
        auto* infos=static_cast<const VkDescriptorBufferInfo*>(data);std::array<VkWriteDescriptorSet,4> writes{};
        for(unsigned i=0;i<4;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&infos[i];}vkUpdateDescriptorSets(f.device,4,writes.data(),0,nullptr);
    }
};
struct NativeScheduler {
    Fixture& f;void RequestOutsideRenderPassOperationContext(){}
    template<class F>void Record(F&& function){function(vk::CommandBuffer{f.commands[1]});}
};
struct NativeAllocator {
    Fixture& f;VkDescriptorPool pool{};VkDescriptorSetLayout layout{};
    VkDescriptorSet Commit(){VkDescriptorSet set;VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&layout;Check(vkAllocateDescriptorSets(f.device,&ai,&set));return set;}
};
struct NativeDelta {
    Fixture& f;NativeDevice device;NativeScheduler scheduler;NativeQueue queue;
    NativeAllocator descriptor_allocator;Handle<int> descriptor_template;
    Handle<VkPipelineLayout> layout;Handle<VkPipeline> pipeline;VkShaderModule shader{};
    NativeDelta(Fixture& fixture,const char* path):f(fixture),device{f},scheduler{f},descriptor_allocator{f}{
        std::array<VkDescriptorSetLayoutBinding,4> bindings{};for(unsigned i=0;i<4;++i)bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=4;li.pBindings=bindings.data();Check(vkCreateDescriptorSetLayout(f.device,&li,nullptr,&descriptor_allocator.layout));
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,8192};VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=2048;pi.poolSizeCount=1;pi.pPoolSizes=&size;Check(vkCreateDescriptorPool(f.device,&pi,nullptr,&descriptor_allocator.pool));
        VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT,0,32};VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pli.setLayoutCount=1;pli.pSetLayouts=&descriptor_allocator.layout;pli.pushConstantRangeCount=1;pli.pPushConstantRanges=&pc;Check(vkCreatePipelineLayout(f.device,&pli,nullptr,&layout.value));
        std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("missing production shader");const auto n=static_cast<size_t>(file.tellg());std::vector<uint32_t> code(n/4);file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),n);
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=n;sm.pCode=code.data();Check(vkCreateShaderModule(f.device,&sm,nullptr,&shader));VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=*layout;ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=shader;ci.stage.pName="main";Check(vkCreateComputePipelines(f.device,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline.value));
    }
    void Copy(VkBuffer,VkBuffer,VkBuffer,VkBuffer,u64,u64,u64,u64,u64,const std::array<uint32_t,4>&);
    ~NativeDelta(){vkDestroyPipeline(f.device,*pipeline,nullptr);vkDestroyShaderModule(f.device,shader,nullptr);vkDestroyPipelineLayout(f.device,*layout,nullptr);vkDestroyDescriptorPool(f.device,descriptor_allocator.pool,nullptr);vkDestroyDescriptorSetLayout(f.device,descriptor_allocator.layout,nullptr);}
};
using u32=uint32_t;
#include "delta-pass.inc"

void Verify(Fixture& f,NativeDelta& pass,size_t bytes,size_t source_offset,size_t data_offset) {
    Allocation source(f,bytes+4096,false),reference(f,bytes+4096,false),data(f,bytes*2+4096,true),flags(f,4096,true);
    Common::ReadbackReference proof;
    const uint32_t value=0x7FC01234,changed_value=0x7FC01235;
    const size_t chunks=(bytes+65535)/65536;
    f.Begin();auto cb=f.commands[1];vkCmdFillBuffer(cb,source.handle,0,source.size,value);vkCmdFillBuffer(cb,reference.handle,0,reference.size,0x87654321);
    pass.Copy(source.handle,reference.handle,data.handle,flags.handle,source_offset,data_offset,bytes,0,flags.size,proof.ValidMask(source_offset,bytes));proof.Scheduled(source_offset,bytes);
    const size_t change=std::min<size_t>(65536+12,bytes-4);
    vkCmdFillBuffer(cb,source.handle,source_offset+change,4,changed_value);
    const size_t second=data_offset+bytes;
    pass.Copy(source.handle,reference.handle,data.handle,flags.handle,source_offset,second,bytes,1024,flags.size,proof.ValidMask(source_offset,bytes));proof.Scheduled(source_offset,bytes);
    f.Submit();
    std::vector<uint32_t> guest(bytes/4,0xAA55AA55);
    Common::ReadbackCpuReference cpu_reference(bytes);
    const auto retire=[&](size_t offset,size_t flag_offset,bool cancel){
        const auto* bits=reinterpret_cast<const uint32_t*>(flags.mapped+flag_offset);
        for(size_t block=0;block<chunks;++block)if(bits[block]) {
            const size_t first=block*65536,last=std::min(bytes,first+65536);
            std::memcpy(cpu_reference.bytes.get()+first,data.mapped+offset+first,last-first);
        }
        auto* dst=reinterpret_cast<unsigned char*>(guest.data());
        const size_t a=65536-4,b=65536+8;
        if(!cancel)Common::CopyChangedMemoryForReadback(dst,cpu_reference.bytes.get(),bytes);
        else {Common::CopyChangedMemoryForReadback(dst,cpu_reference.bytes.get(),a);Common::CopyChangedMemoryForReadback(dst+b,cpu_reference.bytes.get()+b,bytes-b);}
    };
    auto* bits=reinterpret_cast<const uint32_t*>(flags.mapped);
    for(size_t i=0;i<chunks;++i)if(bits[i]!=1)throw std::runtime_error("unknown reference did not force every chunk");
    retire(data_offset,0,false);for(auto word:guest)if(word!=value)throw std::runtime_error("first epoch payload mismatch");
    guest[25]=0xF0F0F0F0; // Another cache changed RAM between queued retirements.
    bits=reinterpret_cast<const uint32_t*>(flags.mapped+1024);
    for(size_t i=0;i<chunks;++i)if(bits[i]!=(i==change/65536?1u:0u))throw std::runtime_error("second epoch mask mismatch");
    retire(second,1024,false);for(size_t i=0;i<guest.size();++i)if(guest[i]!=(i==change/4?changed_value:value))throw std::runtime_error("second epoch payload mismatch");
    // Late CPU changes cancel an interval, invalidate the proof, then a new GPU
    // write must force the affected chunks even if most values are unchanged.
    const size_t cancel=65536-4;
    std::memset(reinterpret_cast<unsigned char*>(guest.data())+cancel,0xA5,12);proof.Invalidate(source_offset+cancel,12);
    f.Begin();cb=f.commands[1];
    vkCmdFillBuffer(cb,source.handle,source_offset+cancel,12,0xA5A5A5A5);
    Fixture::Barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cb,source.handle,source_offset+cancel,4,0xDEADC0DE);
    pass.Copy(source.handle,reference.handle,data.handle,flags.handle,source_offset,data_offset,bytes,0,flags.size,proof.ValidMask(source_offset,bytes));proof.Scheduled(source_offset,bytes);
    f.Submit();bits=reinterpret_cast<const uint32_t*>(flags.mapped);
    if(bits[0]!=1||bits[1]!=1)throw std::runtime_error("CPU invalidation did not force both chunks");
    retire(data_offset,0,true);
    if(guest[cancel/4]!=0xA5A5A5A5||guest[cancel/4+1]!=0xA5A5A5A5||guest[cancel/4+2]!=0xA5A5A5A5)throw std::runtime_error("cancelled CPU bytes overwritten");
    proof.Invalidate(source_offset+cancel,12);
    f.Begin();pass.Copy(source.handle,reference.handle,data.handle,flags.handle,source_offset,data_offset,bytes,0,flags.size,proof.ValidMask(source_offset,bytes));proof.Scheduled(source_offset,bytes);f.Submit();retire(data_offset,0,false);
    if(guest[cancel/4]!=0xDEADC0DE)throw std::runtime_error("next epoch GPU write not visible");
    std::cout<<"PASS production_GPU_delta_FIFO_NaN_bits_CPU_cancel_untracked_RAM_offsets bytes="<<bytes<<" source="<<source_offset<<" destination="<<data_offset<<'\n';
}
void Benchmark(Fixture& f,NativeDelta& pass,size_t bytes,unsigned percent,bool delta) {
    Allocation source(f,bytes,false),reference(f,bytes,false),data(f,bytes,true),flags(f,4096,true);
    std::vector<unsigned char> guest(bytes);
    Common::ReadbackCpuReference cpu_reference(bytes);
    Common::ReadbackReference proof;
    f.Begin();auto cb=f.commands[1];vkCmdFillBuffer(cb,source.handle,0,bytes,0);
    pass.Copy(source.handle,reference.handle,data.handle,flags.handle,0,0,bytes,0,flags.size,proof.ValidMask(0,bytes));
    proof.Scheduled(0,bytes);f.Submit();std::memcpy(cpu_reference.bytes.get(),data.mapped,bytes);
    std::vector<double> samples;
    const size_t chunks=bytes/65536;
    for(unsigned iteration=0;iteration<24;++iteration) {
        const auto start=std::chrono::steady_clock::now();f.Begin();cb=f.commands[1];
        const uint32_t value=iteration+1;
        for(size_t chunk=0;chunk<chunks;++chunk)if((percent==100)||(percent&&chunk%10==0))
            vkCmdFillBuffer(cb,source.handle,chunk*65536,65536,value);
        if(delta)pass.Copy(source.handle,reference.handle,data.handle,flags.handle,0,0,bytes,0,flags.size,proof.ValidMask(0,bytes));
        else {
            Fixture::Barrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
            const VkBufferCopy copy{0,0,bytes};vkCmdCopyBuffer(cb,source.handle,data.handle,1,&copy);
            Fixture::Barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_READ_BIT);
        }
        f.Submit();
        if(delta) {
            const auto* bits=reinterpret_cast<const uint32_t*>(flags.mapped);
            for(size_t chunk=0;chunk<chunks;++chunk)if(bits[chunk])std::memcpy(cpu_reference.bytes.get()+chunk*65536,data.mapped+chunk*65536,65536);
            Common::CopyChangedMemoryForReadback(guest.data(),cpu_reference.bytes.get(),bytes);
        } else Common::CopyChangedMemory(guest.data(),data.mapped,bytes);
        const auto end=std::chrono::steady_clock::now();
        if(iteration>=4)samples.push_back(std::chrono::duration<double,std::milli>(end-start).count());
        // Independent oracle outside the timed interval, including unchanged blocks.
        for(size_t chunk=0;chunk<chunks;++chunk) {
            const uint32_t expected=((percent==100)||(percent&&chunk%10==0))?value:0;
            for(size_t word=chunk*16384;word<(chunk+1)*16384;++word) {
                uint32_t actual;std::memcpy(&actual,guest.data()+word*4,4);
                if(actual!=expected)throw std::runtime_error("benchmark payload mismatch");
            }
        }
    }
    std::sort(samples.begin(),samples.end());
    std::cout<<"BENCH bytes="<<bytes<<" changed_percent="<<percent<<" mode="<<(delta?"production_gpu_delta":"v8_transfer_cpu_SIMD")
             <<" median_ms="<<(samples[9]+samples[10])/2<<" samples=20 PASS_payload\n";
}
int main(int argc,char** argv)try{
    Common::ReadbackCopyContext readback_context;
    if(argc!=3&&argc!=4)return 2;Fixture f(argv[1]);NativeDelta pass(f,argv[2]);
    if(argc==4) {
        for(size_t size:{2097152ULL,8388608ULL})for(unsigned percent:{0u,10u,100u})for(bool delta:{false,true})Benchmark(f,pass,size,percent,delta);
        return 0;
    }
    for(size_t size:{1048588ULL,2097152ULL,8388608ULL})for(size_t source:{0ULL,4ULL,252ULL})Verify(f,pass,size,source,260);
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
