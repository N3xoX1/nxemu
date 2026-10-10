// Deterministic regression for GPU dispatch arguments losing their dirty state before launch.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include <vector>
using u32=std::uint32_t;
using u64=std::uint64_t;
using GPUVAddr=u64;
#define ASSERT_MSG(condition, ...) assert(condition)
#define KEPLER_COMPUTE_REG_INDEX(field) Regs::field##_index
#define LAUNCH_REG_INDEX(field) 12
namespace Settings { bool high=false; bool IsGPULevelHigh(){return high;} }
namespace Engines { enum class EngineTypes {Maxwell3D,KeplerCompute}; }
constexpr u32 ComputeInline=0x6d,MacroRegistersStart=0xe00;
struct Memory {
    bool dirty{};u64 last_address{},last_size{};
    bool IsMemoryDirty(u64 addr,u64 size){last_address=addr;last_size=size;return dirty;}
};
struct Compute {
    struct Regs {
        static constexpr u32 NUM_REGS=4096,exec_upload_index=0x6c,data_upload_index=0x6d,launch_index=0xaf;
        std::array<u32,NUM_REGS> reg_array{};
        struct {u32 linear=1;} exec_upload;
        struct {GPUVAddr Address(){return 0x1000;}} launch_desc_loc;
    } regs;
    struct Upload {
        GPUVAddr target=0x1030;u32 size=12;
        GPUVAddr ExecTargetAddress(){return target;}
        u32 GetUploadSize(){return size;}
        void ProcessExec(bool){}
        void ProcessData(u32,bool){}
        void ProcessData(const u32*,u32){}
    } upload_state;
    Memory& memory_manager;
    explicit Compute(Memory& memory):memory_manager{memory}{}
    GPUVAddr upload_address{},current_dma_segment{};
    bool upload_dirty{},current_dirty{};
    struct UploadInfo {GPUVAddr upload_address,exec_address;u32 copy_size;bool was_dirty{};};
    std::vector<UploadInfo> uploads;
    std::optional<GPUVAddr> indirect_compute{},dispatched_address{};
    void ProcessLaunch(){dispatched_address=indirect_compute;}
    void CallMethod(u32,u32,bool);
    void CallMultiMethod(u32,const u32*,u32,u32);
};
#if FIXED_MODE
#include "fixed-compute.inc"
#else
#include "baseline-compute.inc"
#endif
void RememberDmaOrigin(Memory& memory_manager,Compute& engine,u32 method,u32 words,
                       u32 entry_words,bool non_incrementing,bool is_compute=true) {
    struct {u32 size;} header{entry_words};
    struct {u32 method,method_count,subchannel;u64 dma_get;bool non_incrementing;}
        dma_state{method,words,0,0x8000,non_incrementing};
    std::array<Compute*,1> subchannels{&engine};
    std::array subchannel_type{is_compute?Engines::EngineTypes::KeplerCompute:Engines::EngineTypes::Maxwell3D};
#if FIXED_MODE
#include "fixed-dma-origin.inc"
#else
#include "baseline-dma-origin.inc"
#endif
}
int tests{},failures{};
void Check(const char* label,bool ok){++tests;failures+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<label<<'\n';}
int main(){
    for(bool high:{false,true}) for(bool multi:{false,true}) {
        Settings::high=high;Memory memory{true};Compute compute{memory};
        RememberDmaOrigin(memory,compute,ComputeInline,3,3,true);
        memory.dirty=false; // A safe DMA read/async download has cleared the source dirty bit.
        compute.current_dma_segment=0x8000;
        const u32 stale_arguments[]={0,1,1};
        if(multi)compute.CallMultiMethod(Compute::Regs::data_upload_index,stale_arguments,3,3);
        else compute.CallMethod(Compute::Regs::data_upload_index,0,true);
        compute.CallMethod(Compute::Regs::exec_upload_index,1,true);
        compute.CallMethod(Compute::Regs::launch_index,1,true);
        Check("downloaded_GPU_arguments_still_dispatch_indirectly",compute.dispatched_address==0x8000);
        Check("dirty_origin_consumed_and_launch_state_reset",!compute.current_dirty&&!compute.indirect_compute);
        compute.current_dma_segment=0x9000;
        compute.CallMultiMethod(Compute::Regs::data_upload_index,stale_arguments,3,3);
        compute.CallMethod(Compute::Regs::exec_upload_index,1,true);
        compute.CallMethod(Compute::Regs::launch_index,1,true);
        Check("later_clean_CPU_arguments_do_not_inherit_dirty_origin",!compute.dispatched_address);
    }
    {Memory m{true};Compute c{m};RememberDmaOrigin(m,c,ComputeInline,2,8,true);
     Check("partial_continuation_checks_only_GPU_payload",m.last_address==0x8000&&m.last_size==8);}
    {Memory m{true};Compute c{m};Settings::high=true;RememberDmaOrigin(m,c,MacroRegistersStart,2,8,true,false);
     Check("macro_origin_is_preserved",c.current_dirty&&m.last_size==8);}
    {Memory m{false};Compute c{m};c.current_dma_segment=0x8000;const u32 args[]={5,1,1};
     c.CallMultiMethod(Compute::Regs::data_upload_index,args,3,3);c.CallMethod(Compute::Regs::exec_upload_index,1,true);
     m.dirty=true;c.CallMethod(Compute::Regs::launch_index,1,true);
     Check("new_GPU_write_after_upload_also_dispatches_indirectly",c.dispatched_address==0x8000);}
    {Memory m{true};Compute c{m};RememberDmaOrigin(m,c,ComputeInline,0,3,true);
     Check("completed_command_does_not_mark_new_headers_as_GPU_payload",!c.current_dirty&&m.last_size==0);}
    std::cout<<"tests="<<tests<<" failures="<<failures<<'\n';return failures?1:0;
}
