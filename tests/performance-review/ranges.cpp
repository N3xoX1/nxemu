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
#include "v9-word.h"
#include "yuzu_video_core/continuous_range_cache.h"
#include "yuzu_video_core/flush_region_cache_filter.h"
#include "diagnostics-stub.h"

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
template<class T, auto Kind> __declspec(noinline) void UploadScan(T& tracker,u64 size) {
    tracker.template ForEachModifiedRange<Kind,true>(0,size,[](u64,u64 bytes){result_sink=bytes;});
}

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
    DeviceTracker v9_device;
    PreviousWord::WordManager<DeviceTracker,16> v9(0,v9_device,4ULL<<20);
    UploadScan<decltype(v9),PreviousWord::Type::CPU>(v9,4ULL<<20);
    Bench("4MiB_clean_upload_baseline",1000000,[&](size_t){UploadScan<decltype(old),BaselineWord::Type::CPU>(old,4ULL<<20);});
    Bench("4MiB_clean_upload_v9",1000000,[&](size_t){UploadScan<decltype(v9),PreviousWord::Type::CPU>(v9,4ULL<<20);});
    Bench("4MiB_clean_upload_current",1000000,[&](size_t){UploadScan<decltype(next),VideoCommon::Type::CPU>(next,4ULL<<20);});
    Check("clean_repeat_keeps_protection_counts",next_device.counts==old_device.counts && next_device.notifications==next_notifications && old_device.notifications==old_notifications);
    struct LargeDevice {
        std::vector<int> counts;size_t notifications{};
        explicit LargeDevice(size_t size):counts((size+4095)/4096){}
        void UpdatePagesCachedCount(u64 addr,u64 size,int delta){++notifications;for(u64 page=addr/4096;page<(addr+size)/4096;++page)counts.at(page)+=delta;}
    };
    for(u64 size:{4ULL<<20,16ULL<<20,(16ULL<<20)+4096}) {
        LargeDevice da(size),db(size);
        VideoCommon::WordManager<LargeDevice,16> a(0,da,size);
        PreviousWord::WordManager<LargeDevice,16> b(0,db,size);
        using R=std::vector<std::pair<u64,u64>>;bool equal=true;
        const auto upload=[&](u64 offset,u64 bytes) {
            R x,y;a.ForEachModifiedRange<VideoCommon::Type::CPU,true>(offset,bytes,[&](u64 p,u64 n){x.emplace_back(p,n);});
            b.ForEachModifiedRange<PreviousWord::Type::CPU,true>(offset,bytes,[&](u64 p,u64 n){y.emplace_back(p,n);});
            equal&=x==y&&da.counts==db.counts&&da.notifications==db.notifications;
        };
        upload(0,size);
        for(unsigned round=0;round<1000;++round) {
            const u64 offset=rng()%size,bytes=std::min<u64>(1+rng()%65536,size-offset);
            if(round%3==0) {
                a.ChangeRegionState<VideoCommon::Type::CachedCPU,true>(offset,bytes);b.ChangeRegionState<PreviousWord::Type::CachedCPU,true>(offset,bytes);
                upload(offset,bytes); // CPU bits can be zero while untracked needs notification.
                a.FlushCachedWrites();b.FlushCachedWrites();
            }else {a.ChangeRegionState<VideoCommon::Type::CPU,true>(offset,bytes);b.ChangeRegionState<PreviousWord::Type::CPU,true>(offset,bytes);}
            upload(0,size);upload(0,size);
        }
        Check(size<=(16ULL<<20)?"CPU_summary_cached_untracked_and_64word_boundary_match_v9":"CPU_summary_large_manager_fallback_matches_v9",equal);
        a.ChangeRegionState<VideoCommon::Type::CPU,true>(size-1,1);b.ChangeRegionState<PreviousWord::Type::CPU,true>(size-1,1);
        auto moved_a=std::move(a);auto moved_b=std::move(b);R x,y;
        moved_a.ForEachModifiedRange<VideoCommon::Type::CPU,true>(0,size,[&](u64 p,u64 n){x.emplace_back(p,n);});
        moved_b.ForEachModifiedRange<PreviousWord::Type::CPU,true>(0,size,[&](u64 p,u64 n){y.emplace_back(p,n);});
        Check("CPU_summary_move_preserves_pending_upload_and_notifications",x==y&&da.counts==db.counts&&da.notifications==db.notifications);
    }
}
namespace Settings {bool high=true;bool IsGPULevelHigh(){return high;}}
struct RasterizerDownloadArea {u64 startAddress{},endAddress{};bool preemtive{};auto operator<=>(const RasterizerDownloadArea&)const=default;};
struct CountingLock {
    size_t acquisitions{}; void lock(){++acquisitions;}void unlock(){}
};
struct MockCache {
    CountingLock mutex;bool present{},dirty{};
    bool MayHaveGpuWrites(u64,u64) const {return true;} // History proofs have their own fixture.
    bool HasGpuDirtySummary() const {return false;}
    bool MayHaveGpuWritesForRead(u64 a,u64 n,bool) const {return MayHaveGpuWrites(a,n);}
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
    enum class EntryType : u64 { Free=0, Reserved=1, Mapped=2, SmallPages=3 };
    static constexpr u64 cpu_page_bits=12,page_bits=12,big_page_bits=16;
    static constexpr u64 page_size=1ULL<<page_bits,page_mask=page_size-1;
    static constexpr u64 big_page_size=1ULL<<big_page_bits,big_page_mask=big_page_size-1;
    std::vector<u32> page_table;
    std::vector<u32> big_page_table_dev;
    std::vector<u64> entries,big_entries;
    std::atomic<u64> mapping_generation{};
    std::atomic<u32> mapping_updates_in_progress{};
    size_t unique_identifier;
    size_t walked_pages{};
    explicit MemoryHarness(size_t id,size_t pages=2048):page_table(pages),
        big_page_table_dev((pages+15)/16), entries((pages+31)/32),
        big_entries(((pages+15)/16+31)/32),unique_identifier(id) {
        for(size_t i=0;i<pages;++i) page_table[i]=static_cast<u32>(0x10000+i);
    }
    void SyncEntries() {
        entries.assign(entries.size(),0);
        for(size_t i=0;i<page_table.size();++i) {
            entries[i/32] |= static_cast<u64>(page_table[i]?EntryType::Mapped:EntryType::Reserved) << (2*(i%32));
        }
    }
    template<bool big> void SetEntry(u64 address,EntryType value) {
        auto& table=big?big_entries:entries;
        const size_t index=address>>(big?big_page_bits:page_bits),shift=2*(index%32);
        table[index/32]=(table[index/32]&~(3ULL<<shift))|(static_cast<u64>(value)<<shift);
    }
    template<bool big> EntryType RawGetEntry(size_t) const;
    template<bool big> EntryType GetEntry(size_t address) const {
        ++const_cast<MemoryHarness*>(this)->walked_pages;
        return RawGetEntry<big>(address);
    }
    template<bool big,class M,class R,class U>
    void MemoryOperation(GPUVAddr,size_t,M&&,R&&,U&&) const;
    bool IsContinuousRange(GPUVAddr,size_t)const;
    bool BaselineContinuousRange(GPUVAddr,size_t)const;
    bool PreviousContinuousRange(GPUVAddr,size_t)const;
    size_t MaxContinuousRange(GPUVAddr,size_t)const;
    size_t BaselineMaxContinuousRange(GPUVAddr,size_t)const;
};
#include "memory-operations.inc"
#include "continuity-current.inc"
#include "continuity-baseline.inc"
#include "continuity-previous.inc"
#include "max-continuity-current.inc"
#include "max-continuity-baseline.inc"
void MaxContinuityOracle() {
    MemoryHarness memory(201);memory.SyncEntries();
    Check("max_continuity_full_range",memory.MaxContinuousRange(0,8ULL<<20)==(8ULL<<20));
    const auto scans=memory.walked_pages;
    Check("max_continuity_contained_range_reuses_proof",
          memory.MaxContinuousRange(17,20480)==20480&&memory.walked_pages==scans);
    {
        Tegra::MappingChangeScope update(memory.mapping_generation,memory.mapping_updates_in_progress);
        memory.page_table[4]=0;memory.SyncEntries();
        Check("max_continuity_remapped_hole_returns_exact_prefix",memory.MaxContinuousRange(0,8ULL<<20)==16384);
        memory.page_table[4]=0x10004;memory.SyncEntries();
        Check("max_continuity_active_writer_uses_live_walk",memory.MaxContinuousRange(0,8ULL<<20)==(8ULL<<20));
        memory.page_table[4]=0;memory.SyncEntries();
    }
    Check("max_continuity_active_writer_result_is_not_retained",memory.MaxContinuousRange(0,8ULL<<20)==16384);
    bool valid=true;std::mt19937 rng(0x5166B);
    for(size_t i=0;i<40000;++i) {
        if(i%7==0) {
            Tegra::MappingChangeScope update(memory.mapping_generation,memory.mapping_updates_in_progress);
            const size_t page=rng()%memory.page_table.size();
            memory.page_table[page]=rng()%3?static_cast<u32>(0x10000+page+(rng()%4==0?31:0)):0;
            memory.SyncEntries();
            const size_t big=rng()%memory.big_page_table_dev.size();
            memory.SetEntry<true>(big*65536,static_cast<MemoryHarness::EntryType>(rng()%4));
            memory.big_page_table_dev[big]=static_cast<u32>(0x10000+big*16+(rng()%4==0?19:0));
        }
        const u64 address=rng()%(8ULL<<20);
        const u64 size=std::min<u64>(rng()%(2ULL<<20),(8ULL<<20)-address);
        valid &= memory.MaxContinuousRange(address,size)==memory.BaselineMaxContinuousRange(address,size);
    }
    Check("max_continuity_40000_remaps_tags_holes_and_prefixes_match_validated_walk",valid);
    MemoryHarness small(202);small.SyncEntries();valid=true;
    for(u64 big=0;big<4;++big)for(u64 tag=0;tag<4;++tag) {
        Tegra::MappingChangeScope update(small.mapping_generation,small.mapping_updates_in_progress);
        small.SetEntry<true>(0,static_cast<MemoryHarness::EntryType>(big));
        small.SetEntry<false>(0,static_cast<MemoryHarness::EntryType>(tag));
        for(u64 offset:{0ULL,17ULL,4095ULL})for(u64 size:{0ULL,1ULL,4096ULL-offset}) {
            valid &= small.MaxContinuousRange(offset,size)==small.BaselineMaxContinuousRange(offset,size);
        }
    }
    Check("max_continuity_single_page_all_tags_and_empty_ranges_match_walk",valid);
    MemoryHarness reused(203);reused.SyncEntries();reused.page_table[4]=0;reused.SyncEntries();
    Check("max_continuity_new_owner_rejects_previous_proof",reused.MaxContinuousRange(0,8ULL<<20)==16384);
    MemoryHarness huge(204,65536);huge.SyncEntries();
    Bench("16MiB_max_continuity_baseline",2000,[&](size_t){result_sink=huge.BaselineMaxContinuousRange(0,16ULL<<20);});
    Bench("16MiB_max_continuity_current",2000,[&](size_t){result_sink=huge.MaxContinuousRange(0,16ULL<<20);});
    Check("max_continuity_large_cached_result_preserved",result_sink==(16ULL<<20));
}
void ContinuityOracle() {
    MemoryHarness memory(100);
    memory.SyncEntries();
    Check("continuous_initial_range",memory.IsContinuousRange(0,8ULL<<20));
    const auto scans=memory.walked_pages;
    Check("continuous_contained_reuse",memory.IsContinuousRange(17,20480)&&memory.walked_pages==scans);
    {
        Tegra::MappingChangeScope mapping(memory.mapping_generation,memory.mapping_updates_in_progress);
        memory.page_table[4]=0;
        memory.SyncEntries();
        Check("continuous_hole_invalidates_positive_result",!memory.IsContinuousRange(0,8ULL<<20));
        memory.page_table[4]=0x10004;
        memory.SyncEntries();
        Check("continuous_query_during_update_uses_page_walk",memory.IsContinuousRange(0,8ULL<<20));
        memory.page_table[4]=0;
        memory.SyncEntries();
    }
    Check("continuous_in_progress_result_was_not_retained",!memory.IsContinuousRange(0,8ULL<<20));
    bool valid=true;std::mt19937 rng(0x15CA8);
    for(size_t i=0;i<10000;++i) {
        if(i%3==0) {
            Tegra::MappingChangeScope mapping(memory.mapping_generation,memory.mapping_updates_in_progress);
            const size_t page=rng()%memory.page_table.size();
            memory.page_table[page]=(rng()%4==0)?0:static_cast<u32>(0x10000+page+(rng()%4==0?31:0));
            memory.SyncEntries();
        }
        const u64 start=rng()%(8ULL<<20),size=std::min<u64>(rng()%(2ULL<<20),(8ULL<<20)-start);
        valid &= memory.IsContinuousRange(start,size)==memory.BaselineContinuousRange(start,size);
    }
    Check("continuous_10000_remap_hole_fragment_queries_match_production_baseline",valid);
    MemoryHarness small(103);small.SyncEntries();
    valid=true;
    for(u64 big=0;big<4;++big)for(u64 tag=0;tag<4;++tag) {
        Tegra::MappingChangeScope mapping(small.mapping_generation,small.mapping_updates_in_progress);
        small.SetEntry<true>(0,static_cast<MemoryHarness::EntryType>(big));
        small.SetEntry<false>(0,static_cast<MemoryHarness::EntryType>(tag));
        for(u64 offset:{0ULL,17ULL,4095ULL})for(u64 bytes:{0ULL,1ULL,4096ULL-offset}) {
            valid &= small.IsContinuousRange(offset,bytes)==small.BaselineContinuousRange(offset,bytes);
        }
    }
    Check("single_page_all_big_and_small_mapping_tags_match_exact_page_walk",valid);
    small.SetEntry<true>(0,MemoryHarness::EntryType::SmallPages);
    small.SetEntry<false>(0,MemoryHarness::EntryType::Mapped);
    small.SetEntry<false>(4096,MemoryHarness::EntryType::Reserved);
    Check("single_page_fast_path_stops_at_first_unmapped_byte",
          small.IsContinuousRange(4095,1)&&!small.IsContinuousRange(4095,2));
    small.SetEntry<true>(0,MemoryHarness::EntryType::Reserved);
    Check("reserved_big_page_does_not_expose_mapped_small_page",!small.IsContinuousRange(0,1));
    MemoryHarness tiny(104);tiny.SyncEntries();
    Bench("128B_256pages_continuity_baseline",1000000,[&](size_t i){result_sink=tiny.BaselineContinuousRange((i%256)*4096+128,128);});
    Bench("128B_256pages_continuity_previous",1000000,[&](size_t i){result_sink=tiny.PreviousContinuousRange((i%256)*4096+128,128);});
    Bench("128B_256pages_continuity_current",1000000,[&](size_t i){result_sink=tiny.IsContinuousRange((i%256)*4096+128,128);});
    MemoryHarness reused(101);reused.page_table[4]=0;reused.SyncEntries();
    Check("continuous_new_manager_does_not_reuse_previous_manager",!reused.IsContinuousRange(0,8ULL<<20));
    MemoryHarness huge(102,65536);
    huge.SyncEntries();
    Bench("256MiB_continuity_baseline",2000,[&](size_t){result_sink=huge.BaselineContinuousRange(0,256ULL<<20);});
    Bench("256MiB_continuity_previous",2000,[&](size_t){result_sink=huge.PreviousContinuousRange(0,256ULL<<20);});
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
    UsageOracle();WordOracle();ContinuityOracle();MaxContinuityOracle();FlushOracle();
    std::cout << "RESULT checks=" << checks << " failed=" << failures << '\n';
    return failures ? 1:0;
}
