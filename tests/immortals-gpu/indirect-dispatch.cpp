// Exercise the production handlers in hardware order: EXEC -> DATA -> LAUNCH.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include <vector>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using GPUVAddr = u64;
#define ASSERT_MSG(condition, ...) assert(condition)
#define KEPLER_COMPUTE_REG_INDEX(field) Regs::field##_index
#define LAUNCH_REG_INDEX(field) 12
namespace Settings { bool high=false; bool IsGPULevelHigh(){return high;} }
namespace Engines { enum class EngineTypes {Maxwell3D,KeplerCompute}; }
constexpr u32 ComputeInline=0x6d, MacroRegistersStart=0xe00;
struct Memory {
    bool dirty{}; u64 last_address{}, last_size{};
    bool IsMemoryDirty(u64 address, u64 size) {
        last_address=address; last_size=size; return dirty;
    }
};
struct Compute {
    struct Regs {
        static constexpr u32 NUM_REGS=4096, exec_upload_index=0x6c,
                             data_upload_index=0x6d, launch_index=0xaf;
        std::array<u32, NUM_REGS> reg_array{};
        struct {u32 linear=1;} exec_upload;
        struct {u32 line_count=1;} upload;
        struct {GPUVAddr Address(){return 0x1000;}} launch_desc_loc;
    } regs;
    struct Upload {
        GPUVAddr target=0x1030;
        u32 size=8, active_size=0, write_offset=0, submitted=0;
        bool out_of_bounds=false;
        std::array<u32,4> payload{}, destination{};
        GPUVAddr ExecTargetAddress(){return target;}
        u32 GetUploadSize(){return active_size;}
        void ProcessExec(bool){active_size=size;write_offset=0;}
        void ProcessData(u32 data,bool last){
            payload[write_offset/4]=data;write_offset+=4;
            if(last){destination=payload;++submitted;}
        }
        void ProcessData(const u32* data,u32 words){
            if(words*4<active_size){out_of_bounds=true;return;}
            std::copy_n(data,active_size/4,destination.data());++submitted;
        }
    } upload_state;
    Memory& memory_manager;
    explicit Compute(Memory& memory):memory_manager{memory}{}
    // Retain the old fields so the unchanged v8 implementation can also be tested.
    GPUVAddr upload_address{}, current_dma_segment{}, upload_target{};
    bool upload_dirty{}, current_dirty{}, upload_linear{};
    u32 upload_size{}, uploaded_bytes{};
    struct UploadInfo {GPUVAddr upload_address,exec_address; u32 copy_size; bool was_dirty{};};
    std::vector<UploadInfo> uploads;
    std::optional<GPUVAddr> indirect_compute{}, indirect_compute_yz{}, dispatched_x{}, dispatched_yz{};
    void ProcessLaunch(){dispatched_x=indirect_compute; dispatched_yz=indirect_compute_yz;}
    void CallMethod(u32,u32,bool);
    void CallMultiMethod(u32,const u32*,u32,u32);
    void RecordUploadSource(u32);
    void Exec(GPUVAddr target, u32 bytes) {
        upload_state.target=target; upload_state.size=bytes;
        CallMethod(Regs::exec_upload_index,1,true);
    }
    void Data(GPUVAddr source, u32 words, bool dirty, bool multi=true) {
        const u32 stale[4]{};
        current_dma_segment=source; current_dirty=dirty;
        if(multi) CallMultiMethod(Regs::data_upload_index,stale,words,words);
        else for(u32 i=0;i<words;++i) {
            current_dma_segment=source+i*sizeof(u32); current_dirty=dirty;
            CallMethod(Regs::data_upload_index,0,i+1==words);
        }
    }
    void Launch(){CallMethod(Regs::launch_index,1,true);}
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
void Check(const char* label,bool ok) {
    ++tests; failures+=!ok; std::cout<<(ok?"PASS ":"FAIL ")<<label<<'\n';
}
int main() {
    for(bool high:{false,true}) for(bool multi:{false,true}) {
        Settings::high=high; Memory m; Compute c{m};
        // A previous upload must not supply the source or length of the next one.
        c.Exec(0x2000,12); c.Data(0x7000,3,true,multi);
        c.Exec(0x1030,8); c.Data(0x8000,2,true,multi); c.Launch();
        Check("current_upload_X_source",c.dispatched_x==0x8000);
        Check("current_upload_packed_YZ_source",c.dispatched_yz==0x8004);
        Check("launch_consumes_origin_and_resets_state",
              !c.current_dirty&&!c.indirect_compute&&!c.indirect_compute_yz);
        c.Exec(0x1030,4); c.Data(0x9000,1,false,multi); c.Launch();
        Check("clean_CPU_upload_does_not_inherit_previous_GPU_source",
              !c.dispatched_x&&!c.dispatched_yz);
    }
    {Memory m;Compute c{m};c.Exec(0x1030,4);c.Data(0x8000,1,true);c.Launch();
     Check("X_only_preserves_static_YZ",c.dispatched_x==0x8000&&!c.dispatched_yz);}
    {Memory m;Compute c{m};c.Exec(0x1034,4);c.Data(0x8000,1,true);c.Launch();
     Check("YZ_only_preserves_static_X",!c.dispatched_x&&c.dispatched_yz==0x8000);}
    {Memory m;Compute c{m};c.Exec(0x102c,12);c.Data(0x8000,3,true);c.Launch();
     Check("overlapping_upload_tracks_each_QMD_word",c.dispatched_x==0x8004&&c.dispatched_yz==0x8008);}
    {Memory m;Compute c{m};c.Exec(0x1030,8);c.Data(0x8000,1,true);c.Data(0x9000,1,true);c.Launch();
     Check("split_upload_keeps_discontiguous_sources",c.dispatched_x==0x8000&&c.dispatched_yz==0x9000);}
    {Memory m;Compute c{m};const u32 x=693,yz=0x00010001;c.Exec(0x1030,8);
     c.current_dma_segment=0x8000;c.CallMultiMethod(Compute::Regs::data_upload_index,&x,1,2);
     const bool deferred=c.upload_state.submitted==0;
     c.current_dma_segment=0x9000;c.CallMultiMethod(Compute::Regs::data_upload_index,&yz,1,1);
     Check("partial_payload_is_accumulated_without_overread",deferred&&!c.upload_state.out_of_bounds&&
           c.upload_state.submitted==1&&c.upload_state.destination[0]==x&&c.upload_state.destination[1]==yz);}
    {Memory m;Compute c{m};const u32 x=7,yz=0x00020003;c.Exec(0x1030,8);
     c.current_dma_segment=0x8000;c.CallMethod(Compute::Regs::data_upload_index,x,true);
     const bool deferred=c.upload_state.submitted==0;
     c.current_dma_segment=0x9000;c.CallMultiMethod(Compute::Regs::data_upload_index,&yz,1,1);
     Check("single_word_command_waits_for_complete_upload",deferred&&c.upload_state.submitted==1&&
           c.upload_state.destination[0]==x&&c.upload_state.destination[1]==yz);}
    {Memory m;Compute c{m};c.Exec(0x1030,8);c.Data(0x8000,2,true);
     c.Exec(0x1030,4);c.Data(0x9000,1,false);c.Launch();
     Check("later_CPU_overwrite_replaces_only_X",!c.dispatched_x&&c.dispatched_yz==0x8004);}
    {Memory m;Compute c{m};c.Exec(0x1030,8);c.Data(0x8000,2,true);c.Exec(0x1030,8);c.Launch();
     Check("EXEC_without_DATA_does_not_create_phantom_source",c.dispatched_x==0x8000&&c.dispatched_yz==0x8004);}
    {Memory m;Compute c{m};c.Exec(0x1030,8);c.Data(0x8000,2,false);m.dirty=true;c.Launch();
     Check("new_GPU_write_after_upload_is_detected",c.dispatched_x==0x8000&&c.dispatched_yz==0x8004);}
    {Memory m{true};Compute c{m};RememberDmaOrigin(m,c,ComputeInline,2,8,true);
     Check("partial_continuation_checks_only_GPU_payload",m.last_address==0x8000&&m.last_size==8);}
    {Memory m{true};Compute c{m};Settings::high=true;RememberDmaOrigin(m,c,MacroRegistersStart,2,8,true,false);
     Check("macro_origin_is_preserved",c.current_dirty&&m.last_size==8);}
    {Memory m{true};Compute c{m};RememberDmaOrigin(m,c,ComputeInline,0,3,true);
     Check("completed_command_does_not_mark_headers_as_payload",!c.current_dirty&&m.last_size==0);}
    {Memory m;Compute c{m};for(u32 i=0;i<10000;++i){c.Exec(0x2000+i*4,4);c.Data(0x8000+i*4,1,true);}
     Check("contiguous_payloads_merge_without_growing_launch_scan",c.uploads.size()==1&&c.uploads[0].copy_size==40000);}
    std::cout<<"tests="<<tests<<" failures="<<failures<<'\n';
    return failures?1:0;
}
