#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <vector>
#include "yuzu_video_core/buffer_cache/usage_tracker.h"
#include "yuzu_video_core/buffer_cache/word_manager.h"
#include "baseline-usage.h"
#include "baseline-word.h"
#include "yuzu_video_core/continuous_range_cache.h"
#include "yuzu_video_core/flush_region_cache_filter.h"

int failures{}, checks{};
volatile u64 result_sink{};
void Check(const char* name, bool valid) {
    ++checks; failures += !valid;
    std::cout << (valid ? "PASS " : "FAIL ") << name << '\n';
}
template<class F> void Bench(const char* name, size_t count, F operation) {
    const auto start=std::chrono::steady_clock::now();
    for(size_t i=0;i<count;++i) operation(i);
    const auto ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/count;
    std::cout << "BENCH " << name << " ns/op=" << ns << '\n';
}
template<class T> __declspec(noinline) void Track(T& tracker,u64 offset,u64 size) { tracker.Track(offset,size); }

void UsageOracle() {
    constexpr u64 size=65536;
    VideoCommon::UsageTracker current(size); BaselineUsage::UsageTracker original(size);
    std::array<bool,size/64> bits{}; std::mt19937 rng(0x16F05);
    bool valid=true;
    for(size_t i=0;i<50000;++i) {
        if(i%47==0) {current.Reset();original.Reset();bits.fill(false);}
        u64 offset=rng()%size, bytes=std::min<u64>(rng()%20000,size-offset);
        Track(current,offset,bytes);Track(original,offset,bytes);
        if(bytes) for(u64 j=offset/64;j<=(offset+bytes-1)/64;++j) bits[j]=true;
        for(size_t q=0;q<8;++q) {
            offset=rng()%size;bytes=std::min<u64>(rng()%2048,size-offset);
            bool expected=false;
            if(bytes) for(u64 j=offset/64;j<=(offset+bytes-1)/64;++j) expected|=bits[j];
            valid &= current.IsUsed(offset,bytes)==expected && current.IsUsed(offset,bytes)==original.IsUsed(offset,bytes);
        }
    }
    Check("usage_50000_updates_400000_queries_match_byte_block_oracle_and_baseline",valid);
    current.Reset();current.Track(4096,4096);current.Track(0,64);current.Track(8192,4096);
    Check("disjoint_usage_keeps_hole",!current.IsUsed(64,4032));
    current.Track(64,4032);Check("adjacent_ranges_preserve_usage",current.IsUsed(64,4032));
    current.Reset();Check("reset_invalidates_cached_coverage",!current.IsUsed(0,size));
    current.Track(4096,4096);Check("rebind_after_reset_repopulates_bitmap",current.IsUsed(4096,4096));
    for (auto range:std::array<std::pair<u64,u64>,4>{{{0,0},{size,65536},{~u64{0}-2,8},{0,~u64{0}}}}) {
        current.Reset();original.Reset();current.Track(range.first,range.second);original.Track(range.first,range.second);
        bool equal=true;for(u64 offset=0;offset<size;offset+=64) equal &= current.IsUsed(offset,1)==original.IsUsed(offset,1);
        Check("invalid_or_empty_range_matches_baseline",equal);
    }
    for(u64 bytes:{8ULL<<20,32ULL<<20,128ULL<<20}) {
        VideoCommon::UsageTracker next(bytes);BaselineUsage::UsageTracker old(bytes);
        const auto prefix=std::to_string(bytes>>20)+"MiB_";
        Bench((prefix+"usage_baseline_rebind").c_str(),2000,[&](size_t){Track(old,0,bytes);});
        Bench((prefix+"usage_current_rebind").c_str(),2000,[&](size_t){Track(next,0,bytes);});
        Check("large_repeat_preserves_last_block",next.IsUsed(bytes-1,1));
    }
}

struct DeviceTracker {
    std::array<int,1024> counts{};
    size_t notifications{};
    void UpdatePagesCachedCount(u64 address,u64 size,int delta) {
        ++notifications;for(u64 page=address/4096;page<(address+size)/4096;++page) counts.at(page)+=delta;
    }
};
void WordOracle() {
    DeviceTracker next_device,old_device;
    VideoCommon::WordManager<DeviceTracker,16> next(0,next_device,4ULL<<20);
    BaselineWord::WordManager<DeviceTracker,16> old(0,old_device,4ULL<<20);
    std::mt19937 rng(0x14CA5); bool valid=true;
    using Ranges=std::vector<std::pair<u64,u64>>;
    for(size_t i=0;i<20000;++i) {
        const u64 start=rng()%(4ULL<<20),size=std::min<u64>(rng()%(2ULL<<20),(4ULL<<20)-start);
        switch(rng()%9) {
        case 0: next.ChangeRegionState<VideoCommon::Type::CPU,true>(start,size);old.ChangeRegionState<BaselineWord::Type::CPU,true>(start,size);break;
        case 1: next.ChangeRegionState<VideoCommon::Type::CPU,false>(start,size);old.ChangeRegionState<BaselineWord::Type::CPU,false>(start,size);break;
        case 2: next.ChangeRegionState<VideoCommon::Type::CachedCPU,true>(start,size);old.ChangeRegionState<BaselineWord::Type::CachedCPU,true>(start,size);break;
        case 3: next.FlushCachedWrites();old.FlushCachedWrites();break;
        case 4: next.ChangeRegionState<VideoCommon::Type::GPU,true>(start,size);old.ChangeRegionState<BaselineWord::Type::GPU,true>(start,size);break;
        case 5: {
            Ranges a,b;
            next.ForEachModifiedRange<VideoCommon::Type::CachedCPU,true>(start,size,[&](u64 x,u64 y){a.emplace_back(x,y);});
            old.ForEachModifiedRange<BaselineWord::Type::CachedCPU,true>(start,size,[&](u64 x,u64 y){b.emplace_back(x,y);});
            valid &= a==b;break;
        }
        case 6: {
            Ranges a,b;
            next.ForEachModifiedRange<VideoCommon::Type::GPU,true>(start,size,[&](u64 x,u64 y){a.emplace_back(x,y);});
            old.ForEachModifiedRange<BaselineWord::Type::GPU,true>(start,size,[&](u64 x,u64 y){b.emplace_back(x,y);});
            valid &= a==b;break;
        }
        default: {
            Ranges a,b;
            next.ForEachModifiedRange<VideoCommon::Type::CPU,true>(start,size,[&](u64 x,u64 y){a.emplace_back(x,y);});
            old.ForEachModifiedRange<BaselineWord::Type::CPU,true>(start,size,[&](u64 x,u64 y){b.emplace_back(x,y);});
            valid &= a==b;break;
        }}
        valid &= next_device.counts==old_device.counts && next_device.notifications==old_device.notifications;
        valid &= next.IsRegionModified<VideoCommon::Type::CPU>(start,size)==old.IsRegionModified<BaselineWord::Type::CPU>(start,size);
    }
    Check("word_20000_mixed_cached_cpu_gpu_operations_preserve_uploads_and_protection_counts",valid);
    next.FlushCachedWrites();old.FlushCachedWrites();
    next.ForEachModifiedRange<VideoCommon::Type::CPU,true>(0,4ULL<<20,[](u64,u64){});
    old.ForEachModifiedRange<BaselineWord::Type::CPU,true>(0,4ULL<<20,[](u64,u64){});
    const auto next_notifications=next_device.notifications,old_notifications=old_device.notifications;
    Bench("4MiB_clean_upload_baseline",1000000,[&](size_t){old.ForEachModifiedRange<BaselineWord::Type::CPU,true>(0,4ULL<<20,[](u64,u64){});});
    Bench("4MiB_clean_upload_current",1000000,[&](size_t){next.ForEachModifiedRange<VideoCommon::Type::CPU,true>(0,4ULL<<20,[](u64,u64){});});
    Check("clean_repeat_keeps_protection_counts",next_device.counts==old_device.counts && next_device.notifications==next_notifications && old_device.notifications==old_notifications);
}
namespace Settings {bool high=true;bool IsGPULevelHigh(){return high;}}
struct RasterizerDownloadArea {u64 startAddress{},endAddress{};bool preemtive{};auto operator<=>(const RasterizerDownloadArea&)const=default;};
struct CountingLock {
    size_t acquisitions{}; void lock(){++acquisitions;}void unlock(){}
};
struct MockCache {
    CountingLock mutex;bool present{},dirty{};
    bool IsRegionGpuModified(u64 address,u64 size){return present&&dirty&&address<0x20000&&address+size>0x10000;}
    std::optional<RasterizerDownloadArea> GetFlushArea(u64 address,u64 size){
        if(!IsRegionGpuModified(address,size))return {};
        return RasterizerDownloadArea{0x10000,0x20000,false};
    }
};
struct RasterizerHarness {
    struct Device {
        VideoCommon::FlushRegionCacheFilter<> filter;
        auto FilterFlushCaches(u64 address,u64 size,VideoCommon::CacheType type){return filter.Filter(address,size,type);}
    } device_memory;
    MockCache buffer_cache,texture_cache;
    bool MustFlushRegion(DAddr,u64,VideoCommon::CacheType);
    bool BaselineMustFlushRegion(DAddr,u64,VideoCommon::CacheType);
    RasterizerDownloadArea GetFlushArea(DAddr,u64);
    RasterizerDownloadArea BaselineGetFlushArea(DAddr,u64);
};
#include "flush-current.inc"
#include "flush-baseline.inc"
void FlushOracle() {
    using VideoCommon::CacheType;
    bool valid=true;
    for(u32 mask=0;mask<4;++mask)for(u32 dirty=0;dirty<4;++dirty)for(bool high:{false,true}) {
        RasterizerHarness h;Settings::high=high;
        h.buffer_cache.present=mask&1;h.texture_cache.present=mask&2;
        h.buffer_cache.dirty=dirty&1;h.texture_cache.dirty=dirty&2;
        if(mask&1)h.device_memory.filter.Register(0x10000,0x10000,CacheType::BufferCache);
        if(mask&2)h.device_memory.filter.Register(0x10000,0x10000,CacheType::TextureCache);
        for(u64 address:{0ULL,0x10000ULL,0x1ffffULL,0x20000ULL})for(u64 size:{1ULL,4096ULL,65536ULL}) {
            valid &= h.MustFlushRegion(address,size,CacheType::All)==h.BaselineMustFlushRegion(address,size,CacheType::All);
            valid &= h.GetFlushArea(address,size)==h.BaselineGetFlushArea(address,size);
        }
    }
    Check("flush_positive_negative_boundary_and_accuracy_cases_match_production_baseline",valid);
    RasterizerHarness unregistered;Settings::high=true;
    unregistered.MustFlushRegion(0x10000,4,CacheType::All);unregistered.GetFlushArea(0x10000,4);
    Check("unregistered_memory_skips_both_cache_locks",unregistered.buffer_cache.mutex.acquisitions==0&&unregistered.texture_cache.mutex.acquisitions==0);
    unregistered.BaselineMustFlushRegion(0x10000,4,CacheType::All);unregistered.BaselineGetFlushArea(0x10000,4);
    Check("baseline_same_clean_memory_takes_three_locks",unregistered.buffer_cache.mutex.acquisitions==1&&unregistered.texture_cache.mutex.acquisitions==2);

}

struct MemoryHarness {
    static constexpr u64 cpu_page_bits=12,big_page_bits=16;
    std::vector<u32> page_table;
    std::vector<u32> big_page_table_dev;
    std::atomic<u64> mapping_generation{};
    std::atomic<u32> mapping_updates_in_progress{};
    size_t unique_identifier;
    size_t walked_pages{};
    explicit MemoryHarness(size_t id,size_t pages=2048):page_table(pages),unique_identifier(id) {
        for(size_t i=0;i<pages;++i) page_table[i]=static_cast<u32>(0x10000+i);
    }
    template<bool big,class M,class F,class S> void MemoryOperation(u64 address,size_t size,M mapped,F fail,S small) const {
        for(size_t amount;size;size-=amount,address+=amount) {
            constexpr u64 bits=big?big_page_bits:cpu_page_bits;
            const u64 mask=(1ULL<<bits)-1;
            amount=std::min<u64>(size,(1ULL<<bits)-(address&mask));
            if constexpr(big) {if(small(address>>bits,address&mask,amount))break;}
            else {
                ++const_cast<MemoryHarness*>(this)->walked_pages;
                if(address/4096>=page_table.size()||page_table[address/4096]==0) {if(fail(address>>bits,address&mask,amount))break;}
                else if(mapped(address>>bits,address&mask,amount))break;
            }
        }
    }
    bool IsContinuousRange(GPUVAddr,size_t)const;
    bool BaselineContinuousRange(GPUVAddr,size_t)const;
};
#include "continuity-current.inc"
#include "continuity-baseline.inc"
void ContinuityOracle() {
    MemoryHarness memory(100);
    Check("continuous_initial_range",memory.IsContinuousRange(0,8ULL<<20));
    const auto scans=memory.walked_pages;
    Check("continuous_contained_reuse",memory.IsContinuousRange(17,20480)&&memory.walked_pages==scans);
    {
        Tegra::MappingChangeScope mapping(memory.mapping_generation,memory.mapping_updates_in_progress);
        memory.page_table[4]=0;
        Check("continuous_hole_invalidates_positive_result",!memory.IsContinuousRange(0,8ULL<<20));
        memory.page_table[4]=0x10004;
        Check("continuous_query_during_update_uses_page_walk",memory.IsContinuousRange(0,8ULL<<20));
        memory.page_table[4]=0;
    }
    Check("continuous_in_progress_result_was_not_retained",!memory.IsContinuousRange(0,8ULL<<20));
    bool valid=true;std::mt19937 rng(0x15CA8);
    for(size_t i=0;i<10000;++i) {
        if(i%3==0) {
            Tegra::MappingChangeScope mapping(memory.mapping_generation,memory.mapping_updates_in_progress);
            const size_t page=rng()%memory.page_table.size();
            memory.page_table[page]=(rng()%4==0)?0:static_cast<u32>(0x10000+page+(rng()%4==0?31:0));
        }
        const u64 start=rng()%(8ULL<<20),size=rng()%(2ULL<<20);
        valid &= memory.IsContinuousRange(start,size)==memory.BaselineContinuousRange(start,size);
    }
    Check("continuous_10000_remap_hole_fragment_queries_match_production_baseline",valid);
    MemoryHarness reused(101);reused.page_table[4]=0;
    Check("continuous_new_manager_does_not_reuse_previous_manager",!reused.IsContinuousRange(0,8ULL<<20));
    MemoryHarness huge(102,65536);
    Bench("256MiB_continuity_baseline",2000,[&](size_t){result_sink=huge.BaselineContinuousRange(0,256ULL<<20);});
    Bench("256MiB_continuity_current",2000,[&](size_t){result_sink=huge.IsContinuousRange(0,256ULL<<20);});
    Check("continuous_large_range_correct",result_sink==1);
    Tegra::ContinuousRangeCache cache;
    std::atomic<u64> generation{};std::atomic<u32> writers{};
    cache.Remember(1,0,0,4096);
    const auto stale_snapshot=generation.load();
    {Tegra::MappingChangeScope change(generation,writers);}
    Check("continuous_writer_finishing_after_reader_snapshot_rejects_stale_hit",
          !cache.ContainsStableMapping(1,stale_snapshot,0,4096,generation,writers));
    cache.Remember(1,generation.load(),0,4096);
    Check("continuous_fresh_snapshot_reuses_new_verified_result",
          cache.ContainsStableMapping(1,generation.load(),0,4096,generation,writers));
    cache.Remember(1,0,std::numeric_limits<u64>::max()-3,8);
    Check("continuous_wrapping_range_is_not_retained",!cache.Contains(1,0,std::numeric_limits<u64>::max()-3,1));
}
int main() {
    UsageOracle();WordOracle();ContinuityOracle();FlushOracle();
    std::cout << "RESULT checks=" << checks << " failed=" << failures << '\n';
    return failures ? 1:0;
}
