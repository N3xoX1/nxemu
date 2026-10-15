#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>
#include "yuzu_common/parallel_readback_copy.h"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif
unsigned failures{},checks{};
void Check(const char* name,bool okay){++checks;failures+=!okay;std::cout<<(okay?"PASS ":"FAIL ")<<name<<'\n';}
int main()try {
    for (unsigned cpus : {0U, 1U, 2U, 3U, 4U, 6U, 7U, 8U, 16U, 64U}) {
        const unsigned expected = cpus >= 8 ? 4 : cpus >= 4 ? 2 : 0;
        Check("automatic_worker_budget_reserves_emulation_capacity",
              Common::ReadbackWorkerCountForHardware(nullptr, cpus) == expected);
        for (const char* invalid : {"", "0", "1", "8", "4x", "24", "auto"}) {
            Check("disabled_or_invalid_worker_override_is_serial",
                  Common::ReadbackWorkerCountForHardware(invalid, cpus) == 0);
        }
        Check("explicit_two_workers_respects_capacity",
              Common::ReadbackWorkerCountForHardware("2", cpus) == (cpus >= 4 ? 2U : 0U));
        Check("explicit_four_workers_respects_capacity",
              Common::ReadbackWorkerCountForHardware("4", cpus) == (cpus >= 6 ? 4U : 0U));
    }
    using Byte=unsigned char;constexpr size_t maximum=8388608+63;
    std::vector<Byte> src(maximum+256),dst(src.size()),expected(src.size());std::mt19937 rng{0xC0A4};
    for(unsigned count:{1u,2u,4u}) {
        Common::ReadbackCopyWorkers workers(count);bool correct=true;
        for(unsigned round=0;round<36;++round) {
            for(size_t i=0;i<src.size();++i)src[i]=static_cast<Byte>(i*29+round);
            dst=src;for(unsigned i=0;i<60;++i)dst[rng()%dst.size()]^=0xB7;
            const size_t n=std::array<size_t,9>{0,127,513,1048575,1048576,2097151,2097152,2097175,maximum}[round%9];
            const size_t a=rng()%64,b=rng()%64;expected=dst;
            std::memcpy(expected.data()+b,src.data()+a,n);workers.Copy(dst.data()+b,src.data()+a,n);correct&=dst==expected;
        }
        Check("worker_sizes_alignments_sparse_changes_and_tails_match_full_byte_oracle",correct);
        dst=src;Check("worker_equal_large_span_writes_no_bytes",workers.Copy(dst.data(),src.data(),maximum)==0&&dst==src);
        for(size_t i=0;i<maximum;i+=65536)dst[i]^=0xFF;
        workers.Copy(dst.data(),src.data(),maximum);Check("worker_dense_changes_match_full_copy",dst==src);
        for(int direction:{-1,0,1}) {
            dst=src;expected=src;const size_t n=maximum-128;
            std::memmove(expected.data()+128+direction*31,expected.data()+128,n);
            workers.Copy(dst.data()+128+direction*31,dst.data()+128,n);
            Check("worker_overlap_and_identical_pointer_match_memmove",dst==expected);
        }
    }
    dst.assign(dst.size(),0);std::atomic<bool> ready{};Common::ReadbackCopyWorkers workers(4);
    std::thread consumer{[&]{ready.wait(false,std::memory_order_acquire);Check("worker_return_publishes_all_partitions_to_CPU_consumer",std::memcmp(dst.data(),src.data(),maximum)==0);}};
    std::thread producer{[&]{workers.Copy(dst.data(),src.data(),maximum);ready.store(true,std::memory_order_release);ready.notify_one();}};
    producer.join();consumer.join();
    bool burst_correct=true;
    for(unsigned epoch=0;epoch<96;++epoch) {
        if(epoch%12==0)std::this_thread::sleep_for(std::chrono::milliseconds(2));
        for(size_t partition=0;partition<4;++partition)src[partition*524288+epoch]^=0x5A;
        workers.Copy(dst.data(),src.data(),2097152);
        burst_correct&=std::memcmp(dst.data(),src.data(),2097152)==0;
    }
    Check("burst_and_idle_wakeup_complete_every_partition_before_return",burst_correct);
    // A destruction without any submitted jobs must also wake and join helpers
    // that have already left the initial wait and parked.
    {Common::ReadbackCopyWorkers idle_workers(4);std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    Check("parked_helpers_stop_and_join_without_a_copy",true);
#ifdef _WIN32
    const auto process_cpu=[] {
        FILETIME created{},exited{},kernel{},user{};
        if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))throw std::runtime_error("process timing");
        const auto ticks=[](FILETIME value){return (static_cast<unsigned long long>(value.dwHighDateTime)<<32)|value.dwLowDateTime;};
        return ticks(kernel)+ticks(user);
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto idle_begin=process_cpu();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto idle_ticks=process_cpu()-idle_begin;
    std::cout<<"IDLE process_cpu_ms="<<idle_ticks/10000.0<<" wall_ms=200\n";
    Check("idle_helpers_remain_parked_instead_of_spinning_forever",idle_ticks<500000);
#endif
    const size_t allocation=2097152+8192;
#ifdef _WIN32
    auto* a=static_cast<Byte*>(VirtualAlloc(nullptr,allocation,MEM_RESERVE,PAGE_NOACCESS));
    auto* b=static_cast<Byte*>(VirtualAlloc(nullptr,allocation,MEM_RESERVE,PAGE_NOACCESS));
    if(!a||!b||!VirtualAlloc(a+4096,2097152,MEM_COMMIT,PAGE_READWRITE)||!VirtualAlloc(b+4096,2097152,MEM_COMMIT,PAGE_READWRITE))throw std::runtime_error("guard allocation");
#else
    auto* a=static_cast<Byte*>(mmap(nullptr,allocation,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    auto* b=static_cast<Byte*>(mmap(nullptr,allocation,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(a==MAP_FAILED||b==MAP_FAILED||mprotect(a+4096,2097152,PROT_READ|PROT_WRITE)||mprotect(b+4096,2097152,PROT_READ|PROT_WRITE))throw std::runtime_error("guard allocation");
#endif
    std::memset(a+4096,0x3C,2097152);std::memset(b+4096,0x3C,2097152);b[4096+2097152-1]^=1;
    workers.Copy(b+4096,a+4096,2097152);
    Check("worker_last_changed_byte_at_guard_has_exact_bounds",std::memcmp(a+4096,b+4096,2097152)==0);
#ifdef _WIN32
    VirtualFree(a,0,MEM_RELEASE);VirtualFree(b,0,MEM_RELEASE);
#else
    munmap(a,allocation);munmap(b,allocation);
#endif
    bool used=true;dst=src;Common::CopyChangedMemoryForReadback(dst.data(),src.data(),maximum,&used);
    Check("unscoped_caller_keeps_serial_path",!used&&dst==src);
    Common::ReadbackCopyContext readback_context;
    used=false;Common::CopyChangedMemoryForReadback(dst.data(),src.data(),maximum,&used);
    Check("configured_worker_activation_matches_supported_thread_count",used==(Common::ReadbackWorkerCount()!=0)&&dst==src);
    used=true;Common::CopyChangedMemoryForReadback(dst.data(),src.data(),127,&used);
    Check("configured_small_span_keeps_serial_path",!used);
    used=true;Common::CopyChangedMemoryForReadback(dst.data(),dst.data(),maximum,&used);
    Check("configured_overlap_keeps_serial_path",!used);
    {Common::ReadbackCopyContext nested;Check("nested_context_is_distinct",Common::ReadbackCopyContext::Current()==&nested);}
    Check("nested_context_restores_owner",Common::ReadbackCopyContext::Current()==&readback_context);
    bool exited=false;
    std::thread scoped_caller{[&]{Common::ReadbackCopyContext context;Common::CopyChangedMemoryForReadback(dst.data(),src.data(),maximum);exited=true;}};
    scoped_caller.join();Check("scoped_worker_caller_exits_and_joins",exited&&dst==src);
    std::cout<<"RESULT checks="<<checks<<" failed="<<failures<<" configured_threads="<<Common::ReadbackWorkerCount()<<'\n';
    return failures?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
