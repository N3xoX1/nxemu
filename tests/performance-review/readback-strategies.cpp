// Strategy experiment: real VRAM -> host readback, exact CPU or GPU comparison.
// It does not replace the emulator's cancellation, alias or fence protocol.
#define main PreviousGpuMain
#include "../pr396/vulkan-synthetic.cpp"
#undef main
#include "yuzu_common/differential_memory_copy.h"
#include "v7-reference.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

struct Buffer {
    Fixture& f; VkBuffer handle{}; VkDeviceMemory memory{}; unsigned char* mapped{};
    Buffer(Fixture& fixture,u64 size,bool host):f(fixture) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=size;
        bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        Check(vkCreateBuffer(f.device,&bi,nullptr,&handle));
        VkMemoryRequirements req;vkGetBufferMemoryRequirements(f.device,handle,&req);
        VkPhysicalDeviceMemoryProperties props;vkGetPhysicalDeviceMemoryProperties(f.physical,&props);
        const auto required=host?VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT:VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        uint32_t type=~0U;int score=-1;
        for(uint32_t i=0;i<props.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i))&&(props.memoryTypes[i].propertyFlags&required)==required) {
            const int candidate=host?((props.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)?2:0):((props.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)?0:2);
            if(candidate>score){score=candidate;type=i;}
        }
        if(type==~0U)throw std::runtime_error("no strategy memory type");
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
        Check(vkAllocateMemory(f.device,&ai,nullptr,&memory));Check(vkBindBufferMemory(f.device,handle,memory,0));
        if(host)Check(vkMapMemory(f.device,memory,0,size,0,reinterpret_cast<void**>(&mapped)));
    }
    ~Buffer(){if(mapped)vkUnmapMemory(f.device,memory);vkDestroyBuffer(f.device,handle,nullptr);vkFreeMemory(f.device,memory,nullptr);}
};

struct Delta {
    Fixture& f;VkDescriptorSetLayout layout{};VkDescriptorPool pool{};VkDescriptorSet set{};
    VkPipelineLayout pipeline_layout{};VkPipeline pipeline{};VkShaderModule shader{};
    Delta(Fixture& fixture,const char* path,Buffer& source,Buffer& reference,Buffer& mask,u64 bytes):f(fixture) {
        std::array<VkDescriptorSetLayoutBinding,3> bindings{};
        for(uint32_t i=0;i<3;++i)bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=3;li.pBindings=bindings.data();Check(vkCreateDescriptorSetLayout(f.device,&li,nullptr,&layout));
        VkDescriptorPoolSize count{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3};VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=1;pi.pPoolSizes=&count;Check(vkCreateDescriptorPool(f.device,&pi,nullptr,&pool));
        VkDescriptorSetAllocateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};si.descriptorPool=pool;si.descriptorSetCount=1;si.pSetLayouts=&layout;Check(vkAllocateDescriptorSets(f.device,&si,&set));
        std::array<VkDescriptorBufferInfo,3> infos{{{source.handle,0,bytes},{reference.handle,0,bytes},{mask.handle,0,(bytes+65535)/65536*4}}};
        std::array<VkWriteDescriptorSet,3> writes{};
        for(uint32_t i=0;i<3;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&infos[i];}
        vkUpdateDescriptorSets(f.device,3,writes.data(),0,nullptr);
        VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT,0,4};VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pli.setLayoutCount=1;pli.pSetLayouts=&layout;pli.pushConstantRangeCount=1;pli.pPushConstantRanges=&pc;Check(vkCreatePipelineLayout(f.device,&pli,nullptr,&pipeline_layout));
        std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("missing delta shader");
        const auto length=static_cast<size_t>(file.tellg());std::vector<uint32_t> code(length/4);file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),length);
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=length;sm.pCode=code.data();Check(vkCreateShaderModule(f.device,&sm,nullptr,&shader));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=pipeline_layout;ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=shader;ci.stage.pName="main";Check(vkCreateComputePipelines(f.device,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline));
    }
    void Dispatch(VkCommandBuffer cb,uint32_t words) {
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,nullptr);
        vkCmdPushConstants(cb,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&words);
        vkCmdDispatch(cb,(words+16383)/16384,1,1);
    }
    ~Delta(){vkDestroyPipeline(f.device,pipeline,nullptr);vkDestroyShaderModule(f.device,shader,nullptr);vkDestroyPipelineLayout(f.device,pipeline_layout,nullptr);vkDestroyDescriptorPool(f.device,pool,nullptr);vkDestroyDescriptorSetLayout(f.device,layout,nullptr);}
};

class Worker {
    std::mutex mutex;std::condition_variable wake,done;std::thread thread;
    bool stop{},pending{},finished{};void* dst{};const void* src{};size_t size{},result{};
public:
    Worker():thread([this]{std::unique_lock lock(mutex);for(;;){wake.wait(lock,[this]{return stop||pending;});if(stop)return;
        auto* d=dst;const auto* s=src;auto n=size;pending=false;lock.unlock();const auto copied=Common::CopyChangedMemory(d,s,n);lock.lock();result=copied;finished=true;done.notify_one();}}){}
    size_t Copy(void* d,const void* s,size_t n){
        const size_t half=n/2;
        {std::scoped_lock lock(mutex);dst=static_cast<unsigned char*>(d)+half;src=static_cast<const unsigned char*>(s)+half;size=n-half;finished=false;pending=true;}
        wake.notify_one();const auto copied=Common::CopyChangedMemory(d,s,half);
        std::unique_lock lock(mutex);done.wait(lock,[this]{return finished;});return copied+result;
    }
    ~Worker(){{std::scoped_lock lock(mutex);stop=true;}wake.notify_one();thread.join();}
};

void Run(Fixture& f,const char* shader,size_t bytes,unsigned pattern,unsigned cpu_updates,unsigned rounds) {
    Buffer source(f,bytes,false),reference(f,bytes,false),staging(f,bytes,true),mask(f,(bytes+65535)/65536*4,true);
    Delta delta(f,shader,source,reference,mask,bytes);Worker worker;
    std::vector<uint32_t> guest(bytes/4),expected(bytes/4);
    constexpr uint32_t base=0x12345678,cpu_value=0x11223344,gpu_value=0xF0E1D2C3;
    const size_t chunks=(bytes+65535)/65536;
    const size_t upload_bytes=cpu_updates?((bytes/10+3)/4)*4:0;
    std::array<std::vector<double>,5> timings;
    std::array<size_t,5> downloaded{},stored{};
    for(unsigned round=0;round<rounds;++round)for(unsigned order=0;order<5;++order) {
        const unsigned mode=(round+order)%5;
        std::fill(guest.begin(),guest.end(),base);expected=guest;
        if(upload_bytes){std::fill_n(guest.begin(),upload_bytes/4,cpu_value);std::fill_n(expected.begin(),upload_bytes/4,cpu_value);}
        f.Begin();auto cb=f.commands[1];vkCmdFillBuffer(cb,source.handle,0,bytes,base);vkCmdFillBuffer(cb,reference.handle,0,bytes,base);
        Fixture::Barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);f.Submit();
        for(size_t block=0;block<chunks;++block)if(pattern==2||(pattern==1&&block%10==0)) {
            const auto first=block*16384,last=std::min(first+16384,expected.size());std::fill(expected.begin()+first,expected.begin()+last,gpu_value);
        }
        const auto start=std::chrono::steady_clock::now();
        if(upload_bytes)std::memcpy(staging.mapped,guest.data(),upload_bytes);
        f.Begin();cb=f.commands[1];
        Fixture::Barrier(cb,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
        if(upload_bytes){VkBufferCopy copy{0,0,upload_bytes};vkCmdCopyBuffer(cb,staging.handle,source.handle,1,&copy);if(mode==4)vkCmdCopyBuffer(cb,staging.handle,reference.handle,1,&copy);}
        Fixture::Barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
        for(size_t block=0;block<chunks;++block)if(pattern==2||(pattern==1&&block%10==0))vkCmdFillBuffer(cb,source.handle,block*65536,std::min<size_t>(65536,bytes-block*65536),gpu_value);
        size_t copy_bytes{},store_bytes{};
        const size_t cancelled_first=65536-4,cancelled_last=65536+8;
        const bool cancel=round==0;
        const auto cpu_write_after_download=[&]{
            if(cancel){std::memset(reinterpret_cast<unsigned char*>(guest.data())+cancelled_first,0xA5,cancelled_last-cancelled_first);
                       std::memset(reinterpret_cast<unsigned char*>(expected.data())+cancelled_first,0xA5,cancelled_last-cancelled_first);}
        };
        const auto retire=[&](size_t offset,size_t count){
            const auto copy_part=[&](size_t first,size_t n){
                auto* d=reinterpret_cast<unsigned char*>(guest.data())+first;const auto* s=staging.mapped+first;
                if(mode==0||mode==4){std::memcpy(d,s,n);return n;}
                if(mode==1)return ReferenceV7::CopyChangedMemory(d,s,n);
                if(mode==2)return Common::CopyChangedMemory(d,s,n);
                return worker.Copy(d,s,n);
            };
            const size_t end=offset+count;
            if(!cancel||end<=cancelled_first||offset>=cancelled_last)return copy_part(offset,count);
            size_t copied=0;
            if(offset<cancelled_first)copied+=copy_part(offset,cancelled_first-offset);
            if(end>cancelled_last)copied+=copy_part(cancelled_last,end-cancelled_last);
            return copied;
        };
        if(mode<4) {
            Fixture::Barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
            VkBufferCopy copy{0,0,bytes};vkCmdCopyBuffer(cb,source.handle,staging.handle,1,&copy);
            Fixture::Barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);f.Submit();
            copy_bytes=bytes;
            cpu_write_after_download();
            store_bytes=retire(0,bytes);
        } else {
            Fixture::Barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
            delta.Dispatch(cb,static_cast<uint32_t>(bytes/4));
            Fixture::Barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);f.Submit();
            cpu_write_after_download();
            std::vector<VkBufferCopy> ranges;
            auto* flags=reinterpret_cast<const uint32_t*>(mask.mapped);
            copy_bytes=chunks*4;
            for(size_t block=0;block<chunks;++block)if(flags[block]) {
                const size_t n=std::min<size_t>(65536,bytes-block*65536);
                if(!ranges.empty()&&ranges.back().srcOffset+ranges.back().size==block*65536)ranges.back().size+=n;
                else ranges.push_back({block*65536,block*65536,n});
                copy_bytes+=n;
            }
            if(!ranges.empty()) {
                f.Begin();cb=f.commands[1];
                Fixture::Barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
                vkCmdCopyBuffer(cb,source.handle,staging.handle,static_cast<uint32_t>(ranges.size()),ranges.data());
                vkCmdCopyBuffer(cb,source.handle,reference.handle,static_cast<uint32_t>(ranges.size()),ranges.data());
                Fixture::Barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);f.Submit();
                for(const auto& range:ranges)store_bytes+=retire(range.dstOffset,range.size);
            }
        }
        const auto end=std::chrono::steady_clock::now();
        if(guest!=expected)throw std::runtime_error("strategy final RAM mismatch");
        // After cancellation the GPU reference is stale. Refresh that CPU-written
        // range before a new GPU write, and verify that the next exact comparison
        // detects it. A production implementation must enforce this lifecycle.
        if(cancel&&mode==4) {
            std::memcpy(staging.mapped+cancelled_first,reinterpret_cast<unsigned char*>(guest.data())+cancelled_first,cancelled_last-cancelled_first);
            f.Begin();cb=f.commands[1];
            Fixture::Barrier(cb,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
            VkBufferCopy refresh{cancelled_first,cancelled_first,cancelled_last-cancelled_first};
            vkCmdCopyBuffer(cb,staging.handle,source.handle,1,&refresh);vkCmdCopyBuffer(cb,staging.handle,reference.handle,1,&refresh);
            Fixture::Barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdFillBuffer(cb,source.handle,cancelled_first,4,0xDEADC0DE);
            Fixture::Barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
            delta.Dispatch(cb,static_cast<uint32_t>(bytes/4));
            Fixture::Barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);f.Submit();
            if(!reinterpret_cast<uint32_t*>(mask.mapped)[cancelled_first/65536])throw std::runtime_error("refreshed CPU range failed to detect next GPU write");
        }
        if(round>=3||rounds==1)timings[mode].push_back(std::chrono::duration<double,std::milli>(end-start).count());
        downloaded[mode]=copy_bytes;stored[mode]=store_bytes;
    }
    std::cout<<"PASS exact_final_RAM_and_cancelled_CPU_bytes sizeMiB="<<(bytes>>20)<<" pattern="<<pattern<<" cpu_updates="<<cpu_updates<<'\n';
    constexpr const char* names[]{"full_memcpy","cpu_sse2","cpu_avx2_dispatch","cpu_two_threads","gpu_exact_delta"};
    for(unsigned mode=0;mode<5;++mode){auto& t=timings[mode];std::sort(t.begin(),t.end());std::cout<<"STRATEGY sizeMiB="<<(bytes>>20)<<" pattern="<<pattern<<" cpu_updates="<<cpu_updates<<" mode="<<names[mode]<<" median_ms="<<t[t.size()/2]<<" downloaded_bytes="<<downloaded[mode]<<" stored_bytes="<<stored[mode]<<" samples="<<t.size()<<'\n';}
}

int main(int argc,char** argv)try{
    if(argc!=4)return 2;Fixture f(argv[1]);const bool check=std::string(argv[3])=="check";
    for(size_t bytes:{2ULL<<20,64ULL<<20})for(unsigned pattern:{0U,1U,2U})for(unsigned updates:{0U,1U})Run(f,argv[2],bytes,pattern,updates,check?1:10);
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
