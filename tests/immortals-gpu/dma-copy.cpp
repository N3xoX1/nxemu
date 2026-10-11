#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <vector>
using u8=std::uint8_t;using u32=std::uint32_t;using u64=std::uint64_t;
using GPUVAddr=u64;using DAddr=u64;
#define LOG_INFO(...) ((void)0)
struct Memory {
    static constexpr u64 page=4096;
    std::array<u32,12> mapping{};
    std::vector<u8> bytes=std::vector<u8>(page*24);
    u32 reads{},writes{};
    Memory(){for(u32 i=0;i<mapping.size();++i)mapping[i]=i;}
    std::optional<DAddr> GpuToCpuAddress(GPUVAddr a)const{
        if(a/page>=mapping.size()||mapping[a/page]==23)return {};
        return mapping[a/page]*page+a%page;
    }
    u64 MaxContinuousRange(GPUVAddr addr,u64 size)const{
        u64 found=0;auto first=GpuToCpuAddress(addr);if(!first)return 0;
        while(found<size){auto p=GpuToCpuAddress(addr+found);if(!p||*p!=*first+found)break;
            found+=std::min(size-found,page-(addr+found)%page);}
        return found;
    }
    std::vector<u8> Snapshot(GPUVAddr a,u64 n)const{
        std::vector<u8> result(n);for(u64 i=0;i<n;++i){auto p=GpuToCpuAddress(a+i);result[i]=p?bytes[*p]:0;}return result;
    }
    void ReadBlock(GPUVAddr a,u8* dst,u64 n){++reads;auto data=Snapshot(a,n);std::memcpy(dst,data.data(),n);}
    void WriteBlockCached(GPUVAddr a,const u8* src,u64 n){++writes;
        for(u64 i=0;i<n;++i)if(auto p=GpuToCpuAddress(a+i))bytes[*p]=src[i];}
};
struct Scratch:std::vector<u8>{void resize_destructive(std::size_t n){resize(n);}};
struct Harness{
    Memory memory;Memory* gpu_memory=&memory;
    bool CanAccelerate(GPUVAddr,GPUVAddr,u64);
    bool Copy(u64 src,u64 dst,u32 n){
        const bool accelerated=CanAccelerate(src,dst,n);
        if(accelerated){auto s=*memory.GpuToCpuAddress(src),d=*memory.GpuToCpuAddress(dst);
            auto old=memory.bytes;std::copy_n(old.begin()+s,n,memory.bytes.begin()+d);
        }else{
            auto& memory_manager=memory;Scratch read_buffer;
            struct {u64 offset_in,offset_out;u32 line_length_in;} regs{src,dst,n};
#include "fixed-fallback.inc"
        }
        return accelerated;
    }
};
#if FIXED_MODE
#include "fixed-checks.inc"
#else
#include "baseline-checks.inc"
#endif
int total{},failed{};
void Check(const char* name,bool ok){++total;failed+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<name<<'\n';}
int main(){
    {Harness h;Check("linear_transfer_remains_accelerated",h.CanAccelerate(32,4*4096+64,8000));}
    {Harness h;h.memory.mapping[1]=17;Check("fragmented_source_declined",!h.CanAccelerate(32,4*4096,8000));}
    {Harness h;h.memory.mapping[5]=19;Check("fragmented_destination_declined",!h.CanAccelerate(32,4*4096,8000));}
    {Harness h;h.memory.mapping[1]=23;Check("hole_after_first_byte_declined",!h.CanAccelerate(32,4*4096,8000));}
    {Harness h;Check("same_virtual_range_declined",!h.CanAccelerate(32,32,8000));}
    {Harness h;Check("forward_overlap_declined",!h.CanAccelerate(32,1024,8000));}
    {Harness h;Check("backward_overlap_declined",!h.CanAccelerate(1024,32,8000));}
    {Harness h;h.memory.mapping[4]=0;Check("distinct_virtual_physical_alias_declined",!h.CanAccelerate(32,4*4096+64,2000));}
    {Harness h;Check("adjacent_physical_ranges_valid",h.CanAccelerate(32,4096+32,4096));}
    {Harness h;Check("zero_length_declined",!h.CanAccelerate(32,4*4096,0));}
    {Harness h;Check("size_above_buffer_cache_u32_declined",!h.CanAccelerate(32,4*4096,u64{1}<<32));}
    // A streaming upload crossing a physical-page break must not overwrite the LUT
    // in the next physical page. This is the class of corruption seen in the volume dump.
    {Harness h;h.memory.mapping[5]=19;
     for(u32 i=0;i<h.memory.bytes.size();++i)h.memory.bytes[i]=u8((i*37+i/113)&255);
     const auto initial=h.memory.bytes;const auto expected=h.memory.Snapshot(64,8000);
     const bool accelerated=h.Copy(64,4*4096+64,8000);
     Check("fragmented_upload_uses_page_aware_fallback",!accelerated);
     Check("streamed_volume_bytes_correct",h.memory.Snapshot(4*4096+64,8000)==expected);
     Check("neighbor_LUT_preserved",std::equal(initial.begin()+5*4096,initial.begin()+6*4096,h.memory.bytes.begin()+5*4096));
     Check("source_snapshot_precedes_single_cached_write",h.memory.reads==1&&h.memory.writes==1);}
    {Harness h;for(u32 i=0;i<h.memory.bytes.size();++i)h.memory.bytes[i]=u8(i*13+i/17);
     const auto expected=h.memory.Snapshot(32,8000);h.Copy(32,1024,8000);
     Check("overlapping_copy_matches_source_snapshot",h.memory.Snapshot(1024,8000)==expected);}
    {Harness h;h.memory.mapping[4]=0;
     for(u32 i=0;i<h.memory.bytes.size();++i)h.memory.bytes[i]=u8(i*11+i/31);
     const auto expected=h.memory.Snapshot(32,3000);h.Copy(32,4*4096+256,3000);
     Check("physical_alias_copy_matches_source_snapshot",h.memory.Snapshot(4*4096+256,3000)==expected);}
    u32 mismatches=0;std::mt19937 random(396);
    for(u32 k=0;k<256;++k){Harness h;h.memory.mapping[1]=12+random()%10;h.memory.mapping[5]=12+random()%10;
        for(auto& b:h.memory.bytes)b=u8(random());
        const auto expected=h.memory.Snapshot(64,8000);h.Copy(64,4*4096+64,8000);
        mismatches+=h.memory.Snapshot(4*4096+64,8000)!=expected;}
    Check("256_randomized_fragmented_uploads",mismatches==0);
    std::cout<<"checks="<<total<<" failed="<<failed<<" randomized_mismatches="<<mismatches<<'\n';return failed?1:0;
}
