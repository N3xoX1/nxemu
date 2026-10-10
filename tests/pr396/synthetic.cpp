#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <boost/container/small_vector.hpp>
#include "yuzu_common/bit_field.h"
#include "yuzu_common/range_sets.inc"
#include "yuzu_video_core/buffer_cache/usage_tracker.h"
#include "yuzu_video_core/renderer_vulkan/descriptor_payload.h"

using BufferId = u32;
struct BufferCopy { u64 src_offset{}, dst_offset{}, size{}; };
struct Buffer {
    u64 base{}, content_version{1};
    std::vector<u8> data;
    u64 usage_bytes{};
    VideoCommon::UsageTracker usage{4096};
    u64 CpuAddr() const { return base; }
    u32 Offset(u64 address) const { return static_cast<u32>(address-base); }
    void MarkUsage(u64 offset, u64 size) { usage_bytes += size; usage.Track(offset,size); }
    bool IsRegionUsed(u64 offset,u64 size) const {return usage.IsUsed(offset,size);}
};
struct Memory {
    std::vector<u8> data = std::vector<u8>(1 << 20, 0x11);
    void WriteBlockUnsafe(u64 start, const u8* source, u64 size) {
        std::copy_n(source, size, data.begin()+start);
    }
};
struct Tracker {
    std::vector<std::pair<u64,u64>> uploads;
    template<class F> void ForEachUploadRange(u64, u32, F f) {
        auto pending = std::move(uploads); uploads.clear();
        for (auto [a,s] : pending) f(a,s);
    }
    void MarkRegionAsGpuModified(u64,u64) {}
};
struct Runtime {
    u64 copy_calls{}, copy_bytes{}, marked_writes{};
    bool execute{true};
    void MarkHostWrite(const Buffer&) { ++marked_writes; }
    template<class Copies> void CopyBuffer(Buffer& dst, Buffer& src, const Copies& copies, bool) {
        ++copy_calls;
        for (auto c : copies) {
            copy_bytes += c.size;
            if (execute) std::copy_n(src.data.begin()+c.src_offset, c.size, dst.data.begin()+c.dst_offset);
        }
    }
};
struct AsyncBuffer { std::span<u8> mapped_span; size_t offset{}; };
struct Mapping { u64 MappingGeneration() const { return 1; } };
struct Harness {
    struct SparseBufferPart { DAddr device_addr{}; u32 size{},offset{}; BufferId buffer_id{}; u64 content_version{}; };
    struct SparseBuffer {
        const Mapping* gpu_memory{}; GPUVAddr gpu_addr{}; u32 size{}, capacity{};
        BufferId buffer_id{}; u64 mapping_generation{1}, frame_tick{}, bind_tick{};
        std::vector<SparseBufferPart> parts;
    };
    struct SparseBinding { BufferId buffer_id{}; u32 offset{},size{}; };
    Memory device_memory;
    Tracker memory_tracker;
    Runtime runtime;
    std::deque<Buffer> slot_buffers{Buffer{}};
    Mapping mapping;
    Mapping* gpu_memory{&mapping};
    Common::RangeSet<DAddr> gpu_modified_ranges, uncommitted_gpu_modified_ranges, downloaded_ranges;
    std::deque<Common::RangeSet<DAddr>> committed_gpu_modified_ranges, pending_download_ranges;
    Common::OverlapRangeSet<DAddr> async_downloads;
    std::deque<std::optional<AsyncBuffer>> async_buffers;
    std::deque<boost::container::small_vector<BufferCopy,4>> pending_downloads;
    std::deque<AsyncBuffer> async_buffers_death_ring;
    std::deque<std::vector<u8>> staging;
    std::vector<SparseBuffer> sparse_buffers;
    std::vector<std::pair<GPUVAddr,u32>> sparse_writes;
    u64 frame_tick{}, sparse_bind_tick{1}, content_version{1};
    bool has_bound_sparse{true}, has_written_sparse{true};
    void UploadMemory(Buffer& b, u64, u64, std::span<BufferCopy> copies) {
        for (auto c : copies) std::copy_n(device_memory.data.begin()+b.base+c.dst_offset, c.size, b.data.begin()+c.dst_offset);
    }
#if FIXED_MODE
#include "content-version.inc"
#else
    void MarkBufferContentChanged(Buffer& b) { b.content_version = ++content_version; }
#endif
    void TouchBuffer(Buffer&,BufferId) {}
    BufferId FindBuffer(u64 a,u32 size) {
        for (u32 i=1;i<slot_buffers.size();++i) {
            auto& b=slot_buffers[i];
            if (b.base<=a && a+size<=b.base+b.data.size() && b.base!=0) return i;
        }
        throw std::runtime_error("Unconfigured test buffer");
    }
    SparseBuffer& FindSparseBuffer(GPUVAddr a,u32 size) {
        for (auto& s:sparse_buffers) if(s.gpu_memory==gpu_memory && s.gpu_addr<=a && a+size<=s.gpu_addr+s.size) return s;
        throw std::runtime_error("Unconfigured sparse buffer");
    }
    void UpdateSparseBuffer(GPUVAddr,u32) { throw std::runtime_error("Unexpected mapping update"); }
    SparseBinding GetSparseBinding(GPUVAddr a,u32 size) {
        auto& s=FindSparseBuffer(a,size); auto off=static_cast<u32>(a-s.gpu_addr);
        return {s.buffer_id,off,std::min(size,s.capacity-off)};
    }
    bool SynchronizeBuffer(Buffer&,DAddr,u32);
    bool SynchronizeBuffer(Buffer&,DAddr,u32,boost::container::small_vector<BufferCopy,4>&);
    void ClearDownload(DAddr,u64);
    void PopAsyncBuffers();
    SparseBinding SynchronizeSparseBuffer(GPUVAddr,u32);
    GPUVAddr FindSparseAlias(const struct Binding&,bool) const;
    void MarkSparseWrite(GPUVAddr,u32);
    void CopySparseWrites();
    BufferId AddBuffer(u64 base,u32 size,u8 value=0x11) {
        slot_buffers.push_back(Buffer{base,1,std::vector<u8>(size,value)});
        return static_cast<u32>(slot_buffers.size()-1);
    }
    bool Dirty(u64 start,u32 size) {
        bool result=false; gpu_modified_ranges.ForEachInRange(start,size,[&](u64,u64){result=true;}); return result;
    }
    void Queue(u64 start,u32 size,u8 value) {
        staging.emplace_back(size,value);
        async_buffers.emplace_back(AsyncBuffer{staging.back(),0});
        pending_downloads.push_back({BufferCopy{start,0,size}});
        pending_download_ranges.emplace_back(); pending_download_ranges.back().Add(start,size);
        async_downloads.Add(start,size); gpu_modified_ranges.Add(start,size);
    }
};
struct Binding { DAddr device_addr{}; u32 size{}; BufferId buffer_id{}; GPUVAddr sparse_gpu_addr{}; bool is_sparse{}; };

#if FIXED_MODE
#include "fixed-functions.inc"
#elif PR_MODE
#include "pr-functions.inc"
#else
#include "master-functions.inc"
#endif

using VkBuffer = u64;
constexpr VkBuffer VK_NULL_HANDLE = 0;
struct HazardTracker {
    std::vector<VkBuffer> written_buffers; bool all_buffers_written{};
    void MarkBufferWrite(VkBuffer);
    bool IsBufferWritten(VkBuffer) const noexcept;
};
#include "tracking.inc"
namespace Settings {
    bool dma_default=true, safe=true;
    bool IsDMALevelDefault() { return dma_default; }
    bool UseSafeDMAReads() { return safe; }
}
namespace Engines { enum class EngineTypes { Maxwell3D, KeplerCompute }; }
constexpr u32 MacroRegistersStart=0xe00, ComputeInline=0x6d;
bool DMAReadSafe(u32 method,u32 method_count,bool compute=false,u32 entry_size=1,bool non_incrementing=true) {
    struct {u32 method,method_count,subchannel;bool non_incrementing;} dma_state{method,method_count,0,non_incrementing};
    struct {u32 size;} header{entry_size};
    std::array subchannel_type{compute?Engines::EngineTypes::KeplerCompute:Engines::EngineTypes::Maxwell3D};
#if FIXED_MODE
#include "fixed-dma.inc"
#elif PR_MODE
#include "pr-dma.inc"
#else
#include "master-dma.inc"
#endif
    return use_safe;
}

#include "command-types.inc"
struct DummyEngine { std::array<bool,8192> execution_mask; std::vector<std::pair<u32,u32>> method_sink; DummyEngine(){execution_mask.fill(true);} };
struct Parser {
    static constexpr u32 non_puller_methods=0x40;
    struct {u32 method{},subchannel{},method_count{};u64 dma_get{},dma_word_offset{};bool non_incrementing{},is_last_call{};} dma_state;
    bool dma_increment_once{};DummyEngine engine;std::array<DummyEngine*,8> subchannels{};
    mutable std::vector<std::pair<u32,u32>> written;
    Parser(){subchannels.fill(&engine);}
    void CallMethod(u32 argument) const {written.emplace_back(dma_state.method,argument);}
    void CallMultiMethod(const u32* args,u32 count) const {for(u32 i=0;i<count;++i)CallMethod(args[i]);}
    void SetState(const CommandHeader&);
    void ProcessCommands(std::span<const CommandHeader>);
};
#include "parser.inc"

int failures{}, tests{};
void Check(std::string name,bool ok) {
    ++tests; failures+=!ok;
    std::cout << (ok?"PASS ":"FAIL ") << name << '\n';
}
void TestSynchronization() {
    for(bool with_copies:{false,true}) {
        Harness h; auto id=h.AddBuffer(4096,4096); auto& b=h.slot_buffers[id];
        std::fill(b.data.begin()+64,b.data.begin()+128,0x22);
        h.gpu_modified_ranges.Add(4096+64,64);
        h.memory_tracker.uploads={{4096,4096}};
        boost::container::small_vector<BufferCopy,4> copies;
        if(with_copies) h.SynchronizeBuffer(b,4096,4096,copies); else h.SynchronizeBuffer(b,4096,4096);
        Check(with_copies?"upload_preserves_gpu_bytes_sparse_overload":"upload_preserves_gpu_bytes_ordinary_overload",
              b.data[64]==0x22 && b.data[127]==0x22 && b.data[0]==0x11 && b.data[128]==0x11);
    }
    std::mt19937 rng{396};
    bool all_ok=true;
    for(u32 iteration=0;iteration<2000;++iteration) {
        Harness h; auto id=h.AddBuffer(4096,4096); auto& b=h.slot_buffers[id];
        std::array<bool,4096> dirty{};
        for(u32 j=0;j<16;++j) {
            u32 begin=rng()%4096, length=std::min(1+rng()%127,4096-begin);
            h.gpu_modified_ranges.Add(4096+begin,length);
            for(u32 k=begin;k<begin+length;++k) {dirty[k]=true;b.data[k]=0x22;}
        }
        h.memory_tracker.uploads={{4096,4096}};
        boost::container::small_vector<BufferCopy,4> copies;
        h.SynchronizeBuffer(b,4096,4096,copies);
        for(u32 i=0;i<4096;++i) all_ok &= b.data[i]==(dirty[i]?0x22:0x11);
    }
    Check("random_upload_byte_oracle_2000_cases",all_ok);
}
void TestAsync() {
    {Harness h; h.Queue(4096,128,0x22); h.ClearDownload(4096+32,32);
     std::fill_n(h.device_memory.data.begin()+4096+32,32,0x33);
     h.Queue(4096+32,32,0x44); h.PopAsyncBuffers();
     Check("cancelled_old_download_does_not_overwrite_new_guest_data",h.device_memory.data[4096+32]==0x33);
     h.PopAsyncBuffers();Check("new_download_after_cancellation_reaches_guest",h.device_memory.data[4096+32]==0x44);}
    {Harness h;h.Queue(4096,128,0x22);h.uncommitted_gpu_modified_ranges.Add(4096+32,32);h.PopAsyncBuffers();
     Check("new_uncommitted_gpu_write_stays_dirty",h.Dirty(4096+32,32));
     Check("downloaded_clean_prefix_is_clean",!h.Dirty(4096,32));}
    {Harness h;h.Queue(4096,128,0x22);h.committed_gpu_modified_ranges.emplace_back();
     h.committed_gpu_modified_ranges.back().Add(4096+32,32);h.PopAsyncBuffers();
     Check("new_accumulated_gpu_write_stays_dirty",h.Dirty(4096+32,32));}
    {Harness h;h.Queue(4096,128,0x22);h.Queue(4096+32,32,0x44);h.PopAsyncBuffers();
     Check("overlapping_second_download_stays_dirty",h.Dirty(4096+32,32));h.PopAsyncBuffers();
     Check("last_download_clears_dirty",!h.Dirty(4096,128));}
    {Harness h;h.async_buffers.emplace_back(std::nullopt);h.Queue(4096,128,0x22);h.PopAsyncBuffers();h.PopAsyncBuffers();
     Check("empty_async_batch_keeps_queues_aligned",h.device_memory.data[4096]==0x22 && h.pending_downloads.empty());}
}
#if PR_MODE
bool sparse_destination_used{};
u64 duplicate_copy_count{};
void TestSparse() {
#if FIXED_MODE
    {Harness t;auto canonical=t.AddBuffer(4096,8192), a=t.AddBuffer(0,4096),b=t.AddBuffer(0,4096);
     t.sparse_buffers.push_back({t.gpu_memory,0x10000,4096,4096,a,1,0,1,{{4096,4096,0,canonical,1}}});
     t.sparse_buffers.push_back({t.gpu_memory,0x20000,4096,4096,b,1,0,1,{{8192,4096,0,canonical,1}}});
     t.MarkBufferContentChanged(t.slot_buffers[canonical],4096+16,4);
     const auto version=t.slot_buffers[canonical].content_version;
     Check("partial_write_invalidates_overlapping_sparse_part",t.sparse_buffers[0].parts[0].content_version!=version);
     Check("partial_write_preserves_disjoint_sparse_part",t.sparse_buffers[1].parts[0].content_version==version);
     t.SynchronizeSparseBuffer(0x20000,4096);
     Check("unchanged_sparse_part_needs_no_full_copy",t.runtime.copy_calls==0);
     t.slot_buffers[canonical].data[16]=0x77;t.SynchronizeSparseBuffer(0x10000,4096);
     Check("changed_sparse_part_still_refreshes",t.runtime.copy_calls==1&&t.slot_buffers[a].data[16]==0x77);}
#endif
    Harness h;
    auto source=h.AddBuffer(4096,4096), a=h.AddBuffer(0,4096), b=h.AddBuffer(0,4096);
    auto sparse=[&](u64 addr,u32 id){return Harness::SparseBuffer{h.gpu_memory,addr,4096,4096,id,1,0,1,{{4096,4096,0,source,1}}};};
    h.sparse_buffers.push_back(sparse(0x10000,a));h.sparse_buffers.push_back(sparse(0x20000,b));
    h.MarkSparseWrite(0x10000,4096);
    std::fill(h.slot_buffers[a].data.begin(),h.slot_buffers[a].data.end(),0x55);
    h.CopySparseWrites();
    Check("sparse_write_is_copied_to_canonical_buffer",h.slot_buffers[source].data[0]==0x55);
    h.SynchronizeSparseBuffer(0x10000,4096);
    Check("writer_assembly_keeps_new_bytes",h.slot_buffers[a].data[0]==0x55);
    h.SynchronizeSparseBuffer(0x20000,4096);
    Check("other_sparse_alias_sees_new_bytes",h.slot_buffers[b].data[0]==0x55);
    Check("copyback_marks_canonical_buffer_usage",h.slot_buffers[source].usage_bytes>0);
    Check("contained_ssbo_alias_is_redirected",h.FindSparseAlias(Binding{4096+128,256},false)==0x10000+128);
    h.has_written_sparse=false;
    Check("read_only_ssbo_keeps_canonical_binding",h.FindSparseAlias(Binding{4096,4096},false)==0);
#if FIXED_MODE
    for(bool adjacent:{false,true}) {
        Harness t;auto canonical=t.AddBuffer(4096,4096), assembly=t.AddBuffer(0,4096);
        t.sparse_buffers.push_back({t.gpu_memory,0x10000,4096,4096,assembly,1,0,1,{{4096,4096,0,canonical,1}}});
        t.MarkSparseWrite(0x10000,1024);t.MarkSparseWrite(0x10000+(adjacent?1024:512),1024);
        u32 covered=adjacent?2048:1536;std::fill_n(t.slot_buffers[assembly].data.begin(),covered,0x55);t.CopySparseWrites();
        Check(adjacent?"adjacent_sparse_writes_merge":"overlapping_sparse_writes_merge",t.runtime.copy_calls==1&&t.runtime.copy_bytes==covered&&t.slot_buffers[canonical].data[covered-1]==0x55&&t.slot_buffers[canonical].data[covered]==0x11);
    }
    {Harness t;auto s1=t.AddBuffer(4096,4096),s2=t.AddBuffer(8192,4096),a1=t.AddBuffer(0,4096),a2=t.AddBuffer(0,4096);
     t.sparse_buffers.push_back({t.gpu_memory,0x10000,4096,4096,a1,1,0,1,{{4096,4096,0,s1,1}}});
     t.sparse_buffers.push_back({t.gpu_memory,0x11000,4096,4096,a2,1,0,1,{{8192,4096,0,s2,1}}});
     t.MarkSparseWrite(0x10000,4096);t.MarkSparseWrite(0x11000,4096);
     std::fill(t.slot_buffers[a1].data.begin(),t.slot_buffers[a1].data.end(),0x55);std::fill(t.slot_buffers[a2].data.begin(),t.slot_buffers[a2].data.end(),0x66);t.CopySparseWrites();
     Check("adjacent_allocations_are_not_merged",t.runtime.copy_calls==2&&t.slot_buffers[s1].data[0]==0x55&&t.slot_buffers[s2].data[0]==0x66);}
    {Harness t;auto s=t.AddBuffer(4096,4096),assembly=t.AddBuffer(0,4096);
     t.sparse_buffers.push_back({t.gpu_memory,0x10000,4096,4096,assembly,1,0,1,{{4096,1024,0,s,1},{5120,1024,2048,s,1}}});
     t.MarkSparseWrite(0x10000,4096);std::fill_n(t.slot_buffers[assembly].data.begin(),1024,0x55);std::fill_n(t.slot_buffers[assembly].data.begin()+2048,1024,0x66);t.CopySparseWrites();
     auto count=t.runtime.copy_calls;t.SynchronizeSparseBuffer(0x10000,4096);
     Check("parts_sharing_canonical_buffer_publish_final_version",t.runtime.copy_calls==count&&t.slot_buffers[s].data[0]==0x55&&t.slot_buffers[s].data[1024]==0x66&&t.slot_buffers[s].data[2048]==0x11);}
    {Harness t;auto s=t.AddBuffer(4096,4096),assembly=t.AddBuffer(0,4096);
     t.sparse_buffers.push_back({t.gpu_memory,0x10000,4096,4096,assembly,1,0,1,{{4096,4096,0,s,1}}});
     for(u32 i=0;i<16;++i)t.MarkSparseWrite(0x10000,4096);t.CopySparseWrites();
     duplicate_copy_count=t.runtime.copy_calls;sparse_destination_used=t.slot_buffers[s].usage_bytes!=0;
     Check("16_identical_writable_bindings_copy_once",duplicate_copy_count==1&&t.runtime.copy_bytes==4096);}
    {Harness t;auto s=t.AddBuffer(4096,4096),assembly=t.AddBuffer(0,4096);
     t.sparse_buffers.push_back({t.gpu_memory,0x10000,4096,4096,assembly,1,0,1,{{4096,4096,0,s,1}}});
     t.MarkSparseWrite(0x10000+128,4);t.CopySparseWrites();
     Check("four_byte_copyback_is_tracked_by_real_usage_tracker",t.slot_buffers[s].IsRegionUsed(128,4));}
    std::mt19937 rng{396};bool oracle_ok=true;
    for(u32 test=0;test<1500;++test) {
        Harness t;auto s=t.AddBuffer(4096,4096),assembly=t.AddBuffer(0,4096);std::array<bool,4096> written{};
        t.sparse_buffers.push_back({t.gpu_memory,0x10000,4096,4096,assembly,1,0,1,{{4096,4096,0,s,1}}});
        for(u32 j=0;j<16;++j){u32 start=rng()%4096,size=std::min(1+rng()%512,4096-start);t.MarkSparseWrite(0x10000+start,size);for(u32 k=start;k<start+size;++k){written[k]=true;t.slot_buffers[assembly].data[k]=0x55;}}
        t.CopySparseWrites();u64 groups=0;for(u32 i=0;i<4096;++i){oracle_ok&=t.slot_buffers[s].data[i]==(written[i]?0x55:0x11);if(written[i]&&(i==0||!written[i-1]))++groups;}
        oracle_ok&=t.runtime.copy_calls==groups;
    }
    Check("random_sparse_copyback_byte_and_interval_oracle_1500_cases",oracle_ok);
#endif
}
#endif
void TestDMA() {
    Settings::safe=true;Settings::dma_default=true;
    Check("completed_macro_does_not_disable_safe_fetch_of_next_gp_entry",DMAReadSafe(0xe01,0));
    Check("completed_compute_inline_does_not_disable_next_gp_entry",DMAReadSafe(0x6d,0,true));
    Settings::dma_default=false;
    Check("explicit_safe_dma_is_respected",DMAReadSafe(0xe01,16));
    Settings::dma_default=true;
    Check("ordinary_gpu_command_fetch_is_safe",DMAReadSafe(0x100,2));
    for(bool continuation:{false,true}) {
        Parser parser;parser.dma_state.method=0xe01;parser.dma_state.method_count=continuation?1:0;
        parser.dma_state.non_incrementing=true;
        CommandHeader stale{},fresh{},parameter{};
        stale.method.Assign(0x100);stale.mode.Assign(SubmissionMode::Inline);stale.arg_count.Assign(7);
        fresh=stale;fresh.arg_count.Assign(42);parameter.argument=0;
        bool safe=DMAReadSafe(0xe01,continuation?1:0,false,continuation?2:1);
        std::array<CommandHeader,2> words{parameter,safe?fresh:stale};
        parser.ProcessCommands(continuation?std::span<const CommandHeader>(words):std::span<const CommandHeader>(words).subspan(1));
        Check(continuation?"real_dma_parser_mixed_payload_and_next_command":"real_dma_parser_next_gp_entry",parser.written.back()==std::pair<u32,u32>{0x100,42});
    }
#if FIXED_MODE
    Check("whole_macro_parameter_entry_avoids_readback",!DMAReadSafe(0xe01,16,false,16));
    Check("whole_compute_parameter_entry_avoids_readback",!DMAReadSafe(0x6d,3,true,3));
    Check("incrementing_command_entry_keeps_safe_fetch",DMAReadSafe(0xe01,16,false,16,false));
#endif
}
void TestDescriptorsAndHazards() {
    Vulkan::DescriptorPayload<u64,4,2> payload;int waits=0;
    for(int i=0;i<1000;++i){payload.Acquire(3,[&]{++waits;});payload.Append(1);payload.Append(2);payload.Append(3);}
    Check("three_query_descriptors_fit_reserved_payload",payload.Data()[2]==3 && waits==999);
    bool threw=false;try{payload.Acquire(2,[]{});payload.Append(1);payload.Append(2);payload.Append(3);}catch(const std::length_error&){threw=true;}
    Check("descriptor_under_reservation_is_detected",threw);
    HazardTracker ht;ht.MarkBufferWrite(0);Check("null_buffer_does_not_create_hazard",ht.written_buffers.empty());
    for(u64 i=1;i<=64;++i)ht.MarkBufferWrite(i);
    Check("64_buffer_hazards_are_exact",ht.IsBufferWritten(1)&&ht.IsBufferWritten(64)&&!ht.IsBufferWritten(65));
    ht.MarkBufferWrite(65);Check("65th_buffer_falls_back_conservatively",ht.IsBufferWritten(1000)&&ht.written_buffers.size()==64);
    ht.written_buffers.clear();ht.all_buffers_written=false;Check("context_reset_clears_hazards",!ht.IsBufferWritten(1));
}
void TestUsageTracker() {
    VideoCommon::UsageTracker tracker(8192);
    tracker.Track(128,4);Check("usage_tracker_four_bytes",tracker.IsUsed(128,4));
    Check("usage_tracker_no_same_page_false_positive",!tracker.IsUsed(256,4));
    tracker.Reset();tracker.Track(4095,2);
    Check("usage_tracker_unaligned_page_boundary",tracker.IsUsed(4095,1)&&tracker.IsUsed(4096,1));
    tracker.Reset();tracker.Track(0,8192);Check("usage_tracker_full_pages",tracker.IsUsed(8191,1));
    tracker.Reset();tracker.Track(0,0);Check("usage_tracker_empty_range",!tracker.IsUsed(0,64)&&!tracker.IsUsed(0,0));
    std::mt19937 rng{396};bool valid=true;
    for(u32 i=0;i<5000;++i) {
        tracker.Reset();std::array<bool,128> bits{};
        for(u32 j=0;j<4;++j){u32 start=rng()%8192,size=std::min(1+rng()%2048,8192-start);tracker.Track(start,size);for(u32 k=start/64;k<=(start+size-1)/64;++k)bits[k]=true;}
        for(u32 j=0;j<8;++j){u32 start=rng()%8192,size=std::min(1+rng()%2048,8192-start);bool used=false;for(u32 k=start/64;k<=(start+size-1)/64;++k)used|=bits[k];valid&=tracker.IsUsed(start,size)==used;}
    }
    Check("usage_tracker_block_oracle_5000_cases",valid);
}
volatile u64 sink{};
template<class F> void Bench(const char* name,u64 iterations,F f) {
    std::array<double,7> samples{};
    for(auto& sample:samples) {
        auto start=std::chrono::steady_clock::now();
        for(u64 i=0;i<iterations;++i)f(i);
        auto end=std::chrono::steady_clock::now();
        sample=std::chrono::duration<double,std::nano>(end-start).count()/iterations;
    }
    std::sort(samples.begin(),samples.end());
    std::cout<<"BENCH "<<name<<" ns/op="<<samples[3]<<" min="<<samples.front()<<" max="<<samples.back()<<" iterations="<<iterations<<'\n';
}
void Benchmarks() {
    for(u32 count:{0,8,64}) {
        HazardTracker ht;for(u64 i=1;i<=count;++i)ht.MarkBufferWrite(i);
        std::string name="hazard_lookup_"+std::to_string(count);
        Bench(name.c_str(),1000000,[&](u64 i){sink=ht.IsBufferWritten(i%128+1);});
    }
    for(u32 depth:{1,8,64}) {
        Harness h;
        for(u32 i=0;i<depth;++i){h.Queue(4096,4096,0x22);}
        std::string name="cancel_download_depth_"+std::to_string(depth);
        Bench(name.c_str(),10000,[&](u64){h.ClearDownload(8192,16);});
    }
    for(u32 fragments:{0,16,256}) {
        Harness h;auto id=h.AddBuffer(4096,4096);auto& b=h.slot_buffers[id];
        for(u32 i=0;i<fragments;++i)h.gpu_modified_ranges.Add(4096+i*16,8);
        boost::container::small_vector<BufferCopy,4> copies;
        std::string name="upload_partition_fragments_"+std::to_string(fragments);
        Bench(name.c_str(),10000,[&](u64){h.memory_tracker.uploads={{4096,4096}};copies.clear();h.SynchronizeBuffer(b,4096,4096,copies);sink=copies.size();});
    }
#if PR_MODE
    for(u32 bindings:{1,16}) {
        Harness h;auto source=h.AddBuffer(4096,4096);auto a=h.AddBuffer(0,4096);
        h.sparse_buffers.push_back({h.gpu_memory,0x10000,4096,4096,a,1,0,1,{{4096,4096,0,source,1}}});
        std::string name="sparse_copyback_duplicate_bindings_"+std::to_string(bindings);
        Bench(name.c_str(),10000,[&](u64){for(u32 i=0;i<bindings;++i)h.MarkSparseWrite(0x10000,4096);h.CopySparseWrites();});
        std::cout<<"COPY_COST bindings="<<bindings<<" copies/dispatch="<<h.runtime.copy_calls/70000<<" bytes/dispatch="<<h.runtime.copy_bytes/70000<<'\n';
    }
#endif
}
int main(int argc,char** argv){
    std::cout<<"VARIANT "<<(FIXED_MODE?"CORRECTED":PR_MODE?"PR fa29fce":"MASTER c32270c")<<'\n';
    TestSynchronization();TestAsync();TestDMA();TestDescriptorsAndHazards();
#if FIXED_MODE
    TestUsageTracker();
#endif
#if PR_MODE
    TestSparse();
#endif
    Benchmarks();
    std::cout<<"RESULT tests="<<tests<<" passed="<<tests-failures<<" failed="<<failures<<'\n';
#if FIXED_MODE
    if(!failures&&argc>1){std::ofstream plan(argv[1]);plan<<"#define FIXED_SPARSE_USAGE "<<sparse_destination_used<<"\n#define FIXED_SPARSE_COPIES "<<duplicate_copy_count<<"\n";}
#endif
    return failures?1:0;
}
