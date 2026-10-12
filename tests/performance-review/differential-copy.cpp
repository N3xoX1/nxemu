#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <random>
#include <vector>
#include "yuzu_common/differential_memory_copy.h"
unsigned checks{},failures{};volatile unsigned observed{};
void Check(const char* name,bool okay){++checks;if(!okay)++failures;std::cout<<(okay?"PASS ":"FAIL ")<<name<<'\n';}
void Verify() {
    constexpr size_t bytes=(8ULL<<20)+63;
    std::vector<unsigned char> src(bytes+128),dst(bytes+128),expected(bytes+128);
    std::mt19937 rng{0xD1FF2026};bool okay=true;
    for(unsigned round=0;round<300;++round) {
        for(size_t i=0;i<src.size();++i)src[i]=static_cast<unsigned char>(i*37+round);
        dst=src;
        for(unsigned i=0;i<100;++i)dst[rng()%dst.size()]^=static_cast<unsigned char>(1+rng()%255);
        const size_t count=round%5==0?0:round%5==1?262144:round%5==2?1048576:bytes;
        const size_t source_offset=rng()%64,dest_offset=rng()%64;
        expected=dst;std::memcpy(expected.data()+dest_offset,src.data()+source_offset,count);
        Common::CopyChangedMemory(dst.data()+dest_offset,src.data()+source_offset,count);
        okay &= dst==expected;
    }
    Check("300_random_sizes_alignments_and_changes_match_complete_memcpy_oracle",okay);
    bool equality_okay=true;
    for(size_t length=0;length<=257;++length)for(size_t shift=0;shift<16;++shift) {
        std::vector<unsigned char> left(length+32,0x3C),right=left;
        equality_okay &= Common::ReadbackMemoryEqual(left.data()+shift,right.data()+shift,length);
        for(size_t byte=0;byte<length;++byte) {
            right[shift+byte]^=1;
            equality_okay &= !Common::ReadbackMemoryEqual(left.data()+shift,right.data()+shift,length);
            right[shift+byte]^=1;
        }
    }
    Check("SIMD_equality_checks_every_byte_lane_alignment_and_tail_against_exact_oracle",equality_okay);
    dst=src;Check("exactly_equal_large_copy_writes_zero_bytes",Common::CopyChangedMemory(dst.data(),src.data(),bytes)==0);
    dst[65536+23]^=1;const auto partial=Common::CopyChangedMemory(dst.data(),src.data(),bytes);
    Check("single_changed_chunk_is_copied_and_other_chunks_preserved",partial==65536&&dst==src);
    for(size_t offset=0;offset<bytes;offset+=65536)dst[offset]^=1;
    Check("dense_changes_use_full_bulk_fallback",Common::CopyChangedMemory(dst.data(),src.data(),bytes)==bytes&&dst==src);
    for(int direction:{-1,0,1}) {
        dst=src;expected=src;
        std::memmove(expected.data()+128+direction*31,expected.data()+128,bytes-128);
        Common::CopyChangedMemory(dst.data()+128+direction*31,dst.data()+128,bytes-128);
        Check("overlapping_aliases_match_memmove",dst==expected);
    }
    auto* a=static_cast<unsigned char*>(VirtualAlloc(nullptr,(2ULL<<20)+8192,MEM_RESERVE,PAGE_NOACCESS));
    auto* b=static_cast<unsigned char*>(VirtualAlloc(nullptr,(2ULL<<20)+8192,MEM_RESERVE,PAGE_NOACCESS));
    if(!a||!b||!VirtualAlloc(a+4096,2ULL<<20,MEM_COMMIT,PAGE_READWRITE)||!VirtualAlloc(b+4096,2ULL<<20,MEM_COMMIT,PAGE_READWRITE))throw std::runtime_error("guard allocation");
    std::memcpy(a+4096,src.data(),2ULL<<20);std::memcpy(b+4096,src.data(),2ULL<<20);
    b[4096+(2ULL<<20)-1]^=1;
    Common::CopyChangedMemory(b+4096,a+4096,2ULL<<20);
    Check("last_changed_byte_at_guard_page_has_exact_read_and_write_bounds",std::memcmp(a+4096,b+4096,2ULL<<20)==0);
    VirtualFree(a,0,MEM_RELEASE);VirtualFree(b,0,MEM_RELEASE);
    std::fill(dst.begin(),dst.end(),0);
    std::atomic<bool> ready{};auto writer=std::async(std::launch::async,[&]{Common::CopyChangedMemory(dst.data(),src.data(),bytes);ready.store(true,std::memory_order_release);});
    while(!ready.load(std::memory_order_acquire)){}
    Check("all_bytes_are_visible_to_CPU_consumer_after_completion",std::memcmp(dst.data(),src.data(),bytes)==0);writer.get();
}
void Benchmark() {
    // Fresh staging ranges roll through 256 MiB; destinations are reused guest
    // buffers. This models the live observation, not one giant hot repeated copy.
    constexpr size_t pool=256ULL<<20;
    std::vector<unsigned char> src(pool+64,0x3C),dst((16ULL<<20)+64,0x3C);
    for(size_t bytes:{2ULL<<20,4ULL<<20,8ULL<<20})for(unsigned pattern:{0U,1U,2U}) {
        std::array<std::vector<double>,2> samples;
        for(unsigned round=0;round<9;++round)for(unsigned order=0;order<2;++order) {
            const unsigned mode=(round+order)%2;
            std::fill(dst.begin(),dst.end(),0x3C);
            const auto start=std::chrono::steady_clock::now();
            for(unsigned slot=0;slot<pool/bytes;++slot) {
                const auto* source=src.data()+slot*bytes;
                if(pattern==1)dst[65536+slot%256]^=1;
                if(pattern==2)for(size_t offset=0;offset<bytes;offset+=65536)dst[offset]^=1;
                if(mode)observed=static_cast<unsigned>(Common::CopyChangedMemory(dst.data(),source,bytes));
                else {std::memcpy(dst.data(),source,bytes);observed=dst[bytes-1];}
            }
            const auto end=std::chrono::steady_clock::now();
            if(std::memcmp(dst.data(),src.data(),bytes))throw std::runtime_error("benchmark data mismatch");
            if(round>=2)samples[mode].push_back(std::chrono::duration<double,std::milli>(end-start).count());
        }
        for(unsigned mode=0;mode<2;++mode) {
            auto& values=samples[mode];std::sort(values.begin(),values.end());
            std::cout<<"BENCH "<<(mode?"differential":"memcpy")<<" chunkMiB="<<(bytes>>20)<<" pattern="<<pattern<<" totalMiB=256 median_ms="<<values[values.size()/2]<<'\n';
        }
    }
}
int main()try{Verify();Benchmark();std::cout<<"RESULT checks="<<checks<<" failed="<<failures<<'\n';return failures?1:0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
