// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <limits>
#include <random>
#include <thread>
#include <vector>
#include "yuzu_video_core/gpu_write_history.h"

unsigned checks{},failures{};
void Check(const char* name,bool okay){++checks;failures+=!okay;std::cout<<(okay?"PASS ":"FAIL ")<<name<<'\n';}
volatile bool observed{};
template<class History> void Benchmark(const char* name) {
    History history; history.Mark(0x12340000,8*1024*1024);
    std::vector<double> times;
    for(unsigned trial=0;trial<7;++trial) {
        const auto start=std::chrono::steady_clock::now();
        for(unsigned n=0;n<100000;++n) history.Mark(0x12340000,8*1024*1024);
        const double ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/100000;
        if(trial>=2)times.push_back(ns);
    }
    std::sort(times.begin(),times.end());
    const auto start=std::chrono::steady_clock::now();
    for(unsigned n=0;n<1000000;++n) observed=history.MayHaveWrites(0x70000000+(n%64)*4096,4096);
    std::cout<<"BENCH "<<name<<" repeated_8MiB_mark_ns="<<times[2]<<" negative_query_ns="
             <<std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/1000000<<'\n';
}
int main(int argc,char**) {
    using History=VideoCommon::GpuWriteHistory<24,16>;
    History history;
    Check("empty_history_proves_never_written",!history.MayHaveWrites(65536,1));
    history.Mark(65535,2);
    Check("unaligned_cross_page_write_is_published_on_both_pages",history.MayHaveWrites(65535,1)&&history.MayHaveWrites(65536,1));
    Check("nonwritten_pages_remain_negative",!history.MayHaveWrites(131072,1));
    history.Mark((1ULL<<24)-1,1);
    Check("last_valid_byte_is_representable",history.MayHaveWrites((1ULL<<24)-1,1));
    Check("zero_size_does_not_publish_or_query",!history.MayHaveWrites(std::numeric_limits<uint64_t>::max(),0));
    Check("unsupported_ranges_are_conservative",history.MayHaveWrites(1ULL<<24,1)&&history.MayHaveWrites(std::numeric_limits<uint64_t>::max()-1,4));
    history.Mark(0,1ULL<<24);
    Check("full_words_and_full_domain_do_not_shift_by_64",history.MayHaveWrites(0,1)&&history.MayHaveWrites(1ULL<<23,1));
    History unsupported;unsupported.Mark(std::numeric_limits<uint64_t>::max()-1,4);
    Check("invalid_publication_disables_negative_proofs",unsupported.MayHaveWrites(0,1));
    History random;std::vector<bool> pages(256);std::mt19937 rng{0xCACE};bool okay=true;
    for(unsigned trial=0;trial<20000;++trial) {
        const uint64_t address=rng()%(1<<24),size=std::min<uint64_t>(rng()%1000000,(1<<24)-address);
        random.Mark(address,size);
        if(size)for(uint64_t p=address>>16;p<=((address+size-1)>>16);++p)pages[p]=true;
        const uint64_t query=rng()%(1<<24),bytes=std::min<uint64_t>(rng()%200000,(1<<24)-query);
        bool expected=false;if(bytes)for(uint64_t p=query>>16;p<=((query+bytes-1)>>16);++p)expected=expected||pages[p];
        okay&=random.MayHaveWrites(query,bytes)==expected;
    }
    Check("20000_publications_and_queries_match_independent_page_oracle",okay);
    History concurrent;std::atomic<unsigned> published{};std::atomic<bool> visibility{true};
    std::thread writer{[&]{for(unsigned p=0;p<256;++p){concurrent.Mark(static_cast<uint64_t>(p)<<16,1);published.store(p+1,std::memory_order_release);}}};
    std::thread reader{[&]{for(unsigned p=0;p<256;++p){while(published.load(std::memory_order_acquire)<=p)std::this_thread::yield();if(!concurrent.MayHaveWrites(static_cast<uint64_t>(p)<<16,1))visibility=false;}}};
    writer.join();reader.join();Check("publication_before_dirty_exposure_has_acquire_visibility",visibility);
    History multi;std::vector<std::thread> writers;
    for(unsigned t=0;t<4;++t)writers.emplace_back([&,t]{for(unsigned p=t;p<256;p+=4)for(unsigned repeat=0;repeat<50;++repeat)multi.Mark(static_cast<uint64_t>(p)<<16,1);});
    for(auto& t:writers)t.join();
    okay=true;
    for(unsigned p=0;p<256;++p)okay&=multi.MayHaveWrites(static_cast<uint64_t>(p)<<16,1);
    Check("concurrent_disjoint_publications_in_shared_words_are_not_lost",okay);
    // No cancellation/deletion/reset API exists: the proof intentionally stays
    // conservative even when the caller removes or reuses a GPU resource.
    Check("old_write_remains_positive_after_resource_reuse",concurrent.MayHaveWrites(0,1));
    std::cout<<"RESULT checks="<<checks<<" failed="<<failures<<'\n';
    if(!failures&&argc>1) {
        Benchmark<VideoCommon::GpuWriteHistory<34,12,false>>("4KiB_always_RMW");
        Benchmark<VideoCommon::GpuWriteHistory<34,16,false>>("64KiB_always_RMW");
        Benchmark<VideoCommon::GpuWriteHistory<34,16,true>>("64KiB_skip_published");
    }
    return failures?1:0;
}
