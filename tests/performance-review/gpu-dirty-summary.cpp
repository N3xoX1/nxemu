#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
#include "yuzu_common/range_sets.inc"
#include "yuzu_video_core/gpu_dirty_summary.h"
unsigned checks{}, failures{};
void Check(const char* name,bool okay){++checks;failures+=!okay;std::cout<<(okay?"PASS ":"FAIL ")<<name<<'\n';}
template<class Summary> void Refresh(Summary& summary,Common::RangeSet<u64>& dirty,u64 address,u64 size){
    summary.Refresh(address,size,[&](u64 base,u64 amount,auto&& visitor){dirty.ForEachInRange(base,amount,visitor);});
}
int main()try {
    const char* option=std::getenv("NXEMU_CURRENT_GPU_DIRTY_FILTER");
    const bool enabled=option && option[0]=='1' && option[1]=='\0';
    Check("factory_requires_explicit_opt_in",bool(VideoCommon::MakeGpuDirtySummary())==enabled);
    using Summary=VideoCommon::GpuDirtySummary<>;
    auto summary=std::make_unique<Summary>();Common::RangeSet<u64> dirty;
    constexpr u64 page=65536,word=64*page,limit=1ULL<<34;
    Check("initially_clean_and_zero_size",!summary->MayHaveWrites(0,limit)&&!summary->MayHaveWrites(~u64{0},0));
    summary->Mark(word-1,2);dirty.Add(word-1,2);
    Check("both_sides_of_word_boundary_are_dirty",summary->MayHaveWrites(word-1,1)&&summary->MayHaveWrites(word,1));
    dirty.Subtract(word-1,1);Refresh(*summary,dirty,word-1,1);
    Check("refresh_does_not_clear_neighbour_word",!summary->MayHaveWrites(word-1,1)&&summary->MayHaveWrites(word,1));
    dirty.Subtract(word,1);Refresh(*summary,dirty,word,1);
    Check("complete_retirement_clears_current_dirty_pages",!summary->MayHaveWrites(word,1));
    summary->Mark(0,word);dirty.Add(0,word);
    dirty.Subtract(0,word);dirty.Add(page+23,1);dirty.Add(63*page+9,1);
    Refresh(*summary,dirty,0,word);
    Check("newer_pending_epoch_and_alias_bytes_survive_refresh",summary->MayHaveWrites(page+23,1)&&summary->MayHaveWrites(63*page+9,1));
    Check("clean_pages_in_same_word_can_bypass",!summary->MayHaveWrites(2*page,61*page));
    dirty.Add(3*page+100,7);summary->Mark(3*page+100,7);
    dirty.Subtract(3*page+100,6);Refresh(*summary,dirty,3*page+100,6);
    Check("partial_page_dirty_byte_is_not_cleared",summary->MayHaveWrites(3*page+106,1));
    dirty.Subtract(3*page+106,1);Refresh(*summary,dirty,3*page+106,1);
    Check("cancelled_or_retired_bytes_clear_only_when_page_is_clean",!summary->MayHaveWrites(3*page,1));
    summary->Mark(limit-1,1);dirty.Add(limit-1,1);Refresh(*summary,dirty,limit-1,1);
    Check("last_supported_byte_and_high_bit",summary->MayHaveWrites(limit-1,1));
    dirty.Subtract(limit-1,1);Refresh(*summary,dirty,limit-1,1);
    Check("last_supported_byte_can_clear",!summary->MayHaveWrites(limit-1,1));
    Check("unsupported_or_overflow_query_is_conservative",summary->MayHaveWrites(limit,1)&&summary->MayHaveWrites(~u64{0},2));
    summary->Mark(limit,1);dirty.Clear();Refresh(*summary,dirty,0,limit);
    Check("unsupported_write_is_sticky_conservative",summary->MayHaveWrites(0,1));

    using Small=VideoCommon::GpuDirtySummary<20,10>;
    auto random=std::make_unique<Small>();Common::RangeSet<u64> ranges;std::vector<unsigned char> oracle(1<<20);
    std::mt19937 rng{0xD17F};bool complete=true,can_skip=false;
    for(unsigned iteration=0;iteration<5000;++iteration){
        const u64 start=rng()%oracle.size(),length=std::min<u64>(1+rng()%4096,oracle.size()-start);
        if(iteration%3){random->Mark(start,length);ranges.Add(start,length);std::fill_n(oracle.begin()+start,length,1);}
        else {ranges.Subtract(start,length);std::fill_n(oracle.begin()+start,length,0);Refresh(*random,ranges,start,length);}
        for(unsigned probe=0;probe<8;++probe){const u64 a=rng()%oracle.size(),n=std::min<u64>(1+rng()%2048,oracle.size()-a);
            const bool exact=std::find(oracle.begin()+a,oracle.begin()+a+n,1)!=oracle.begin()+a+n;
            const bool candidate=random->MayHaveWrites(a,n);complete&=!exact||candidate;can_skip|=!candidate;}
    }
    Check("40000_independent_byte_oracles_have_no_false_negatives",complete);
    Check("random_clean_queries_produce_actual_negatives",can_skip);

    for(bool early_clear:{false,true}){
        auto publication=std::make_unique<Summary>();Common::RangeSet<u64> remaining;
        publication->Mark(0,1);unsigned guest=17,observed=0;std::atomic<bool> read_done{};
        std::thread reader{[&]{while(publication->MayHaveWrites(0,1))std::this_thread::yield();observed=guest;read_done.store(true,std::memory_order_release);read_done.notify_one();}};
        if(!early_clear)guest=99;
        Refresh(*publication,remaining,0,1);
        if(early_clear){read_done.wait(false,std::memory_order_acquire);guest=99;}
        reader.join();
        Check(early_clear?"early_clear_negative_control_exposes_old_RAM_without_data_race":"negative_acquire_observes_completed_RAM_copy",observed==(early_clear?17u:99u));
    }
    // A clean query does not acquire the cache lock; a positive still must wait.
    std::mutex cache_lock;summary=std::make_unique<Summary>();summary->Mark(page,1);
    cache_lock.lock();std::atomic<bool> positive_entered{},positive_done{};
    std::thread positive{[&]{positive_entered.store(true,std::memory_order_release);positive_entered.notify_one();
        if(summary->MayHaveWrites(page,1)){std::lock_guard guard(cache_lock);}positive_done.store(true,std::memory_order_release);}};
    positive_entered.wait(false,std::memory_order_acquire);
    Check("clean_query_skips_held_cache_mutex",!summary->MayHaveWrites(0,1));
    Check("positive_query_retains_held_cache_mutex",!positive_done.load(std::memory_order_acquire));
    cache_lock.unlock();positive.join();Check("positive_query_completes_after_unlock",positive_done.load());

    // Measure summary refresh cost separately from any hypothetical saved waits.
    summary=std::make_unique<Summary>();dirty.Clear();dirty.Add(123*page+17,1);
    constexpr u64 amount=512ULL<<20;constexpr unsigned rounds=100;
    const auto begin=std::chrono::steady_clock::now();
    for(unsigned i=0;i<rounds;++i){summary->Mark(0,amount);Refresh(*summary,dirty,0,amount);}
    const auto end=std::chrono::steady_clock::now();
    Check("large_refresh_preserves_small_remaining_dirty_interval",summary->MayHaveWrites(123*page+17,1)&&!summary->MayHaveWrites(0,page));
    std::cout<<"BENCH logical_MiB=512 mark_and_refresh_us="<<std::chrono::duration<double,std::micro>(end-begin).count()/rounds<<" rounds=100\n";
    std::cout<<"RESULT checks="<<checks<<" failed="<<failures<<'\n';return failures?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
