// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>
#include "yuzu_common/parallel_readback_copy.h"
#include "yuzu_video_core/query_cache/query_base.h"
#include "diagnostics-stub.h"
using namespace VideoCommon;
unsigned checks{},failures{};
void Check(const char* name,bool okay){++checks;failures+=!okay;std::cout<<(okay?"PASS ":"FAIL ")<<name<<'\n';}
struct RetireModel {
    std::atomic<bool> copying{},allow_copy{},copy_complete{};
    std::vector<unsigned char> guest=std::vector<unsigned char>(4096,0x11),gpu=std::vector<unsigned char>(4096,0x77);
    std::atomic<unsigned> cpu_metadata{};bool texture_done{},query_after_copy{};
    struct Texture {
        std::recursive_mutex mutex;RetireModel& owner;
        void PopAsyncFlushes(){owner.texture_done=true;}
    } texture_cache{{},*this};
    struct Buffer {
        std::recursive_mutex mutex;RetireModel& owner;
        void PopAsyncFlushes(){
            owner.copying.store(true,std::memory_order_release);owner.copying.notify_one();
            owner.allow_copy.wait(false,std::memory_order_acquire);
            std::memcpy(owner.guest.data(),owner.gpu.data(),owner.gpu.size());
            owner.copy_complete.store(true,std::memory_order_release);
        }
    } buffer_cache{{},*this};
    struct Query {
        RetireModel& owner;void PopAsyncFlushes(){owner.query_after_copy=owner.texture_done&&owner.copy_complete.load(std::memory_order_acquire);}
    } query_cache{*this};
    void PopAsyncFlushes();
};
#include "retirement.inc"

struct QueryModel {
    using QueryLocation=unsigned;
    struct Impl {
        struct Memory {
            std::vector<unsigned char> bytes;
            template<class T>T* GetPointer(DAddr address){return reinterpret_cast<T*>(bytes.data()+address);}
        } device_memory;
        GuestQuery query{false,0,0};
        QueryBase* ObtainQuery(QueryLocation){return &query;}
    } storage;
    Impl* impl=&storage;
    bool SemiFlushQueryDirty(QueryLocation);
};
#include "query.inc"

void LockControls(){
    for(bool release_early:{false,true}){
        auto model=std::make_unique<RetireModel>();auto& r=*model;
        auto retirement=std::async(std::launch::async,[&]{
            if(!release_early)r.PopAsyncFlushes();
            else { // Test-only negative control: snapshot ownership, then unlock.
                {std::scoped_lock lock{r.buffer_cache.mutex,r.texture_cache.mutex};r.texture_cache.PopAsyncFlushes();}
                r.buffer_cache.PopAsyncFlushes();r.query_cache.PopAsyncFlushes();
            }
        });
        r.copying.wait(false,std::memory_order_acquire);
        auto writer=std::async(std::launch::async,[&]{
            std::scoped_lock lock{r.buffer_cache.mutex};
            r.cpu_metadata=1;r.guest[0]=0xA5;
        });
        const bool writer_finished=writer.wait_for(std::chrono::milliseconds(30))==std::future_status::ready;
        Check("CPU_writer_blocks_during_production_retirement_and_escapes_unlocked_control",writer_finished==release_early);
        // Under the unlocked control the writer has completed before memcpy;
        // under production locking it cannot touch RAM until memcpy completes.
        if(release_early)writer.get();
        r.allow_copy.store(true,std::memory_order_release);r.allow_copy.notify_one();
        retirement.get();if(!release_early)writer.get();
        Check("production_retains_late_CPU_write_negative_control_overwrites_it",r.guest[0]==(release_early?0x77:0xA5));
        Check("query_retirement_observes_completed_texture_and_buffer_copy",r.query_after_copy);
        Check("whole_GPU_payload_completed_before_retirement_returns",std::memcmp(r.guest.data()+1,r.gpu.data()+1,r.gpu.size()-1)==0);
    }
}
void RawQueryControls(){
    constexpr size_t bytes=2*1024*1024+31;constexpr DAddr address=65536+127;
    std::vector<unsigned char> reference(bytes,0x39);
    QueryModel query;query.storage.device_memory.bytes=reference;
    query.storage.query.guest_address=address;query.storage.query.value=0xF1234567AABBCCDDULL;
    query.storage.query.flags=QueryFlagBits::IsFinalValueSynced|QueryFlagBits::IsHostManaged;
    Common::ReadbackCopyContext context;
    for(bool wide:{false,true}){
        query.storage.device_memory.bytes=reference;
        if(wide)query.storage.query.flags|=QueryFlagBits::HasTimestamp;
        else query.storage.query.flags&=~QueryFlagBits::HasTimestamp;
        const bool still_dirty=query.SemiFlushQueryDirty(0);
        const size_t write_size=wide?8:4;
        const auto value=query.storage.query.value;
        Check("actual_query_method_writes_raw_RAM_without_buffer_tracking",!still_dirty&&std::memcmp(query.storage.device_memory.bytes.data()+address,&value,write_size)==0);
        // A subsequent GPU write produces the same bytes as the old GPU epoch.
        // Its GPU comparison flag is zero; RAM has nevertheless changed.
        const bool gpu_changed=false;
        auto unsafe=query.storage.device_memory.bytes;
        if(gpu_changed)std::memcpy(unsafe.data(),reference.data(),bytes);
        Check("GPU_flag_only_elision_leaves_wrong_guest_bytes_negative_control",unsafe!=reference);
        // A generation counter bumped only by BufferCache::WriteMemory also
        // misses this production query writer; such a shortcut is not a proof.
        const unsigned buffer_generation_before=0,buffer_generation_after=0;
        Check("buffer_only_generation_elision_also_misses_raw_query_writer",buffer_generation_before==buffer_generation_after&&unsafe!=reference);
        const auto copied=Common::CopyChangedMemoryForReadback(query.storage.device_memory.bytes.data(),reference.data(),bytes);
        Check("production_exact_readback_repairs_unchanged_GPU_epoch_after_query_write",copied>0&&query.storage.device_memory.bytes==reference);
    }
    // A cancelled CPU byte belongs to a later CPU write and must be excluded,
    // even when all other bytes are restored from the current GPU epoch.
    query.storage.device_memory.bytes.assign(bytes,0x11);
    query.storage.device_memory.bytes[address]=0xA5;
    Common::CopyChangedMemoryForReadback(query.storage.device_memory.bytes.data(),reference.data(),address);
    Common::CopyChangedMemoryForReadback(query.storage.device_memory.bytes.data()+address+1,reference.data()+address+1,bytes-address-1);
    auto expected=reference;expected[address]=0xA5;
    Check("excluding_cancelled_CPU_byte_keeps_newer_value_while_restoring_other_RAM",query.storage.device_memory.bytes==expected);
}
int main(){LockControls();RawQueryControls();std::cout<<"RESULT checks="<<checks<<" failed="<<failures<<'\n';return failures?1:0;}
