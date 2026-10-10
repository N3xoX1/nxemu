#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
using u32=std::uint32_t;using u64=std::uint64_t;
using DAddr=u64;using GPUVAddr=u64;using BufferId=u32;
#include "binding.inc"
constexpr Binding NULL_BINDING{};
namespace Common {
template<class T> T AlignDown(T value,u32 alignment){return value&~(T{alignment}-1);}
template<class T> T AlignUp(T value,u64 alignment){return (value+alignment-1)&~(alignment-1);}
}
namespace Core {constexpr u64 DEVICE_PAGESIZE=4096;}
constexpr u64 operator""_MiB(unsigned long long value){return value*1024*1024;}
#define LOG_WARNING(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define ASSERT_MSG(value,...) do {if(!(value))throw std::runtime_error("bad mapping");}while(false)
struct Memory {
    u64 base=0x10000,descriptor=0x40000,physical=0x200000;
    u32 size=8192,continuous=0,layout=8192;
    bool prefix_mapped=false,sparse=true,later_pages_mapped=true;
    u64 region_base=0x10000,region_size=16384;
    template<class T> T Read(GPUVAddr addr)const{
        if(addr==descriptor)return static_cast<T>(base);
        if(addr==descriptor+8)return static_cast<T>(size);
        throw std::runtime_error("unexpected descriptor read");
    }
    u64 GetMemoryLayoutSize(GPUVAddr)const{return layout;}
    u64 MaxContinuousRange(GPUVAddr,u64 requested)const{return std::min<u64>(continuous,requested);}
    std::optional<DAddr> GpuToCpuAddress(GPUVAddr addr)const{
        if(prefix_mapped)return physical+(addr-(base&~u64{255}));
        return std::nullopt;
    }
    std::optional<std::pair<GPUVAddr,std::size_t>> GetSparseRegion(GPUVAddr addr)const{
        if(sparse&&addr>=region_base&&addr<region_base+region_size)return std::pair{region_base,static_cast<std::size_t>(region_size)};
        return std::nullopt;
    }
    std::vector<std::pair<GPUVAddr,std::size_t>> GetSubmappedRange(GPUVAddr addr,u32 span)const{
        if(!later_pages_mapped||span<2)return {};
        return {{addr+span/2,span-span/2}};
    }
};
struct Runtime {u32 GetStorageBufferAlignment()const{return 256;}};
struct Harness {
    Memory memory;const Memory* gpu_memory=&memory;Runtime runtime;
    Binding StorageBufferBinding(GPUVAddr,u32,bool)const;
    Binding Resolve(bool write=false,u32 index=0)const{return StorageBufferBinding(memory.descriptor,index,write);}
};
#if FIXED_MODE
#include "fixed-storage.inc"
#else
#include "baseline-storage.inc"
#endif
int total=0,failed=0;
void Check(const char* name,bool ok){++total;failed+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<name<<'\n';}
int main(){
    {Harness h;auto b=h.Resolve();Check("unmapped_prefix_retains_later_pages",b.is_sparse&&b.size==8192&&b.sparse_gpu_addr==0x10000&&b.device_addr==0);}
    {Harness h;auto b=h.Resolve(true);Check("unmapped_prefix_is_writable",b.is_sparse&&b.size==8192);}
    {Harness h;h.memory.base+=32;auto b=h.Resolve();Check("unaligned_hole_keeps_descriptor_offset",b.is_sparse&&b.sparse_gpu_addr==0x10000&&b.size==8224);}
    {Harness h;h.memory.size=61440;auto b=h.Resolve();Check("sparse_size_stops_at_reservation_end",b.is_sparse&&b.size==16384);}
    {Harness h;h.memory.base=0x13020;h.memory.size=61440;auto b=h.Resolve(true);Check("interior_descriptor_stops_at_reservation_end",b.is_sparse&&b.size==4096&&b.sparse_gpu_addr==0x13000);}
    {Harness h;h.memory.size=60*1024*1024;h.memory.region_size=h.memory.size;auto b=h.Resolve();Check("immortals_60mb_pool_with_leading_hole",b.is_sparse&&b.size==60*1024*1024);}
    {Harness h;h.memory.size=0;auto b=h.Resolve();Check("custom_cbuf_layout_supports_leading_hole",b.is_sparse&&b.size==8192);}
    {Harness h;h.memory.layout=32*1024*1024;h.memory.region_size=h.memory.layout;auto b=h.Resolve(false,1);Check("custom_cbuf_size_remains_capped",b.is_sparse&&b.size==8*1024*1024);}
    {Harness h;h.memory.prefix_mapped=true;h.memory.continuous=4096;auto b=h.Resolve();Check("mapped_prefix_and_later_hole_still_sparse",b.is_sparse&&b.device_addr==h.memory.physical&&b.size==8192);}
    {Harness h;h.memory.prefix_mapped=true;h.memory.continuous=8192;auto b=h.Resolve();Check("fully_mapped_linear_read_unchanged",!b.is_sparse&&b.device_addr==h.memory.physical&&b.size==8192);}
    {Harness h;h.memory.prefix_mapped=true;h.memory.continuous=8192;h.memory.base+=32;h.memory.size=37;auto b=h.Resolve(true);Check("linear_write_preserves_exact_size",!b.is_sparse&&b.size==69);}
    {Harness h;h.memory.prefix_mapped=true;h.memory.continuous=8192;h.memory.base+=32;h.memory.size=37;auto b=h.Resolve();Check("linear_read_preserves_page_rounding",!b.is_sparse&&b.size==4096);}
    {Harness h;h.memory.sparse=false;auto b=h.Resolve();Check("ordinary_unmapped_address_stays_null",!b.is_sparse&&b.size==0);}
    {Harness h;h.memory.size=h.memory.layout=0;auto b=h.Resolve();Check("empty_descriptor_stays_null",!b.is_sparse&&b.size==0);}
    {Harness h;h.memory.later_pages_mapped=false;auto b=h.Resolve();Check("wholly_unmapped_sparse_descriptor_stays_null",!b.is_sparse&&b.size==0);}
    {Harness h;h.memory.region_size=60*1024*1024;h.memory.base+=32*1024*1024;h.memory.later_pages_mapped=false;auto b=h.Resolve(true);Check("hole_beyond_resident_extent_stays_null",!b.is_sparse&&b.size==0);}
    {Harness h;h.memory.prefix_mapped=true;h.memory.sparse=false;h.memory.continuous=4096;auto b=h.Resolve(true);Check("linear_overrun_still_truncated",!b.is_sparse&&b.size==4096);}
    const auto begin=std::chrono::steady_clock::now();u64 checksum=0;Harness h;
    h.memory.prefix_mapped=true;h.memory.continuous=8192;
    for(u32 i=0;i<1000000;++i){h.memory.size=1+(i%8192);checksum+=h.Resolve(i&1).size;}
    const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    std::cout<<"linear_resolve_1m_ms="<<elapsed<<" checksum="<<checksum<<'\n';
    std::cout<<"checks="<<total<<" failed="<<failed<<'\n';return failed?1:0;
}
