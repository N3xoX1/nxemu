// SPDX-License-Identifier: GPL-2.0-or-later
// ARM experiments only. QEMU establishes correctness, never device performance.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
#include "yuzu_common/differential_memory_copy.h"

#if defined(__aarch64__)
using Byte = unsigned char;
using Equal = Common::ReadbackDetail::Comparer;

template<size_t Step, bool Prefetch>
__attribute__((noinline)) bool EqualXor(const Byte* a, const Byte* b, size_t n) {
    while (n >= Step) {
        if constexpr (Prefetch) {
            if (n > 4096) {
                __builtin_prefetch(a + 2048, 0, 3);
                __builtin_prefetch(b + 2048, 0, 3);
            }
        }
        uint8x16_t x = vdupq_n_u8(0), y = x, z = x, w = x;
        for (size_t i = 0; i < Step; i += 64) {
            x = vorrq_u8(x, veorq_u8(vld1q_u8(a+i), vld1q_u8(b+i)));
            y = vorrq_u8(y, veorq_u8(vld1q_u8(a+i+16), vld1q_u8(b+i+16)));
            z = vorrq_u8(z, veorq_u8(vld1q_u8(a+i+32), vld1q_u8(b+i+32)));
            w = vorrq_u8(w, veorq_u8(vld1q_u8(a+i+48), vld1q_u8(b+i+48)));
        }
        if (vmaxvq_u8(vorrq_u8(vorrq_u8(x,y), vorrq_u8(z,w)))) return false;
        a += Step; b += Step; n -= Step;
    }
    return Common::ReadbackDetail::EqualNeon(a,b,n);
}

__attribute__((noinline)) bool EqualQuadLoad(const Byte* a, const Byte* b, size_t n) {
    while (n >= 64) {
        const auto x = vld1q_u8_x4(a), y = vld1q_u8_x4(b);
        const auto p = vandq_u8(vceqq_u8(x.val[0],y.val[0]), vceqq_u8(x.val[1],y.val[1]));
        const auto q = vandq_u8(vceqq_u8(x.val[2],y.val[2]), vceqq_u8(x.val[3],y.val[3]));
        if (vminvq_u8(vandq_u8(p,q)) != 255) return false;
        a += 64; b += 64; n -= 64;
    }
    return Common::ReadbackDetail::EqualNeon(a,b,n);
}

struct Strategy { const char* name; Equal equal; };
const std::array strategies{
    Strategy{"production_neon", Common::ReadbackDetail::EqualNeon},
    Strategy{"libc", Common::ReadbackDetail::EqualPortable},
    Strategy{"xor64", EqualXor<64,false>},
    Strategy{"xor256", EqualXor<256,false>},
    Strategy{"prefetch256", EqualXor<256,true>},
    Strategy{"quad_load", EqualQuadLoad},
};

struct Guarded {
    size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    size_t bytes = page * 3;
    Byte* allocation = static_cast<Byte*>(mmap(nullptr,bytes,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    Guarded() {
        if (allocation == MAP_FAILED || mprotect(allocation+page,page,PROT_READ|PROT_WRITE))
            throw std::runtime_error("guard allocation");
        std::memset(allocation+page,0xB7,page);
    }
    ~Guarded() { if (allocation != MAP_FAILED) munmap(allocation,bytes); }
    Byte* End() const { return allocation + page*2; }
};

bool Verify(Equal equal) {
    std::vector<Byte> a(131072+128), b(a.size());
    for (size_t i=0;i<a.size();++i) a[i]=static_cast<Byte>(i*37+9);
    // Every lane at SIMD/unroll boundaries, and all short tail lengths.
    for (size_t n=0;n<=513;++n) for (size_t shift : {0u,1u,7u,31u,63u}) {
        b=a;
        if (!equal(a.data()+shift,b.data()+shift,n)) return false;
        for (size_t i=0;i<n;++i) {
            b[shift+i]^=0x80;
            if (equal(a.data()+shift,b.data()+shift,n)) return false;
            b[shift+i]^=0x80;
        }
    }
    std::mt19937 rng{0xA64C};
    for (unsigned round=0;round<300;++round) {
        const size_t x=rng()%64, y=rng()%64, n=rng()%131073;
        b=a;
        if (round%3 && n) b[y+rng()%n]^=0xFF;
        if (equal(a.data()+x,b.data()+y,n) != (std::memcmp(a.data()+x,b.data()+y,n)==0)) return false;
    }
    Guarded ga,gb;
    // No access beyond either exact end, including a zero-length guard pointer.
    for (size_t n=0;n<=1024;++n) {
        if (!equal(ga.End()-n,gb.End()-n,n)) return false;
        if (n) {
            gb.End()[-1]^=1;
            if (equal(ga.End()-n,gb.End()-n,n)) return false;
            gb.End()[-1]^=1;
        }
    }
    return true;
}

void Benchmark() {
    constexpr size_t chunk=65536, logical=128*1024*1024;
    std::cout << "strategy,working_MiB,changed_chunk_percent,logical_MiB,median_ms\n";
    for (size_t working : {4u*1024*1024,16u*1024*1024,64u*1024*1024}) {
        std::vector<Byte> a(working),b(working);
        for (size_t i=0;i<working;++i) a[i]=static_cast<Byte>(i*43+5);
        for (unsigned pattern : {0u,10u,100u}) {
            b=a;
            for (size_t i=0;i<working/chunk;++i) if (i%100<pattern) b[i*chunk+chunk-1]^=1;
            size_t equal_chunks=0;
            for (size_t i=0;i<working/chunk;++i) equal_chunks+=i%100>=pattern;
            const size_t expected=equal_chunks*(logical/working);
            std::array<std::vector<double>,strategies.size()> samples;
            for (unsigned repetition=0;repetition<7;++repetition) {
                for (size_t order=0;order<strategies.size();++order) {
                    const size_t k=(order+repetition)%strategies.size();
                    size_t equal_count=0;
                    const auto start=std::chrono::steady_clock::now();
                    for (size_t offset=0;offset<logical;offset+=chunk) {
                        const size_t i=offset%working;
                        equal_count+=strategies[k].equal(a.data()+i,b.data()+i,chunk);
                    }
                    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
                    if (equal_count!=expected) throw std::runtime_error("benchmark oracle");
                    if (repetition>=2) samples[k].push_back(ms);
                }
            }
            for (size_t k=0;k<strategies.size();++k) {
                auto& times=samples[k];std::sort(times.begin(),times.end());
                size_t changed_chunks=working/chunk-equal_chunks;
                std::cout<<strategies[k].name<<','<<working/(1024*1024)<<','
                         <<100.0*changed_chunks/(working/chunk)<<",128,"<<times[times.size()/2]<<'\n';
            }
        }
    }
}

int main(int argc,char** argv) try {
    unsigned failures=0;
    for (const auto& strategy : strategies) {
        const bool okay=Verify(strategy.equal); failures+=!okay;
        std::cout<<(okay?"PASS ":"FAIL ")<<strategy.name<<" exact bytes, lanes, tails, alignments and guards\n";
    }
    std::cout<<"RESULT strategies="<<strategies.size()<<" failed="<<failures<<'\n';
    if (failures) return 1;
    if (argc==2 && std::strcmp(argv[1],"--benchmark")==0) Benchmark();
    return 0;
} catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 2; }
#else
int main() { std::cerr<<"This fixture requires an AArch64 build.\n"; return 2; }
#endif
