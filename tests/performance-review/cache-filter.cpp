// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include "yuzu_common/common_types.h"
#include "yuzu_common/range_sets.inc"
#include "yuzu_video_core/flush_region_cache_filter.h"
#include "yuzu_video_core/gpu_write_history.h"
#include "yuzu_video_core/gpu_dirty_summary.h"
#include "diagnostics-stub.h"
using namespace VideoCommon;
using BufferId=unsigned;
namespace Settings { bool high{}; bool IsGPULevelHigh(){return high;} }
struct CountingMutex {
    std::recursive_mutex inner;unsigned calls{};
    void lock(){inner.lock();++calls;}void unlock(){inner.unlock();}
};
struct BufferModel {
    CountingMutex mutex;GpuWriteHistory<> gpu_write_history;
    std::unique_ptr<GpuDirtySummary<>> gpu_dirty_summary;
    Common::RangeSet<DAddr>* exact_dirty_ranges{};
    bool registered{},dirty{};unsigned cpu_writes{};
    bool MayHaveGpuWrites(DAddr,size_t)const noexcept;
    bool HasGpuDirtySummary()const noexcept;
    bool MayHaveGpuWritesForRead(DAddr,size_t,bool)const noexcept;
    bool IsRegionGpuModified(DAddr addr,size_t size){if(!exact_dirty_ranges)return dirty;bool found=false;exact_dirty_ranges->ForEachInRange(addr,size,[&](DAddr,DAddr){found=true;});return found;}
    bool OnCPUWrite(DAddr,u64){if(!registered)return false;if(dirty)return true;++cpu_writes;return false;}
};
struct TextureModel {
    CountingMutex mutex;bool registered{},dirty{};unsigned cpu_writes{};
    bool IsRegionGpuModified(DAddr,u64){return dirty;}
    void WriteMemory(DAddr,u64){if(registered)++cpu_writes;}
};
struct DeviceModel {
    FlushRegionCacheFilter<> filter;
    CacheType FilterFlushCaches(DAddr a,u64 n,CacheType c){return filter.Filter(a,n,c);}
};
struct RasterizerModel {
    DeviceModel device_memory;BufferModel buffer_cache;TextureModel texture_cache;
    struct {unsigned calls{};void InvalidateRegion(DAddr,u64){++calls;}} pipeline_cache;
    bool MustFlushRegion(DAddr,u64,CacheType);
    bool OnCPUWrite(DAddr,u64);
};
struct WrittenModel {
    GpuWriteHistory<> gpu_write_history;
    std::unique_ptr<GpuDirtySummary<>> gpu_dirty_summary{std::make_unique<GpuDirtySummary<>>()};
    bool published_first{true};unsigned dirty_calls{},host_calls{},content_calls{};
    struct Tracker {WrittenModel& owner;void MarkRegionAsGpuModified(DAddr a,u32 n){++owner.dirty_calls;owner.published_first &= (!GpuWriteFilterEnabled() || owner.gpu_write_history.MayHaveWrites(a,n)) && owner.gpu_dirty_summary->MayHaveWrites(a,n);}} memory_tracker{*this};
    struct Ranges {WrittenModel& owner;void Add(DAddr a,u32 n){owner.published_first &= !GpuWriteFilterEnabled() || owner.gpu_write_history.MayHaveWrites(a,n);}} gpu_modified_ranges{*this},uncommitted_gpu_modified_ranges{*this};
    struct Runtime {WrittenModel& owner;void MarkHostWrite(unsigned){++owner.host_calls;}} runtime{*this};
    std::array<unsigned,1> slot_buffers{};
    void MarkBufferContentChanged(unsigned,DAddr,u32){++content_calls;}
    void MarkWrittenBuffer(BufferId,DAddr,u32);
};
#include "rasterizer.inc"
unsigned checks{},failures{};
void Check(const char* name,bool okay){++checks;failures+=!okay;std::cout<<(okay?"PASS ":"FAIL ")<<name<<'\n';}
void Initialize(RasterizerModel& r,bool b,bool t,bool db,bool dt) {
    constexpr DAddr address=0x20000;
    r.buffer_cache.registered=b;r.texture_cache.registered=t;
    r.buffer_cache.dirty=db;r.texture_cache.dirty=dt;
    if(b)r.device_memory.filter.Register(address,64,CacheType::BufferCache);
    if(t)r.device_memory.filter.Register(address,64,CacheType::TextureCache);
    if(db)r.buffer_cache.gpu_write_history.Mark(address,64);
}
void CurrentSummaryPredicate(){
    auto model=std::make_unique<RasterizerModel>();auto& r=*model;
    constexpr DAddr address=0x20000,pending=address+65536;
    Initialize(r,true,false,false,false);Settings::high=false;
    Common::RangeSet<DAddr> remaining;r.buffer_cache.exact_dirty_ranges=&remaining;
    r.buffer_cache.gpu_dirty_summary=std::make_unique<GpuDirtySummary<>>();
    remaining.Add(address,64);r.buffer_cache.gpu_dirty_summary->Mark(address,64);
    r.buffer_cache.gpu_write_history.Mark(address,64);
    Check("current_dirty_positive_uses_actual_locked_predicate",r.MustFlushRegion(address,1,CacheType::BufferCache)&&r.buffer_cache.mutex.calls==1);
    r.device_memory.filter.Register(pending,64,CacheType::BufferCache);
    remaining.Add(pending,64);r.buffer_cache.gpu_dirty_summary->Mark(pending,64);
    remaining.Subtract(address,64);
    r.buffer_cache.gpu_dirty_summary->Refresh(address,64,[&](DAddr a,u64 n,auto&& visit){remaining.ForEachInRange(a,n,visit);});
    r.buffer_cache.mutex.calls=0;
    Check("retired_region_skips_original_cache_mutex",!r.MustFlushRegion(address,1,CacheType::BufferCache)&&r.buffer_cache.mutex.calls==0);
    Check("later_pending_region_keeps_original_cache_mutex",r.MustFlushRegion(pending,1,CacheType::BufferCache)&&r.buffer_cache.mutex.calls==1);
    r.buffer_cache.mutex.lock();
    auto negative=std::async(std::launch::async,[&]{return r.MustFlushRegion(address,1,CacheType::BufferCache);});
    const bool ready=negative.wait_for(std::chrono::milliseconds(30))==std::future_status::ready;
    r.buffer_cache.mutex.unlock();Check("retired_negative_completes_while_cache_mutex_is_held",ready&&!negative.get());
    r.device_memory.filter.Register(address,64,CacheType::TextureCache);
    const auto before=r.buffer_cache.mutex.calls;
    Check("texture_alias_preserves_historical_buffer_lock",!r.MustFlushRegion(address,1,CacheType::BufferCache)&&r.buffer_cache.mutex.calls==before+1);
}
int main(){
    bool flush=true,writes=true,early=true;unsigned cases{};
    constexpr DAddr address=0x20000;
    for(bool b:{false,true})for(bool t:{false,true})for(bool db:{false,true})for(bool dt:{false,true}) {
        if((db&&!b)||(dt&&!t))continue;
        for(bool high:{false,true})for(auto requested:{CacheType::None,CacheType::BufferCache,CacheType::TextureCache,CacheType::BufferCache|CacheType::TextureCache}){
            Settings::high=high;RasterizerModel r;Initialize(r,b,t,db,dt);
            const bool expected=(True(requested&CacheType::BufferCache)&&db)||(high&&True(requested&CacheType::TextureCache)&&dt);
            flush &= r.MustFlushRegion(address,64,requested)==expected;++cases;
        }
        RasterizerModel r;Initialize(r,b,t,db,dt);
        const bool actual=r.OnCPUWrite(address,64);
        writes &= actual==db && r.buffer_cache.cpu_writes==(b&&!db) &&
                  r.texture_cache.cpu_writes==(t&&!db) && r.pipeline_cache.calls==!db;
        if(db)early &= r.texture_cache.mutex.calls==0&&r.pipeline_cache.calls==0;
    }
    Check("72_flush_presence_dirty_accuracy_and_requested_cache_cases_match_original",flush&&cases==72);
    Check("CPU_write_effects_and_shader_invalidation_match_original",writes);
    Check("GPU_dirty_CPU_write_preserves_original_early_return",early);
    RasterizerModel clean;Initialize(clean,true,false,false,false);
    clean.MustFlushRegion(address,1,CacheType::BufferCache);
    Check("never_GPU_written_registered_buffer_skips_lock_only_when_enabled",clean.buffer_cache.mutex.calls==!GpuWriteFilterEnabled());
    RasterizerModel absent;absent.OnCPUWrite(address,1);
    Check("unregistered_CPU_write_skips_both_cache_locks_only_when_enabled",
          absent.buffer_cache.mutex.calls==!GpuWriteFilterEnabled()&&absent.texture_cache.mutex.calls==!GpuWriteFilterEnabled()&&absent.pipeline_cache.calls==1);
    RasterizerModel old;Initialize(old,true,false,false,false);old.buffer_cache.gpu_write_history.Mark(address,1);
    Check("previously_dirty_now_clean_buffer_uses_original_locked_predicate",!old.MustFlushRegion(address,1,CacheType::BufferCache)&&old.buffer_cache.mutex.calls==1);
    RasterizerModel empty;Check("empty_CPU_write_is_noop",!empty.OnCPUWrite(0,1)&&!empty.OnCPUWrite(address,0)&&empty.pipeline_cache.calls==0);
    RasterizerModel unsupported;
    unsupported.MustFlushRegion((1ULL<<34)-1,2,CacheType::BufferCache);
    Check("unsupported_read_query_conservatively_locks",unsupported.buffer_cache.mutex.calls==1);
    WrittenModel written;written.MarkWrittenBuffer(0,address,1);
    Check("actual_ordinary_GPU_write_publishes_before_tracker_and_ranges",written.published_first&&written.dirty_calls==1&&written.host_calls==1&&written.content_calls==1);
    // Writer retains the real cache lock. A proven negative is allowed to finish
    // before its release; a positive or disabled path must wait for that release.
    for(bool positive:{false,true}) {
        RasterizerModel r;Initialize(r,true,false,positive,false);
        r.buffer_cache.mutex.lock();
        auto query=std::async(std::launch::async,[&]{return r.MustFlushRegion(address,1,CacheType::BufferCache);});
        const bool ready=query.wait_for(std::chrono::milliseconds(30))==std::future_status::ready;
        r.buffer_cache.mutex.unlock();const bool result=query.get();
        Check("held_cache_lock_is_bypassed_only_for_proven_negative",ready==(GpuWriteFilterEnabled()&&!positive)&&result==positive);
    }
    CurrentSummaryPredicate();
    std::cout<<"RESULT checks="<<checks<<" failed="<<failures<<" filter_enabled="<<GpuWriteFilterEnabled()<<'\n';return failures?1:0;
}
