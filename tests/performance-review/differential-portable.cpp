// Platform-independent correctness fixture, also runnable under AArch64 QEMU.
#include <algorithm>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
#include "yuzu_common/differential_memory_copy.h"

int main() {
    using namespace Common;
    std::vector<ReadbackDetail::Comparer> comparers{ReadbackDetail::EqualPortable,
                                                  ReadbackMemoryEqual};
#if defined(__aarch64__)
    comparers.push_back(ReadbackDetail::EqualNeon);
    std::cout << "CPU ARM64 NEON\n";
#elif defined(__x86_64__)
    comparers.push_back(ReadbackDetail::EqualSse2);
    if (ReadbackDetail::HasAvx2()) comparers.push_back(ReadbackDetail::EqualAvx2);
    std::cout << "CPU x64 AVX2=" << ReadbackDetail::HasAvx2() << '\n';
#endif
    for (auto compare : comparers) {
        for (size_t size = 0; size <= 513; ++size) {
            for (size_t shift = 0; shift < 64; ++shift) {
                std::vector<unsigned char> a(size + 128, 0x39), b(a);
                if (!compare(a.data()+shift,b.data()+shift,size)) return 1;
                for (size_t byte = 0; byte < size; ++byte) {
                    b[shift+byte] ^= 1;
                    if (compare(a.data()+shift,b.data()+shift,size)) return 2;
                    b[shift+byte] ^= 1;
                }
            }
        }
    }
    std::cout << "PASS all_backends_every_byte_alignment_and_tail\n";
    std::mt19937 rng(0x20261012);
    for (unsigned round=0; round<100; ++round) {
        const size_t size=(round%4==0?4096:round%4==1?1048575:round%4==2?1048576:2097215);
        std::vector<unsigned char> src(size+128), dst(size+128);
        for (size_t i=0; i<src.size(); ++i) src[i]=static_cast<unsigned char>(rng());
        dst=src;
        if (round%3) for (unsigned byte=0; byte<50; ++byte) dst[rng()%dst.size()]^=1;
        auto expected=dst;
        const size_t a=rng()%64,b=rng()%64;
        std::memcpy(expected.data()+a,src.data()+b,size);
        CopyChangedMemory(dst.data()+a,src.data()+b,size);
        if (dst!=expected) return 3;
    }
    std::cout << "PASS random_differential_copy_matches_independent_full_copy\n";
    const size_t page=static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const size_t bytes=2*1024*1024;
    auto* a=static_cast<unsigned char*>(mmap(nullptr,bytes+2*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    auto* b=static_cast<unsigned char*>(mmap(nullptr,bytes+2*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if (a==MAP_FAILED||b==MAP_FAILED||mprotect(a+page,bytes,PROT_READ|PROT_WRITE)||mprotect(b+page,bytes,PROT_READ|PROT_WRITE)) return 4;
    std::memset(a+page,0x39,bytes);std::memset(b+page,0x39,bytes);
    for (auto compare : comparers) for (size_t size=0;size<=513;++size) {
        auto* left=a+page+bytes-size;auto* right=b+page+bytes-size;
        if (!compare(left,right,size)) return 5;
        if (size) {right[size-1]^=1;if(compare(left,right,size))return 6;right[size-1]^=1;}
    }
    b[page+bytes-1]^=1;
    CopyChangedMemory(b+page,a+page,bytes);
    if (std::memcmp(a+page,b+page,bytes)) return 7;
    munmap(a,bytes+2*page);munmap(b,bytes+2*page);
    std::cout << "PASS guard_pages_for_every_backend_and_final_changed_byte\n";
    return 0;
}
