// Compare exact CPU retirement strategies against an independent byte oracle.
// Experimental comparers stay in the fixture; the copy worker pool is shared
// with production. No AVX-512 dispatch is installed in the emulator.
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <thread>
#include <vector>
#include "yuzu_common/differential_memory_copy.h"
#include "yuzu_common/parallel_readback_copy.h"

using Byte = unsigned char;
using Equal = bool (*)(const Byte*, const Byte*, size_t);
using Copy = size_t (*)(void*, const void*, size_t);
volatile size_t observed;
bool EqualLibc(const Byte* a, const Byte* b, size_t n) { return !std::memcmp(a,b,n); }

template<size_t Step, bool Prefetch>
__declspec(noinline) bool EqualXor(const Byte* a, const Byte* b, size_t n) {
    while(n>=Step) {
        if constexpr(Prefetch)if(n>4096){_mm_prefetch(reinterpret_cast<const char*>(a+2048),_MM_HINT_T0);_mm_prefetch(reinterpret_cast<const char*>(b+2048),_MM_HINT_T0);}
        __m256i x=_mm256_setzero_si256(),y=x,z=x,w=x;
        for(size_t offset=0;offset<Step;offset+=128) {
            x=_mm256_or_si256(x,_mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a+offset)),_mm256_loadu_si256(reinterpret_cast<const __m256i*>(b+offset))));
            y=_mm256_or_si256(y,_mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a+offset+32)),_mm256_loadu_si256(reinterpret_cast<const __m256i*>(b+offset+32))));
            z=_mm256_or_si256(z,_mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a+offset+64)),_mm256_loadu_si256(reinterpret_cast<const __m256i*>(b+offset+64))));
            w=_mm256_or_si256(w,_mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a+offset+96)),_mm256_loadu_si256(reinterpret_cast<const __m256i*>(b+offset+96))));
        }
        const auto diff=_mm256_or_si256(_mm256_or_si256(x,y),_mm256_or_si256(z,w));
        if(!_mm256_testz_si256(diff,diff))return false;
        a+=Step;b+=Step;n-=Step;
    }
    return Common::ReadbackDetail::EqualSse2(a,b,n);
}
__declspec(noinline) bool Equal512(const Byte* a,const Byte* b,size_t n) {
    while(n>=256) {
        const auto x=_mm512_xor_si512(_mm512_loadu_si512(a),_mm512_loadu_si512(b));
        const auto y=_mm512_xor_si512(_mm512_loadu_si512(a+64),_mm512_loadu_si512(b+64));
        const auto z=_mm512_xor_si512(_mm512_loadu_si512(a+128),_mm512_loadu_si512(b+128));
        const auto w=_mm512_xor_si512(_mm512_loadu_si512(a+192),_mm512_loadu_si512(b+192));
        const auto diff=_mm512_or_si512(_mm512_or_si512(x,y),_mm512_or_si512(z,w));
        if(_mm512_test_epi64_mask(diff,diff))return false;
        a+=256;b+=256;n-=256;
    }
    return Common::ReadbackDetail::EqualSse2(a,b,n);
}
bool Has512() {
    int regs[4];__cpuidex(regs,0,0);if(regs[0]<7)return false;
    __cpuidex(regs,1,0);if((regs[2]&((1<<26)|(1<<27)|(1<<28)))!=((1<<26)|(1<<27)|(1<<28)))return false;
    if((_xgetbv(0)&0xe6)!=0xe6)return false;
    __cpuidex(regs,7,0);return (regs[1]&(1<<16))!=0;
}

template<Equal Compare, bool WholeCopy=true>
size_t CopyChunks(void* dst,const void* src,size_t n) {
    const auto a=reinterpret_cast<uintptr_t>(dst),b=reinterpret_cast<uintptr_t>(src);
    if((a>=b?a-b:b-a)<n){std::memmove(dst,src,n);return n;}
    if constexpr(WholeCopy)if(n<1048576){std::memcpy(dst,src,n);return n;}
    auto* d=static_cast<Byte*>(dst);auto* s=static_cast<const Byte*>(src);size_t written=0;unsigned run=0;
    while(n) {
        const size_t step=std::min(n,size_t{65536});
        if(!Compare(d,s,step)) {
            if(++run==4){std::memcpy(d,s,n);return written+n;}
            std::memcpy(d,s,step);written+=step;
        }else run=0;
        d+=step;s+=step;n-=step;
    }
    return written;
}
// Fuse exact comparison and stores for 128-byte units. Dense differences return
// to memcpy. This needs no reference hash or RAM-write tracking shortcuts.
__declspec(noinline) size_t CopyFine(void* dst,const void* src,size_t n) {
    const auto da=reinterpret_cast<uintptr_t>(dst),sa=reinterpret_cast<uintptr_t>(src);
    if((da>=sa?da-sa:sa-da)<n){std::memmove(dst,src,n);return n;}
    if(n<1048576){std::memcpy(dst,src,n);return n;}
    auto* d=static_cast<Byte*>(dst);auto* s=static_cast<const Byte*>(src);size_t written=0;unsigned run=0;
    while(n>=128) {
        const auto x=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(s));
        const auto y=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(s+32));
        const auto z=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(s+64));
        const auto w=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(s+96));
        const auto a=_mm256_xor_si256(x,_mm256_loadu_si256(reinterpret_cast<const __m256i*>(d)));
        const auto b=_mm256_xor_si256(y,_mm256_loadu_si256(reinterpret_cast<const __m256i*>(d+32)));
        const auto c=_mm256_xor_si256(z,_mm256_loadu_si256(reinterpret_cast<const __m256i*>(d+64)));
        const auto e=_mm256_xor_si256(w,_mm256_loadu_si256(reinterpret_cast<const __m256i*>(d+96)));
        const auto diff=_mm256_or_si256(_mm256_or_si256(a,b),_mm256_or_si256(c,e));
        if(!_mm256_testz_si256(diff,diff)) {
            if(++run==4){std::memcpy(d,s,n);return written+n;}
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(d),x);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(d+32),y);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(d+64),z);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(d+96),w);written+=128;
        }else run=0;
        d+=128;s+=128;n-=128;
    }
    if(n&&!EqualLibc(d,s,n)){std::memcpy(d,s,n);written+=n;}
    return written;
}

// One persistent helper, and the calling thread, with completion inside every
// copy. No deferred visibility and no allocation in the timed path.
class ParallelCopy {
    std::atomic<unsigned> epoch{};std::atomic<bool> done{},stop{};
    Byte* dst{};const Byte* src{};size_t size{},written{};
    std::thread worker;
public:
    ParallelCopy():worker{[this] {
        unsigned seen=0;
        for(;;) {
            epoch.wait(seen,std::memory_order_acquire);seen=epoch.load(std::memory_order_acquire);
            if(stop.load(std::memory_order_relaxed))break;
            written=Common::CopyChangedMemory(dst,src,size);
            done.store(true,std::memory_order_release);done.notify_one();
        }
    }} {}
    ~ParallelCopy(){stop.store(true,std::memory_order_relaxed);epoch.fetch_add(1,std::memory_order_release);epoch.notify_one();worker.join();}
    size_t Run(void* destination,const void* source,size_t bytes) {
        const auto a=reinterpret_cast<uintptr_t>(destination),b=reinterpret_cast<uintptr_t>(source);
        if(bytes<2097152||(a>=b?a-b:b-a)<bytes)return Common::CopyChangedMemory(destination,source,bytes);
        const size_t half=(bytes/2)&~size_t{65535};
        dst=static_cast<Byte*>(destination)+half;src=static_cast<const Byte*>(source)+half;size=bytes-half;
        done.store(false,std::memory_order_relaxed);epoch.fetch_add(1,std::memory_order_release);epoch.notify_one();
        const auto first=Common::CopyChangedMemory(destination,source,half);
        done.wait(false,std::memory_order_acquire);return first+written;
    }
};

class MultiCopy : public Common::ReadbackCopyWorkers {
public:
    explicit MultiCopy(unsigned count):ReadbackCopyWorkers(count){}
    size_t Run(void* d,const void* s,size_t n){return Copy(d,s,n);}
};

struct Strategy {const char* name;Copy copy;Equal equal;unsigned parallel{};};
std::vector<Strategy> Strategies() {
    std::vector<Strategy> strategies{{"v9",Common::CopyChangedMemory,Common::ReadbackMemoryEqual},
        {"libc",CopyChunks<EqualLibc>,EqualLibc}};
    if(Common::ReadbackDetail::HasAvx2()) {
        strategies.push_back({"xor128",CopyChunks<EqualXor<128,false>>,EqualXor<128,false>});
        strategies.push_back({"xor512",CopyChunks<EqualXor<512,false>>,EqualXor<512,false>});
        strategies.push_back({"prefetch512",CopyChunks<EqualXor<512,true>>,EqualXor<512,true>});
        strategies.push_back({"fine128",CopyFine,EqualXor<128,false>});
    }
    if(Has512())strategies.push_back({"avx512",CopyChunks<Equal512>,Equal512});
    strategies.push_back({"two_threads",nullptr,nullptr,2});
    strategies.push_back({"three_threads",nullptr,nullptr,3});
    strategies.push_back({"four_threads",nullptr,nullptr,4});return strategies;
}
void Verify(const std::vector<Strategy>& strategies) {
    ParallelCopy parallel;MultiCopy triple(3),quad(4);std::mt19937 rng{0x98005070};
    const auto run=[&](const Strategy& strategy,void* d,const void* s,size_t n){return strategy.parallel==4?quad.Run(d,s,n):strategy.parallel==3?triple.Run(d,s,n):strategy.parallel?parallel.Run(d,s,n):strategy.copy(d,s,n);};
    for(const auto& strategy:strategies) {
        std::vector<Byte> a(2097152+1024),b(a.size()),expected(a.size());
        for(unsigned round=0;round<50;++round) {
            const size_t n=round%5==0?0:round%5==1?513:round%5==2?1048576:2097152+23;
            const size_t x=rng()%64,y=rng()%64;
            for(size_t i=0;i<a.size();++i)a[i]=static_cast<Byte>(i*29+round);
            b=a;for(unsigned k=0;k<50;++k)b[rng()%b.size()]^=0xA5;
            expected=b;std::memcpy(expected.data()+y,a.data()+x,n);
            run(strategy,b.data()+y,a.data()+x,n);
            if(b!=expected)throw std::runtime_error("strategy copy oracle failed");
        }
        if(strategy.equal) {
            for(size_t n=0;n<1025;++n)for(size_t shift:{0ULL,1ULL,7ULL,31ULL,63ULL}) {
                std::fill_n(a.data()+shift,n,0x3C);std::fill_n(b.data()+shift,n,0x3C);
                if(!strategy.equal(a.data()+shift,b.data()+shift,n))throw std::runtime_error("equality oracle failed");
                for(size_t i=0;i<n;++i) {
                    b[shift+i]^=1;if(strategy.equal(a.data()+shift,b.data()+shift,n))throw std::runtime_error("changed byte missed");b[shift+i]^=1;
                }
            }
        }
        auto* p=static_cast<Byte*>(VirtualAlloc(nullptr,2097152+8192,MEM_RESERVE,PAGE_NOACCESS));
        auto* q=static_cast<Byte*>(VirtualAlloc(nullptr,2097152+8192,MEM_RESERVE,PAGE_NOACCESS));
        if(!p||!q||!VirtualAlloc(p+4096,2097152,MEM_COMMIT,PAGE_READWRITE)||!VirtualAlloc(q+4096,2097152,MEM_COMMIT,PAGE_READWRITE))throw std::runtime_error("guard allocation failed");
        std::memset(p+4096,0x3C,2097152);std::memset(q+4096,0x3C,2097152);q[4096+2097152-1]^=1;
        run(strategy,q+4096,p+4096,2097152);
        if(std::memcmp(p+4096,q+4096,2097152))throw std::runtime_error("guarded copy failed");
        VirtualFree(p,0,MEM_RELEASE);VirtualFree(q,0,MEM_RELEASE);
        std::cout<<"PASS strategy_oracles_tail_guard_alignment_every_byte mode="<<strategy.name<<'\n';
    }
}
void Benchmark(const std::vector<Strategy>& strategies) {
    ParallelCopy parallel;MultiCopy triple(3),quad(4);constexpr size_t copy_size=2097152,volume=512ULL<<20;
    for(size_t working_set:{16ULL<<20,64ULL<<20,128ULL<<20,256ULL<<20})for(unsigned pattern:{0u,1u,2u}) {
        std::vector<Byte> reference(working_set,0x3C),guest(working_set,0x3C);
        std::vector<std::vector<double>> samples(strategies.size());
        std::vector<size_t> writes(strategies.size());
        for(unsigned round=0;round<9;++round)for(size_t order=0;order<strategies.size();++order) {
            const size_t mode=(round+order)%strategies.size();const auto& strategy=strategies[mode];
            std::fill(guest.begin(),guest.end(),0x3C);size_t written=0;
            const auto start=std::chrono::steady_clock::now();
            for(size_t slot=0;slot<volume/copy_size;++slot) {
                const size_t offset=(slot*copy_size)%working_set;auto* dst=guest.data()+offset;auto* src=reference.data()+offset;
                if(pattern==1)for(size_t chunk=0;chunk<copy_size;chunk+=1048576)dst[chunk+slot%256]^=1;
                if(pattern==2)std::memset(dst,0x55,copy_size);
                written+=strategy.parallel==4?quad.Run(dst,src,copy_size):strategy.parallel==3?triple.Run(dst,src,copy_size):strategy.parallel?parallel.Run(dst,src,copy_size):strategy.copy(dst,src,copy_size);
            }
            const auto end=std::chrono::steady_clock::now();observed=written;
            if(guest!=reference)throw std::runtime_error("timed strategy payload oracle failed");
            if(round>=2)samples[mode].push_back(std::chrono::duration<double,std::milli>(end-start).count());
            writes[mode]=written;
        }
        for(size_t mode=0;mode<strategies.size();++mode) {
            auto values=samples[mode];std::sort(values.begin(),values.end());
            std::cout<<"BENCH host_reference working_MiB="<<(working_set>>20)<<" pattern="<<pattern<<" mode="<<strategies[mode].name
                     <<" logical_MiB=512 median_ms="<<values[values.size()/2]<<" written_bytes="<<writes[mode]<<" samples=7 PASS_payload\n";
        }
    }
}
int main()try{const auto strategies=Strategies();Verify(strategies);Benchmark(strategies);return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
