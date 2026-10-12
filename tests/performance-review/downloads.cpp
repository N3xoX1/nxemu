// Exact production planning/retirement, production page tracker and range sets.
// GPU execution and guest RAM are deterministic doubles with strict bounds checks.
#define main PriorCpuMain
#include "../pr396/synthetic.cpp"
#undef main
#include "yuzu_video_core/buffer_cache/memory_tracker_base.h"
#include "yuzu_common/readback_reference.h"

struct DownloadBuffer {
    u64 base{}; std::vector<u8> data;
    std::shared_ptr<Common::ReadbackReference> reference;
    std::vector<u8> shadow;
    std::shared_ptr<Common::ReadbackCpuReference> cpu_reference;
    u64 CpuAddr() const {return base;}
    u64 SizeBytes() const {return data.size();}
    void MarkUsage(u64 offset,u64 size) {
        if(offset>data.size() || size>data.size()-offset) throw std::runtime_error("out of bounds usage");
    }
};
struct PageDevice {
    void UpdatePagesCachedCount(u64,u64,int) {}
};
struct DownloadStaging {
    std::vector<u8>* buffer{}; std::span<u8> mapped_span; size_t offset{};
};
struct DownloadRuntime {
    std::deque<std::vector<u8>> storage;
    u64 copy_bytes{},copy_calls{};
    bool gpu_delta{},has_references{};
    bool HasReadbackReferences() const { return has_references; }
    std::shared_ptr<Common::ReadbackCpuReference> GetReadbackCpuReference(const DownloadBuffer& b) const { return b.cpu_reference; }
    void InvalidateReadbackReference(DownloadBuffer& b,u64 offset,u64 size) {
        if(b.reference)b.reference->Invalidate(offset,size);
    }
    bool CanDifferentialReadback(const DownloadBuffer&,const BufferCopy& c) const {
        return gpu_delta&&c.size>=1048576&&c.size<=Common::ReadbackReference::MaxCopySize&&
               ((c.src_offset|c.dst_offset|c.size)&3)==0;
    }
    bool DifferentialReadback(DownloadBuffer& b,const DownloadStaging& data,
                             const DownloadStaging& flags,const BufferCopy& c,u64 flag_offset) {
        if(!CanDifferentialReadback(b,c))return false;
        if(!b.reference){b.reference=std::make_shared<Common::ReadbackReference>();b.shadow.resize(b.data.size());b.cpu_reference=std::make_shared<Common::ReadbackCpuReference>(b.data.size());has_references=true;}
        const auto valid=b.reference->ValidMask(c.src_offset,c.size);
        for(u64 offset=0,block=0;offset<c.size;offset+=65536,++block) {
            const u64 n=std::min<u64>(65536,c.size-offset);
            const bool known=(valid[block/32]&(1u<<(block%32)))!=0;
            const u32 changed=!known||!std::equal(b.data.begin()+c.src_offset+offset,b.data.begin()+c.src_offset+offset+n,b.shadow.begin()+c.src_offset+offset);
            const u64 flag_at=flags.offset+flag_offset+block*4;
            if(flag_at+4>flags.buffer->size()||c.dst_offset+offset+n>data.buffer->size())throw std::runtime_error("delta stage bounds");
            std::memcpy(flags.buffer->data()+flag_at,&changed,4);
            if(changed){copy_bytes+=n;std::copy_n(b.data.begin()+c.src_offset+offset,n,data.buffer->begin()+c.dst_offset+offset);std::copy_n(b.data.begin()+c.src_offset+offset,n,b.shadow.begin()+c.src_offset+offset);}
        }
        b.reference->Scheduled(c.src_offset,c.size);return true;
    }
    DownloadStaging DownloadStagingBuffer(u64 size,bool) {
        storage.emplace_back(size+64,0x77);
        auto& bytes=storage.back();
        return {&bytes,std::span<u8>{bytes}.subspan(64),64};
    }
    void PreCopyBarrier() {}
    void PostCopyBarrier() {}
    template<class Copies> void CopyBuffer(std::vector<u8>* dst,DownloadBuffer& src,const Copies& copies,bool) {
        ++copy_calls;
        for(auto c:copies) {
            if(c.src_offset>src.data.size() || c.size>src.data.size()-c.src_offset ||
               c.dst_offset>dst->size() || c.size>dst->size()-c.dst_offset)
                throw std::runtime_error("out of bounds GPU copy");
            copy_bytes+=c.size;
            std::copy_n(src.data.begin()+c.src_offset,c.size,dst->begin()+c.dst_offset);
        }
    }
};
struct DownloadHarness {
    static constexpr bool IS_OPENGL=false;
    using Buffer=DownloadBuffer;
    using Async_Buffer=DownloadStaging;
    Memory device_memory;
    PageDevice pages;
    VideoCommon::MemoryTrackerBase<PageDevice> memory_tracker{pages};
    DownloadRuntime runtime;
    std::deque<Buffer> slot_buffers{Buffer{}};
    Common::RangeSet<DAddr> gpu_modified_ranges,uncommitted_gpu_modified_ranges,downloaded_ranges;
    std::unique_ptr<VideoCommon::GpuDirtySummary<>> gpu_dirty_summary{VideoCommon::MakeGpuDirtySummary()};
    std::deque<Common::RangeSet<DAddr>> committed_gpu_modified_ranges,pending_download_ranges;
    Common::OverlapRangeSet<DAddr> async_downloads;
    std::deque<std::optional<Async_Buffer>> async_buffers;
    std::deque<boost::container::small_vector<BufferCopy,4>> pending_downloads;
    std::deque<std::optional<Async_Buffer>> pending_readback_flags;
    std::deque<std::vector<u64>> pending_readback_flag_offsets;
    std::deque<std::vector<Common::ReadbackCpuCopy>> pending_readback_cpu_copies;
    std::deque<Async_Buffer> async_buffers_death_ring;
    void AddBuffer(u64 base,u64 size) {
        slot_buffers.push_back({base,std::vector<u8>(size)});
        for(u64 i=0;i<size;++i) slot_buffers.back().data[i]=static_cast<u8>((base+i)*17+43);
        memory_tracker.ForEachUploadRange(base,size,[](u64,u64){});
    }
    void Mark(u64 begin,u64 size) {
        if(gpu_dirty_summary)gpu_dirty_summary->Mark(begin,size);
        gpu_modified_ranges.Add(begin,size);uncommitted_gpu_modified_ranges.Add(begin,size);
        memory_tracker.MarkRegionAsGpuModified(begin,size);
    }
    template<class F> void ForEachBufferInRange(u64 begin,u64 size,F&& callback) {
        for(u32 i=1;i<slot_buffers.size();++i) {
            auto& b=slot_buffers[i];if(b.base<begin+size && begin<b.base+b.data.size()) callback(i,b);
        }
    }
    void AccumulateFlushes(); void CommitAsyncFlushesHigh(); void CommitBaseline();
    void PopAsyncBuffers(); void ClearDownload(DAddr,u64);
    void InvalidateReadbackReferences(DAddr,u64);
};
#include "current.inc"
#include "baseline.inc"

void SamePage() {
    DownloadHarness current,baseline;
    for(auto* h:{&current,&baseline}) {
        h->AddBuffer(4096,4096);
        for(u64 i=0;i<64;++i) h->Mark(4096+i*64,4);
    }
    current.CommitAsyncFlushesHigh();baseline.CommitBaseline();
    Check("baseline_reproduces_64fold_repeated_bytes",baseline.runtime.copy_bytes==64*64*4);
    Check("current_copies_each_written_byte_once",current.runtime.copy_bytes==64*4);
    Check("current_uses_64_copies_instead_of_4096",current.runtime.copy_calls==64 && baseline.runtime.copy_calls==4096);
    current.PopAsyncBuffers();baseline.PopAsyncBuffers();
    Check("same_page_final_guest_bytes_equal_baseline",current.device_memory.data==baseline.device_memory.data);
    Check("unaligned_staging_offset_is_preserved",current.device_memory.data[4096]==current.slot_buffers[1].data[0]);
}
void Boundary() {
    DownloadHarness h;
    h.AddBuffer(4096,2048);h.AddBuffer(6144,2048);
    h.Mark(6000,32);h.Mark(7000,32);
    h.CommitAsyncFlushesHigh();h.PopAsyncBuffers();
    Check("page_shared_by_buffers_has_no_out_of_bounds_copy",h.runtime.copy_bytes==64);
    Check("first_buffer_boundary_bytes_match",h.device_memory.data[6000]==h.slot_buffers[1].data[1904]);
    Check("second_buffer_boundary_bytes_match",h.device_memory.data[7000]==h.slot_buffers[2].data[856]);
    bool reproduced=false;
    try {DownloadHarness old;old.AddBuffer(4096,2048);old.AddBuffer(6144,2048);old.Mark(6000,32);old.Mark(7000,32);old.CommitBaseline();}
    catch(const std::runtime_error&) {reproduced=true;}
    Check("baseline_reproduces_cross_buffer_page_overread",reproduced);
}
void FullAndEmpty() {
    DownloadHarness h;h.AddBuffer(4096,16384);h.Mark(4096,16384);
    h.CommitAsyncFlushesHigh();h.PopAsyncBuffers();
    Check("full_buffer_multiplage_download_keeps_every_byte",std::equal(h.slot_buffers[1].data.begin(),h.slot_buffers[1].data.end(),h.device_memory.data.begin()+4096));
    Check("full_buffer_download_has_exact_byte_count",h.runtime.copy_bytes==16384);
    DownloadHarness empty;empty.CommitAsyncFlushesHigh();empty.PopAsyncBuffers();
    Check("empty_commit_keeps_retirement_queues_aligned",empty.async_buffers.empty()&&empty.pending_downloads.empty()&&empty.pending_download_ranges.empty());
    DownloadHarness untracked;untracked.AddBuffer(4096,8192);untracked.Mark(4096,8192);
    untracked.memory_tracker.MarkRegionAsCpuModified(4096,4096);
    untracked.CommitAsyncFlushesHigh();untracked.PopAsyncBuffers();
    Check("CPU_untracked_page_is_excluded_from_download",untracked.runtime.copy_bytes==4096&&untracked.device_memory.data[4096]==0x11&&untracked.device_memory.data[8192]==untracked.slot_buffers[1].data[4096]);
}
void TrackerBoundaries() {
    constexpr u64 bytes=128ULL<<20;
    DownloadHarness current,baseline;
    for(auto* h:{&current,&baseline}) {
        h->device_memory.data.resize(bytes+8192,0x11);
        h->AddBuffer(4096,bytes);h->Mark(4096,bytes);
    }
    current.CommitAsyncFlushesHigh();baseline.CommitBaseline();
    Check("real_tracker_splits_128MiB_at_33_manager_boundaries",baseline.runtime.copy_calls==33);
    Check("contiguous_tracker_managers_become_one_128MiB_transfer",current.runtime.copy_calls==1&&current.pending_downloads.front().size()==1&&current.pending_downloads.front()[0].size==bytes);
    Check("merged_plan_keeps_exact_readback_byte_count",current.runtime.copy_bytes==bytes&&baseline.runtime.copy_bytes==bytes);
    const u64 cancelled=4096+(4ULL<<20)-32;
    current.ClearDownload(cancelled,128);baseline.ClearDownload(cancelled,128);
    std::fill_n(current.device_memory.data.begin()+cancelled,128,0xE3);
    std::fill_n(baseline.device_memory.data.begin()+cancelled,128,0xE3);
    current.PopAsyncBuffers();baseline.PopAsyncBuffers();
    Check("merged_retirement_matches_baseline_across_cancelled_manager_boundary",current.device_memory.data==baseline.device_memory.data&&current.device_memory.data[cancelled]==0xE3);
    DownloadHarness separate;
    separate.device_memory.data.resize(8ULL<<20,0x11);
    separate.AddBuffer(4096,4ULL<<20);separate.AddBuffer(4096+(4ULL<<20),2ULL<<20);
    separate.Mark(4096,6ULL<<20);separate.CommitAsyncFlushesHigh();
    Check("adjacent_canonical_buffers_are_never_combined",separate.runtime.copy_calls==2);
    DownloadHarness gap;gap.AddBuffer(4096,65536);
    gap.Mark(4096,4096);gap.Mark(4096+8192,4096);gap.CommitAsyncFlushesHigh();
    Check("dirty_range_gap_remains_separate_after_manager_coalescing",gap.runtime.copy_calls==2);
    DownloadHarness padding;padding.AddBuffer(4096,8192);
    padding.Mark(4096,4096);padding.AccumulateFlushes();
    padding.Mark(4096+1,1);padding.CommitAsyncFlushesHigh();
    Check("unaligned_staging_padding_is_not_combined",padding.runtime.copy_bytes==4096&&padding.runtime.copy_calls>=2);
}
void Oracle() {
    std::mt19937 rng{0x5070C0DE};
    bool bytes_ok=true,once_ok=true,cancel_ok=true,newer_ok=true;
    for(u32 round=0;round<300;++round) {
        DownloadHarness h;h.AddBuffer(4096,16384);
        std::array<bool,16384> dirty{},cancelled{};
        for(u32 i=0;i<80;++i) {
            const u64 offset=rng()%dirty.size(), size=std::min<u64>(1+rng()%384,dirty.size()-offset);
            h.Mark(4096+offset,size);for(u64 j=offset;j<offset+size;++j) dirty[j]=true;
            if(i%7==0)h.AccumulateFlushes();
        }
        h.CommitAsyncFlushesHigh();
        std::array<u32,16384> visits{};
        for(auto c:h.pending_downloads.front()) for(u64 j=c.src_offset-4096;j<c.src_offset-4096+c.size;++j) ++visits.at(j);
        for(u64 j=0;j<dirty.size();++j) once_ok &= visits[j]==u32{dirty[j]};
        for(u32 i=0;i<20;++i) {
            const u64 offset=rng()%dirty.size(),size=std::min<u64>(1+rng()%128,dirty.size()-offset);
            h.ClearDownload(4096+offset,size);
            for(u64 j=offset;j<offset+size;++j) {cancelled[j]=true;h.device_memory.data[4096+j]=0xE3;}
        }
        const u64 newer=4096+rng()%dirty.size();h.Mark(newer,1);
        h.PopAsyncBuffers();
        for(u64 j=0;j<dirty.size();++j) {
            const auto expected=cancelled[j]?0xE3:dirty[j]?h.slot_buffers[1].data[j]:0x11;
            bytes_ok &= h.device_memory.data[4096+j]==expected;
            if(cancelled[j])cancel_ok &= h.device_memory.data[4096+j]==0xE3;
        }
        bool kept=false;h.gpu_modified_ranges.ForEachInRange(newer,1,[&](u64,u64){kept=true;});newer_ok &= kept;
    }
    Check("300_random_batched_write_plans_match_byte_union_once",once_ok);
    Check("300_random_retirements_match_independent_byte_oracle",bytes_ok);
    Check("CPU_writes_after_commit_are_never_overwritten",cancel_ok);
    Check("newer_GPU_writes_remain_dirty_after_old_retirement",newer_ok);
}
void DeltaEpochs() {
    constexpr u64 base=4096,bytes=2ULL<<20;
    DownloadHarness h;h.runtime.gpu_delta=true;h.device_memory.data.resize(bytes+8192,0x11);h.AddBuffer(base,bytes);
    h.Mark(base,bytes);h.CommitAsyncFlushesHigh();
    auto first=h.slot_buffers[1].data;
    h.slot_buffers[1].data[65536+17]^=0x7F;auto second=h.slot_buffers[1].data;
    h.Mark(base,bytes);h.CommitAsyncFlushesHigh();
    Check("delta_first_reference_is_forced_full_and_second_epoch_is_one_chunk",h.runtime.copy_bytes==bytes+65536);
    h.PopAsyncBuffers();
    Check("delta_fifo_first_epoch_matches_snapshot_before_newer_write",std::equal(first.begin(),first.end(),h.device_memory.data.begin()+base));
    h.PopAsyncBuffers();
    Check("delta_fifo_second_epoch_matches_newer_snapshot",std::equal(second.begin(),second.end(),h.device_memory.data.begin()+base));
    const auto before=h.runtime.copy_bytes;h.Mark(base,bytes);h.CommitAsyncFlushesHigh();h.PopAsyncBuffers();
    Check("delta_unchanged_epoch_uses_no_payload_transfer",h.runtime.copy_bytes==before);
    h.Mark(base,bytes);h.CommitAsyncFlushesHigh();
    h.device_memory.data[base+12345]^=0xAA; // Simulate a texture/query write, without WriteMemory.
    h.PopAsyncBuffers();
    Check("delta_unchanged_GPU_epoch_restores_RAM_changed_by_other_cache",h.runtime.copy_bytes==before&&std::equal(second.begin(),second.end(),h.device_memory.data.begin()+base));
    h.slot_buffers[1].data[65536-3]^=0x5F;h.Mark(base,bytes);h.CommitAsyncFlushesHigh();
    const u64 cancel=base+65536-5;h.ClearDownload(cancel,12);
    std::fill_n(h.device_memory.data.begin()+cancel,12,0xA5);
    auto expected=h.slot_buffers[1].data;std::fill_n(expected.begin()+cancel-base,12,0xA5);
    h.PopAsyncBuffers();
    Check("delta_retirement_preserves_cancelled_bytes_across_chunk_boundary",std::equal(expected.begin(),expected.end(),h.device_memory.data.begin()+base));
    std::copy_n(expected.begin()+cancel-base,12,h.slot_buffers[1].data.begin()+cancel-base);
    h.slot_buffers[1].data[cancel-base+3]=0xD7;expected=h.slot_buffers[1].data;
    const auto prior=h.runtime.copy_bytes;h.Mark(base,bytes);h.CommitAsyncFlushesHigh();h.PopAsyncBuffers();
    Check("delta_CPU_invalidation_forces_both_intersecting_chunks",h.runtime.copy_bytes-prior==131072);
    Check("delta_next_GPU_write_after_CPU_cancel_matches_complete_copy",std::equal(expected.begin(),expected.end(),h.device_memory.data.begin()+base));
    Check("delta_all_payload_flag_and_retirement_queues_finish_aligned",h.async_buffers.empty()&&h.pending_downloads.empty()&&h.pending_download_ranges.empty()&&h.pending_readback_flags.empty()&&h.pending_readback_flag_offsets.empty()&&h.pending_readback_cpu_copies.empty());
    DownloadHarness late;late.runtime.gpu_delta=true;late.device_memory.data.resize(bytes+8192,0x11);late.AddBuffer(base,bytes);
    late.Mark(base,bytes);late.CommitAsyncFlushesHigh();late.Mark(base,bytes);late.CommitAsyncFlushesHigh();
    late.PopAsyncBuffers();late.device_memory.data[base+78651]^=0xB7;late.PopAsyncBuffers();
    Check("delta_two_queued_epochs_restore_intervening_untracked_RAM_change",std::equal(late.slot_buffers[1].data.begin(),late.slot_buffers[1].data.end(),late.device_memory.data.begin()+base));
    DownloadHarness removed;removed.runtime.gpu_delta=true;removed.device_memory.data.resize(bytes+8192,0x11);removed.AddBuffer(base,bytes);
    const auto original=removed.slot_buffers[1].data;removed.Mark(base,bytes);removed.CommitAsyncFlushesHigh();removed.slot_buffers.pop_back();removed.PopAsyncBuffers();
    Check("delta_CPU_snapshot_survives_canonical_buffer_removal",std::equal(original.begin(),original.end(),removed.device_memory.data.begin()+base));
    DownloadHarness mixed;mixed.runtime.gpu_delta=true;mixed.device_memory.data.resize(bytes+65536+8192,0x11);
    mixed.AddBuffer(base,bytes);mixed.AddBuffer(base+bytes,32768);
    mixed.Mark(base,bytes+32768);mixed.CommitAsyncFlushesHigh();mixed.PopAsyncBuffers();
    const auto seeded=mixed.runtime.copy_bytes;
    mixed.slot_buffers[2].data[16]^=1;mixed.Mark(base,bytes+32768);mixed.CommitAsyncFlushesHigh();mixed.PopAsyncBuffers();
    Check("delta_mixed_GPU_and_full_fallback_offsets_remain_aligned",mixed.runtime.copy_bytes-seeded==32768&&mixed.device_memory.data[base+bytes+16]==mixed.slot_buffers[2].data[16]);
    Common::ReadbackReference reference;
    reference.Scheduled(4096,8ULL<<20);
    auto mask=reference.ValidMask(4096,(8ULL<<20)-12);
    bool all=true;for(auto word:mask)all&=word==0xFFFFFFFF;
    Check("delta_reference_tracks_128_chunks_and_partial_last_chunk",all);
    reference.Invalidate(4096+31*65536+65530,12);
    mask=reference.ValidMask(4096,(8ULL<<20)-12);
    Check("delta_invalidation_crossing_mask_word_boundary_clears_both_proofs",(mask[0]&(1u<<31))==0&&(mask[1]&1)==0);
}

void CurrentDirtyRetirement(){
    DownloadHarness h;h.device_memory.data.resize(3ULL<<20,0x11);h.AddBuffer(4096,2ULL<<20);
    h.gpu_dirty_summary=std::make_unique<VideoCommon::GpuDirtySummary<>>();h.runtime.gpu_delta=true;
    h.Mark(4096,2ULL<<20);h.CommitAsyncFlushesHigh();
    constexpr u64 future=4096+33000;h.slot_buffers[1].data[33000]^=0x13;h.Mark(future,1);
    h.PopAsyncBuffers();
    Check("actual_retirement_keeps_newer_GPU_epoch_summary_positive",h.gpu_dirty_summary->MayHaveWrites(future,1));
    Check("actual_retirement_clears_clean_neighbour_pages",!h.gpu_dirty_summary->MayHaveWrites(4096+131072,1));
    h.CommitAsyncFlushesHigh();h.PopAsyncBuffers();
    Check("actual_final_retirement_publishes_clean_after_payload",!h.gpu_dirty_summary->MayHaveWrites(4096,2ULL<<20)&&h.device_memory.data[future]==h.slot_buffers[1].data[33000]);
}
void Scaling() {
    for(u32 n:{1U,8U,64U})for(bool baseline:{true,false}) {
        const auto label=std::string(baseline?"baseline_":"current_")+std::to_string(n)+"_same_page_writes";
        DownloadHarness h;h.AddBuffer(4096,4096);
        Bench(label.c_str(),200,[&](u64) {
            h.async_buffers.clear();h.pending_downloads.clear();h.pending_download_ranges.clear();
            h.pending_readback_flags.clear();h.pending_readback_flag_offsets.clear();
            h.pending_readback_cpu_copies.clear();
            h.async_downloads.Clear();h.runtime.storage.clear();h.runtime.copy_bytes=0;h.runtime.copy_calls=0;
            for(u64 i=0;i<n;++i)h.Mark(4096+i*64,4);
            if(baseline)h.CommitBaseline();else h.CommitAsyncFlushesHigh();
            sink=h.runtime.copy_bytes;
        });
    }
}
#ifndef DOWNLOAD_NO_MAIN
int main() try {
    SamePage();Boundary();FullAndEmpty();TrackerBoundaries();Oracle();DeltaEpochs();CurrentDirtyRetirement();Scaling();
    std::cout<<"FAILURES="<<failures<<'\n';return failures?1:0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 2;}
#endif
