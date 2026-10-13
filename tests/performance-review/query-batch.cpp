// Exercise the actual deferred query-copy code and scheduler chunk, with real Vulkan validation.
#define main PriorTestMain
#include "../pr396/vulkan-synthetic.cpp"
#undef main
#include <chrono>
#include <cstddef>
#include <memory>
#include <ranges>
using u8 = uint8_t;
namespace Common {
template<class T> constexpr T AlignUp(T value, T alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}
}
struct Trace {
    uint64_t checksum{};
    std::vector<uint64_t>* events{};
    void Add(uint64_t value) { checksum += value; if(events)events->push_back(value); }
};
namespace vk {
struct CommandBuffer {
    VkCommandBuffer handle{};
    Trace* trace{};
    void PipelineBarrier(VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags flags,
                         const VkMemoryBarrier& barrier) const {
        if(trace)trace->Add(0x100000000ULL + src + dst + barrier.srcAccessMask + barrier.dstAccessMask);
        if(handle)vkCmdPipelineBarrier(handle,src,dst,flags,1,&barrier,0,nullptr,0,nullptr);
    }
    void CopyBuffer(VkBuffer src, VkBuffer dst, const std::vector<VkBufferCopy>& copies) const {
        if(trace) { trace->Add(0x200000000ULL);for(const auto& c:copies)trace->Add(c.srcOffset+c.dstOffset+c.size); }
        if(handle)vkCmdCopyBuffer(handle,src,dst,static_cast<uint32_t>(copies.size()),copies.data());
    }
};
}
struct ChunkHarness {
#include "command-chunk.inc"
    CommandChunk chunk;
    vk::CommandBuffer buffer;
    size_t records{}, contexts{}, submissions{}, bytes{};
    std::vector<VkBuffer> written;
    void RequestOutsideRenderPassOperationContext() { ++contexts; }
    void MarkBufferWrite(VkBuffer value) { written.push_back(value); }
    template<class T> void Record(T&& c) {
        auto command=[c=std::move(c)](vk::CommandBuffer main,vk::CommandBuffer){c(main);};
        if(!chunk.Record(command)){Execute();if(!chunk.Record(command))throw std::runtime_error("Chunk overflow");}
        ++records;
    }
    void Execute() { bytes+=chunk.command_offset;chunk.ExecuteAll(buffer,{});++submissions; }
};
#include "command-execute.inc"
#include "query-barrier.inc"
struct QueryHarness {
    struct Impl {
        ChunkHarness scheduler;
        std::vector<std::pair<VkBuffer,VkDeviceSize>> buffers_to_upload_to;
        std::vector<std::vector<VkBufferCopy>> copies_setup;
    } data;
    Impl* impl=&data;
    void Barriers(bool);
    void OldSync(VkBuffer);
    void NewSync(VkBuffer);
    void Setup(VkBuffer buffer, unsigned count) {
        data.buffers_to_upload_to={{buffer,0}};data.copies_setup.resize(1);
        data.copies_setup[0].clear();
        for(unsigned i=0;i<count;++i)data.copies_setup[0].push_back({VkDeviceSize(i*8),VkDeviceSize(i*8),8});
    }
};
#include "query-generic-barriers.inc"
#include "query-old.inc"
#include "query-new.inc"
void Require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
void DeferredTest() {
    QueryHarness old,current;Trace a,b;std::vector<uint64_t> at,bt;a.events=&at;b.events=&bt;
    old.data.scheduler.buffer.trace=&a;current.data.scheduler.buffer.trace=&b;
    auto buffer=reinterpret_cast<VkBuffer>(uintptr_t(1));
    for(unsigned i=0;i<2000;++i) {
        old.Setup(buffer,i%9+1);current.Setup(buffer,i%9+1);
        old.OldSync(buffer);current.NewSync(buffer);
        Require(old.data.buffers_to_upload_to.empty()&&current.data.buffers_to_upload_to.empty(),"Deferred ownership not moved");
        // WFI still records standalone barriers before and after nested query work.
        if(i%17==0){old.Barriers(true);current.Barriers(true);old.Barriers(false);current.Barriers(false);}
    }
    old.data.scheduler.Execute();current.data.scheduler.Execute();
    Require(at==bt&&a.checksum==b.checksum,"Changed barrier/copy order or data");
    Require(old.data.scheduler.written==current.data.scheduler.written,"Write tracking changed");
    Require(old.data.scheduler.records-current.data.scheduler.records==4000,"Not two fewer commands per batch");
    Require(old.data.scheduler.contexts-current.data.scheduler.contexts==2000,"Context request difference unexpected");
    Require(old.data.scheduler.chunk.Empty()&&current.data.scheduler.chunk.Empty(),"Chunk was not reset");
    std::cout<<"PASS deferred ownership, 2000 batches, chunk rollover, WFI nesting, identical GPU command trace; old_records="<<old.data.scheduler.records<<" new_records="<<current.data.scheduler.records<<" old_bytes="<<old.data.scheduler.bytes<<" new_bytes="<<current.data.scheduler.bytes<<'\n';
}
struct Gpu : Fixture {
    using Fixture::Fixture;
    void Test(bool old) {
        for(unsigned i=0;i<64;++i){buffers[0].mapped[i]=1000+i;buffers[1].mapped[i]=0;}
        Begin();auto cb=commands[1];
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,nullptr);
        vkCmdDispatch(cb,1,1,1);
        // Output is independent of the query destination but gets written twice.
        VkBufferMemoryBarrier output{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        output.srcAccessMask=output.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
        output.srcQueueFamilyIndex=output.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        output.buffer=buffers[2].buffer;output.size=4;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&output,0,nullptr);
        QueryHarness q;q.data.scheduler.buffer.handle=cb;
        // Distinct destinations/offsets, two successive batches and same-buffer writes.
        q.Setup(buffers[1].buffer,16);if(old)q.OldSync(buffers[0].buffer);else q.NewSync(buffers[0].buffer);
        q.Setup(buffers[1].buffer,16);if(old)q.OldSync(buffers[0].buffer);else q.NewSync(buffers[0].buffer);
        q.data.scheduler.Execute();vkCmdDispatch(cb,1,1,1);
        Barrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);Submit();
        for(unsigned i=0;i<32;++i)Require(buffers[1].mapped[i]==1000+i,"Query regions incorrect");
        Require(buffers[2].mapped[0]==1000,"Shader did not see query result");
        std::cout<<"PASS GPU old="<<old<<" copied=128B shader_result=1000\n";
    }
};
__declspec(noinline) double Cpu(bool old) {
    QueryHarness q;Trace trace;q.data.scheduler.buffer.trace=&trace;
    // Keep allocations identical: measure only the scheduler/command recording path.
    auto buffer=reinterpret_cast<VkBuffer>(uintptr_t(1));
    auto start=std::chrono::steady_clock::now();
    for(unsigned i=0;i<100000;++i){q.Setup(buffer,1);if(old)q.OldSync(buffer);else q.NewSync(buffer);}
    q.data.scheduler.Execute();auto end=std::chrono::steady_clock::now();
    Require(trace.checksum!=0,"Optimized away benchmark");
    return std::chrono::duration<double,std::nano>(end-start).count()/100000;
}
int main(int argc,char** argv)try {
    if(argc!=3)return 2;
    if(std::string(argv[2])=="bench") {
        std::array<std::vector<double>,2> samples;
        for(int run=0;run<17;++run)for(int i=0;i<2;++i){bool old=(i+run)%2;double ns=Cpu(old);if(run>=2)samples[old].push_back(ns);}
        for(unsigned old=0;old<2;++old){auto& s=samples[old];std::sort(s.begin(),s.end());std::cout<<"CPU_BENCH old="<<old<<" ns_per_batch="<<s[s.size()/2]<<" samples="<<s.size()<<'\n';}
    } else {DeferredTest();Gpu g(argv[1]);g.Test(true);g.Test(false);}
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
