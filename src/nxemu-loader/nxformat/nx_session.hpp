#ifndef NX_SESSION_HPP_INCLUDED
#define NX_SESSION_HPP_INCLUDED

#include "nx_reader.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>

namespace nx {

struct SessionOptions {
    OpenOptions reader = [] {
        OpenOptions o;
        o.max_cached_block_bytes = 64ull * 1024 * 1024;
        o.max_cached_blocks = 1024;
        o.max_read_bytes = 4ull * 1024 * 1024;
        return o;
    }();
    std::size_t parallel_reads = 8;
    bool sequential_prefetch = true;
    unsigned sequential_reads = 3, lookahead_blocks = 1;
    std::size_t prefetch_queue = 16;
    // A read that spans several Zstandard blocks hands the later ones to the
    // worker, which decodes from the end while the caller decodes from the start.
    bool split_reads = true;
};

struct SessionStats {
    CacheStats cache;
    std::uint64_t prefetch_submitted = 0, prefetch_completed = 0;
    std::uint64_t prefetch_cancelled = 0, prefetch_failed = 0, prefetch_dropped = 0;
};

// One cursor per open file handle. Independent handles do not infer each other's
// access patterns. No cursor owns a Session, so queued work cannot create cycles.
struct ReadCursor {
    std::mutex mutex;
    std::atomic<std::uint64_t> generation{0};
    std::uint64_t end = 0;
    unsigned streak = 0;
    bool initialized = false;
    FileId file;
    std::uint64_t predicted = 0;
    bool has_prediction = false;
};

// Mount once before publication; all archive tables then remain immutable.
// Source, decoder callbacks and their contexts must support concurrent reads.
class Session {
public:
    static Result<std::shared_ptr<Session>> open(std::shared_ptr<RandomAccessSource> source,
                                               SessionOptions options = {}) {
        if (!source || !options.reader.verify_read_blocks || !options.parallel_reads ||
            options.parallel_reads > 64 || !options.sequential_reads ||
            options.lookahead_blocks > 2 || options.prefetch_queue > 256)
            return Result<std::shared_ptr<Session>>::failure(ErrorCode::invalid_argument,"invalid persistent reader options");
        try {
            auto session=std::shared_ptr<Session>(new Session(std::move(source),options));
            auto reader=options.reader;
            reader.collect_read_stats=true;
            reader.is_cancelled=&Session::cancelled;
            reader.cancellation_context=session.get();
            auto archive=Archive::open(*session->source_,reader);
            if (!archive) return Result<std::shared_ptr<Session>>::failure(archive.error.code,archive.error.message);
            session->archive_=std::move(*archive.value);
            auto mounted=session->archive_.mount();
            if (!mounted) return Result<std::shared_ptr<Session>>::failure(mounted.error.code,mounted.error.message);
            if (((options.sequential_prefetch && options.lookahead_blocks) || options.split_reads) &&
                options.prefetch_queue && options.reader.max_cached_blocks && options.reader.max_cached_block_bytes)
                session->worker_=std::thread([p=session.get()] { p->work(); });
            return Result<std::shared_ptr<Session>>::success(std::move(session));
        } catch (const std::bad_alloc&) {
            return Result<std::shared_ptr<Session>>::failure(ErrorCode::resource_limit,"persistent reader allocation failed");
        } catch (const std::system_error&) {
            return Result<std::shared_ptr<Session>>::failure(ErrorCode::resource_limit,"prefetch worker creation failed");
        }
    }

    ~Session() { stop(); if (worker_.joinable()) worker_.join(); }
    Session(const Session&)=delete;
    Session& operator=(const Session&)=delete;
    const Archive& archive() const noexcept { return archive_; }
    // Borrowed for source diagnostics; the source remains pinned by the session.
    RandomAccessSource& backing_source() const noexcept { return *source_; }
    std::uint64_t max_read_bytes() const noexcept { return options_.reader.max_read_bytes; }

    void stop() noexcept {
        stopped_.store(true,std::memory_order_release);
        { std::lock_guard<std::mutex> lock(queue_mutex_); cancelled_+=queue_.size(); queue_.clear(); }
        queue_cv_.notify_all(); gate_cv_.notify_all();
    }

    Status read(FileId file, std::uint64_t offset, std::size_t length, std::uint8_t* output,
                std::size_t capacity, const std::shared_ptr<ReadCursor>& cursor = {}) {
        if (cursor) {
            std::lock_guard<std::mutex> lock(cursor->mutex);
            // Invalidate old speculation when the seek is requested, before
            // waiting for admission/IO, even if the new demand later fails.
            if (cursor->initialized && (cursor->file.value!=file.value || cursor->end!=offset)) {
                ++cursor->generation; cursor->has_prediction=false;
            }
        }
        // Reject invalid demand buffers before scheduling any background IO.
        if (length>capacity || (length && !output) || length>options_.reader.max_read_bytes)
            return Status::failure(ErrorCode::resource_limit,"read buffer or configured read limit is too small");
        Admission admitted(*this,false);
        if (!admitted) return Status::failure(ErrorCode::cancelled,"persistent reader stopped");
        if (options_.split_reads && length && worker_.joinable() && length<=capacity) {
            try { split(file,offset,length); }
            catch (const std::bad_alloc&) { std::lock_guard<std::mutex> lock(queue_mutex_); ++dropped_; }
        }
        auto result=archive_.read_file(file,offset,length,output,capacity);
        if (result && cursor && length && worker_.joinable() && options_.sequential_prefetch && options_.lookahead_blocks) {
            // Speculation is optional: allocator pressure must not fail a
            // demand read whose verified bytes have already been committed.
            try { predict(file,offset,length,cursor); }
            catch (const std::bad_alloc&) { std::lock_guard<std::mutex> lock(queue_mutex_); ++dropped_; }
        }
        return result;
    }

    // Independent verification deliberately bypasses the verified block cache.
    Status verify_all() {
        Admission admitted(*this,false);
        return admitted ? archive_.verify_all() : Status::failure(ErrorCode::cancelled,"persistent reader stopped");
    }
    Result<std::vector<std::uint8_t>> media(std::uint32_t index) {
        Admission admitted(*this,false);
        return admitted ? archive_.read_media(index) : Result<std::vector<std::uint8_t>>::failure(ErrorCode::cancelled,"persistent reader stopped");
    }
    SessionStats stats() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return {archive_.cache_stats(),submitted_,completed_,cancelled_,failed_,dropped_};
    }

private:
    Session(std::shared_ptr<RandomAccessSource> source, SessionOptions options)
        : source_(std::move(source)), options_(options) {}
    static bool cancelled(void* context) {
        auto& s=*static_cast<Session*>(context);
        return s.stopped_.load(std::memory_order_acquire) ||
            (s.options_.reader.is_cancelled && s.options_.reader.is_cancelled(s.options_.reader.cancellation_context));
    }
    class Admission {
    public:
        Admission(Session& s,bool background):session_(s) {
            std::unique_lock<std::mutex> lock(s.gate_mutex_);
            // Reserve a demand slot; prefetch never consumes the last one.
            const auto limit=background ? (s.options_.parallel_reads>1 ? s.options_.parallel_reads-1 : 1) : s.options_.parallel_reads;
            if (!background) ++s.waiting_demands_;
            s.gate_cv_.wait(lock,[&] { return cancelled(&s) ||
                (s.active_<limit && (!background || !s.waiting_demands_)); });
            if (!background) --s.waiting_demands_;
            if (!cancelled(&s)) { ++s.active_; entered_=true; }
        }
        ~Admission() { if (entered_) { {std::lock_guard<std::mutex> lock(session_.gate_mutex_); --session_.active_;} session_.gate_cv_.notify_all(); } }
        explicit operator bool() const { return entered_; }
    private:
        Session& session_;
        bool entered_=false;
    };
    struct Job { FileId file; std::uint64_t offset, generation; std::shared_ptr<ReadCursor> cursor; };
    void predict(FileId file,std::uint64_t offset,std::size_t length,const std::shared_ptr<ReadCursor>& cursor) {
        const auto info=archive_.file_info(file);
        if (!info) return;
        std::lock_guard<std::mutex> state(cursor->mutex);
        const bool sequential=cursor->initialized && cursor->file.value==file.value && cursor->end==offset;
        if (!sequential) { ++cursor->generation; cursor->streak=1; cursor->has_prediction=false; }
        else if (cursor->streak<options_.sequential_reads) ++cursor->streak;
        cursor->initialized=true; cursor->file=file; cursor->end=offset+length;
        if (cursor->streak<options_.sequential_reads) return;
        const auto chunk=1ull<<info.value->chunk_log2;
        if (chunk>options_.reader.max_cached_block_bytes) return;
        const auto next=((cursor->end-1)/chunk+1)*chunk;
        if (cursor->has_prediction && cursor->predicted==next) return;
        std::lock_guard<std::mutex> lock(queue_mutex_);
        // Coalesce this handle's queued prediction; discard stale generations.
        for (auto it=queue_.begin();it!=queue_.end();) {
            if (it->cursor==cursor) { ++cancelled_; it=queue_.erase(it); } else ++it;
        }
        for (unsigned i=0;i<options_.lookahead_blocks;++i) {
            const auto at=next+std::uint64_t(i)*chunk;
            if (at>=info.value->size || stopped_) break;
            // RAW blocks are read per slice and never cached: nothing to warm.
            if (!archive_.block_needs_decode(file,at)) continue;
            if (queue_.size()==options_.prefetch_queue) { ++dropped_; break; }
            queue_.push_back({file,at,cursor->generation.load(),cursor}); ++submitted_;
        }
        cursor->has_prediction=true; cursor->predicted=next;
        queue_cv_.notify_one();
    }
    // Queue the far half of a multi-block read, last block first, ahead of
    // speculative jobs. The caller and the worker meet in the middle; a block
    // the caller reaches first is a cache hit for the worker.
    void split(FileId file,std::uint64_t offset,std::size_t length) {
        const auto info=archive_.file_info(file);
        if (!info || offset>info.value->size || length>info.value->size-offset) return;
        const auto shift=info.value->chunk_log2;
        if ((1ull<<shift)>options_.reader.max_cached_block_bytes) return;
        const auto first=offset>>shift, last=(offset+length-1)>>shift;
        if (first==last) return;
        const auto keep=(last-first+2)/2;  // the caller decodes the first half, rounded up
        std::vector<std::uint64_t> wanted;
        for (auto b=last; b>=first+keep && b>first; --b)
            if (archive_.block_needs_decode(file,b<<shift)) wanted.push_back(b<<shift);
        if (wanted.empty()) return;
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (stopped_) return;
        for (auto it=wanted.rbegin(); it!=wanted.rend(); ++it) {
            if (queue_.size()==options_.prefetch_queue) { ++dropped_; queue_.pop_back(); }
            queue_.push_front({file,*it,0,{}}); ++submitted_;
        }
        queue_cv_.notify_one();
    }
    void work() noexcept {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait(lock,[&] { return stopped_ || !queue_.empty(); });
                if (stopped_) return;
                job=std::move(queue_.front()); queue_.pop_front();
            }
            Admission admitted(*this,static_cast<bool>(job.cursor));
            if (!admitted) return;
            if (job.cursor && job.cursor->generation.load()!=job.generation) {
                std::lock_guard<std::mutex> lock(queue_mutex_); ++cancelled_; continue;
            }
            // One-byte read warms/verifies the entire physical block using the
            // same LRU/coalescing path as demand. No speculative bytes escape.
            std::uint8_t byte=0;
            auto result=archive_.read_file(job.file,job.offset,1,&byte,1);
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if ((job.cursor && job.cursor->generation.load()!=job.generation) || stopped_) ++cancelled_;
            else if (result) ++completed_; else ++failed_;
        }
    }
    std::shared_ptr<RandomAccessSource> source_;
    SessionOptions options_;
    Archive archive_;
    std::atomic<bool> stopped_{false};
    std::mutex gate_mutex_;
    std::condition_variable gate_cv_;
    std::size_t active_=0,waiting_demands_=0;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<Job> queue_;
    std::thread worker_;
    std::uint64_t submitted_=0,completed_=0,cancelled_=0,failed_=0,dropped_=0;
};
} // namespace nx
#endif
