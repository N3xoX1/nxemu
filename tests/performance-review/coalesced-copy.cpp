#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
#include "yuzu_common/coalesced_memory_copy.h"
#include "yuzu_common/differential_memory_copy.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/scope_exit.h"
#include "diagnostics-stub.h"
volatile unsigned observed{};
unsigned checks{},failures{};
void Check(const char* name,bool okay){++checks;if(!okay)++failures;std::cout<<(okay?"PASS ":"FAIL ")<<name<<'\n';}
struct CopyHarness {
    static constexpr size_t DEVICE_PAGEBITS=12,DEVICE_PAGEMASK=4095;
    std::vector<u8> ram;
    std::vector<u32> compressed_physical_ptr,continuity_tracker;
    explicit CopyHarness(size_t pages=128):ram(pages*4096,0xE3),compressed_physical_ptr(pages),continuity_tracker(pages,1) {}
    unsigned unmapped_count{};
    template<class T>T* GetPointerFromRaw(PAddr addr){return reinterpret_cast<T*>(ram.data()+addr);}
    void WalkBlock(DAddr,std::size_t,auto,auto,auto);
    void WriteBlockUnsafe(DAddr,const void*,size_t);
    void WriteBlockForGpuReadback(DAddr,const void*,size_t);
};
#define LOG_ERROR(...) ++unmapped_count
#include "device-copy.inc"
#undef LOG_ERROR
void Oracle() {
    std::vector<u8> source(512*1024+64);for(size_t i=0;i<source.size();++i)source[i]=static_cast<u8>(i*37+(i>>11));
    std::mt19937 rng{0xD3C01};bool okay=true,holes=true;
    for(unsigned round=0;round<2000;++round) {
        CopyHarness current,baseline;
        for(unsigned page=0;page<128;++page) {
            const u32 physical=round%4==0?page+1:round%4==1?(page/8)*8+(7-page%8)+1:rng()%9==0?0:rng()%128+1;
            current.compressed_physical_ptr[page]=baseline.compressed_physical_ptr[page]=physical;
            if(round%4==0)current.continuity_tracker[page]=baseline.continuity_tracker[page]=128-page;
        }
        const size_t address=rng()%(128*4096),size=rng()%(128*4096-address+1),offset=rng()%64;
        current.WriteBlockForGpuReadback(address,source.data()+offset,size);
        baseline.WriteBlockUnsafe(address,source.data()+offset,size);
        okay &= current.ram==baseline.ram;holes &= current.unmapped_count==baseline.unmapped_count;
    }
    Check("2000_exact_device_walker_copies_match_original_for_contiguous_fragmented_alias_and_partial_ranges",okay);
    Check("unmapped_holes_keep_identical_skip_and_error_behavior",holes);
    // Exercise the production small-readback path with a large physical span,
    // including a mostly unchanged destination. The 4 KiB cases above do not
    // enter the differential copy helper's threshold.
    constexpr size_t small_pages=(8ULL<<20)/4096;
    CopyHarness partial_current(small_pages),partial_baseline(small_pages);
    std::vector<u8> partial_source(8ULL<<20,0xE3);
    for(size_t page=0;page<small_pages;++page) {
        partial_current.compressed_physical_ptr[page]=partial_baseline.compressed_physical_ptr[page]=static_cast<u32>(page+1);
        partial_current.continuity_tracker[page]=partial_baseline.continuity_tracker[page]=static_cast<u32>(small_pages-page);
    }
    partial_source[65536+7]=0x12;partial_source.back()=0x37;
    partial_current.WriteBlockForGpuReadback(0,partial_source.data(),partial_source.size());
    partial_baseline.WriteBlockUnsafe(0,partial_source.data(),partial_source.size());
    Check("production_small_readback_differential_path_matches_original_for_sparse_changes",partial_current.ram==partial_baseline.ram);
    std::vector<u8> large_source((96ULL<<20)+4096);
    for(size_t i=0;i<large_source.size();++i)large_source[i]=static_cast<u8>(i*23+(i>>11));
    bool large_okay=true;
    for(unsigned round=0;round<8;++round) {
        constexpr size_t pages=(96ULL<<20)/4096+1;
        CopyHarness current(pages),baseline(pages);
        for(unsigned page=0;page<pages;++page) {
            const u32 physical=round%4==0?page+1:round%4==1?((page/8)*8+(7-page%8))%pages+1:rng()%9==0?0:rng()%pages+1;
            current.compressed_physical_ptr[page]=baseline.compressed_physical_ptr[page]=physical;
        }
        current.WriteBlockForGpuReadback(31,large_source.data()+17,96ULL<<20);
        baseline.WriteBlockUnsafe(31,large_source.data()+17,96ULL<<20);
        large_okay &= current.ram==baseline.ram&&current.unmapped_count==baseline.unmapped_count;
    }
    Check("eight_96MiB_production_bulk_paths_match_original_with_holes_aliases_and_unaligned_ends",large_okay);
    std::vector<u8> dst(32768,0xE3);Common::CoalescedMemoryCopy copies;unsigned runs=0;
    for(unsigned page=0;page<4;++page)runs+=copies.Append(dst.data()+page*4096,source.data()+page*4096,4096);
    runs+=copies.Flush();
    Check("four_contiguous_spans_emit_one_copy",runs==1&&std::equal(dst.begin(),dst.begin()+16384,source.begin()));
    Common::CoalescedMemoryCopy fragmented;runs=0;
    runs+=fragmented.Append(dst.data(),source.data(),4096);
    runs+=fragmented.Append(dst.data()+8192,source.data()+4096,4096);
    runs+=fragmented.Append(dst.data()+12288,source.data()+16384,4096);
    runs+=fragmented.Flush();
    Check("destination_gap_and_source_gap_both_end_run",runs==3);
    Check("empty_flush_has_no_copy",!fragmented.Flush()&&!fragmented.Append(nullptr,nullptr,0));
    // End the copied bytes exactly at inaccessible pages: no span may overrun.
    auto* a=static_cast<u8*>(VirtualAlloc(nullptr,12288,MEM_RESERVE,PAGE_NOACCESS));
    auto* b=static_cast<u8*>(VirtualAlloc(nullptr,12288,MEM_RESERVE,PAGE_NOACCESS));
    if(!a||!b||!VirtualAlloc(a+4096,4096,MEM_COMMIT,PAGE_READWRITE)||!VirtualAlloc(b+4096,4096,MEM_COMMIT,PAGE_READWRITE))throw std::runtime_error("guard allocation");
    std::memcpy(a+4096,source.data(),4096);Common::CoalescedMemoryCopy guarded;
    for(size_t offset=0;offset<4096;offset+=64)guarded.Append(b+4096+offset,a+4096+offset,64);
    guarded.Flush();Check("coalesced_copy_at_guard_page_boundary_keeps_exact_bounds",std::memcmp(a+4096,b+4096,4096)==0);
    VirtualFree(a,0,MEM_RELEASE);VirtualFree(b,0,MEM_RELEASE);
}
int main() try {
    Oracle();
    constexpr size_t capacity=128ULL<<20;
    auto* src=static_cast<unsigned char*>(VirtualAlloc(nullptr,capacity+4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    auto* dst=static_cast<unsigned char*>(VirtualAlloc(nullptr,capacity+4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!src||!dst)return 2;
    for(size_t i=0;i<capacity+4096;++i)src[i]=static_cast<unsigned char>(i*17+(i>>13));
    for(size_t bytes:{1ULL<<20,16ULL<<20,64ULL<<20,96ULL<<20,128ULL<<20}) {
        std::array<std::vector<double>,2> samples;
        for(unsigned round=0;round<9;++round)for(unsigned order=0;order<2;++order) {
            const unsigned mode=(round+order)%2;
            const auto start=std::chrono::steady_clock::now();
            for(unsigned repeat=0;repeat<8;++repeat) {
                Common::CoalescedMemoryCopy copy;
                for(size_t offset=0;offset<bytes;offset+=4096) {
                    if(mode)copy.Append(dst+17+offset,src+31+offset,4096);
                    else std::memcpy(dst+17+offset,src+31+offset,4096);
                }
                copy.Flush();observed=dst[17+bytes-1]+dst[repeat*8191];
            }
            const auto end=std::chrono::steady_clock::now();
            if(std::memcmp(dst+17,src+31,bytes))return 1;
            if(round>=2)samples[mode].push_back(std::chrono::duration<double,std::micro>(end-start).count()/8);
        }
        for(unsigned mode=0;mode<2;++mode) {
            auto& values=samples[mode];std::sort(values.begin(),values.end());
            std::cout<<"BENCH "<<(mode?"coalesced":"4KiB_memcpy")<<" MiB="<<(bytes>>20)<<" median_us="<<values[values.size()/2]<<'\n';
        }
    }
    VirtualFree(src,0,MEM_RELEASE);VirtualFree(dst,0,MEM_RELEASE);
    std::cout<<"PASS actual copied bytes match on all samples\n";
    std::cout<<"RESULT checks="<<checks<<" failed="<<failures<<'\n';return failures?1:0;
}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
