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
    // A 4 MiB read of 64 KiB Zstandard chunks splits into up to 63 jobs.
    std::size_t prefetch_queue = 128;
    // A read that spans several Zstandard blocks hands the later ones to the
    // decode workers, which decode from the end while the caller decodes from
    // the start.
    bool split_reads = true;
    // Background decode threads for split reads and prefetch. 0 picks
    // min(4, hardware threads / 2), at least 1. 1 is the 1.0.0 behaviour.
    unsigned decode_workers = 0;
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
            options.lookahead_blocks > 2 || options.prefetch_queue > 256 || options.decode_workers > 16)
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
                options.prefetch_queue && options.reader.max_cached_blocks && options.reader.max_cached_block_bytes) {
                // On failure the session destructor stops and joins the
                // workers already started.
                session->workers_.reserve(session->worker_count());
                for (unsigned i=0;i<session->worker_count();++i)
                    session->workers_.emplace_back([p=session.get()] { p->work(); });
            }
            return Result<std::shared_ptr<Session>>::success(std::move(session));
        } catch (const std::bad_alloc&) {
            return Result<std::shared_ptr<Session>>::failure(ErrorCode::resource_limit,"persistent reader allocation failed");
        } catch (const std::system_error&) {
            return Result<std::shared_ptr<Session>>::failure(ErrorCode::resource_limit,"prefetch worker creation failed");
        }
    }

    ~Session() { stop(); for (auto& worker : workers_) if (worker.joinable()) worker.join(); }
    Session(const Session&)=delete;
    Session& operator=(const Session&)=delete;
    const Archive& archive() const noexcept { return archive_; }
    // Borrowed for source diagnostics; the source remains pinned by the session.
    RandomAccessSource& backing_source() const noexcept { return *source_; }
    std::uint64_t max_read_bytes() const noexcept { return options_.reader.max_read_bytes; }

    void stop() noexcept {
        stopped_.store(true,std::memory_order_release);
        { std::lock_guard<std::mutex> lock(queue_mutex_); cancelled_+=queue_.size(); queue_.clear(); }
        // Lock so a sleeper between its predicate test and wait() sees stopped_.
        { std::lock_guard<std::mutex> lock(gate_mutex_); }
        queue_cv_.notify_all(); gate_cv_.notify_all();
    }

    // Same contract as Archive::read_file(): fast by default (verified blocks
    // go straight to `output`; on failure `bytes` leading bytes are verified),
    // all-or-nothing with OpenOptions::atomic_reads (options.reader).
    PartialRead read(FileId file, std::uint64_t offset, std::size_t length, std::uint8_t* output,
                     std::size_t capacity, const std::shared_ptr<ReadCursor>& cursor = {}) {
        return demand(file,offset,length,output,capacity,cursor);
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
    PartialRead demand(FileId file, std::uint64_t offset, std::size_t length, std::uint8_t* output,
                       std::size_t capacity, const std::shared_ptr<ReadCursor>& cursor) {
        const auto failure=[](ErrorCode code,const char* message) { return PartialRead(Status::failure(code,message),0); };
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
            return failure(ErrorCode::resource_limit,"read buffer or configured read limit is too small");
        Admission admitted(*this,false);
        if (!admitted) return failure(ErrorCode::cancelled,"persistent reader stopped");
        if (options_.split_reads && length && !workers_.empty() && length<=capacity && split_helps()) {
            try { split(file,offset,length); }
            catch (const std::bad_alloc&) { std::lock_guard<std::mutex> lock(queue_mutex_); ++dropped_; }
        }
        PartialRead result=archive_.read_file(file,offset,length,output,capacity);
        if (result && cursor && length && !workers_.empty() && options_.sequential_prefetch && options_.lookahead_blocks) {
            // Speculation is optional: allocator pressure must not fail a
            // demand read whose verified bytes have already been committed.
            try { predict(file,offset,length,cursor); }
            catch (const std::bad_alloc&) { std::lock_guard<std::mutex> lock(queue_mutex_); ++dropped_; }
        }
        return result;
    }

    Session(std::shared_ptr<RandomAccessSource> source, SessionOptions options)
        : source_(std::move(source)), options_(options) {}
    unsigned worker_count() const noexcept {
        if (options_.decode_workers) return options_.decode_workers;
        const unsigned threads=std::thread::hardware_concurrency();
        return std::max(1u,std::min(4u,threads/2));
    }
    static bool cancelled(void* context) {
        auto& s=*static_cast<Session*>(context);
        return s.stopped_.load(std::memory_order_acquire) ||
            (s.options_.reader.is_cancelled && s.options_.reader.is_cancelled(s.options_.reader.cancellation_context));
    }
    // Slots are an atomic counter, so an uncontended read takes no lock. The
    // mutex and condition variable are only used to sleep when the limit is
    // reached. No lost wakeup: a sleeper raises `sleepers_` under the mutex
    // before it tests `active_`; a releaser lowers `active_` before it reads
    // `sleepers_` (both sequentially consistent), then locks before notifying.
    class Admission {
    public:
        Admission(Session& s,bool background):session_(s) {
            // Reserve a demand slot; prefetch never consumes the last one.
            const auto limit=background ? (s.options_.parallel_reads>1 ? s.options_.parallel_reads-1 : 1) : s.options_.parallel_reads;
            if (cancelled(&s)) return;
            if (try_take(limit,background)) return;
            std::unique_lock<std::mutex> lock(s.gate_mutex_);
            ++s.sleepers_;
            if (!background) ++s.waiting_demands_;
            s.gate_cv_.wait(lock,[&] { return cancelled(&s) || try_take(limit,background); });
            if (!background) --s.waiting_demands_;
            --s.sleepers_;
            if (entered_ && cancelled(&s)) { lock.unlock(); release(); }
        }
        ~Admission() { if (entered_) release(); }
        explicit operator bool() const { return entered_; }
    private:
        bool try_take(std::size_t limit,bool background) {
            auto& s=session_;
            auto active=s.active_.load();
            while (active<limit && (!background || !s.waiting_demands_.load())) {
                if (s.active_.compare_exchange_weak(active,active+1)) { entered_=true; return true; }
            }
            return false;
        }
        void release() {
            entered_=false;
            session_.active_.fetch_sub(1);
            if (session_.sleepers_.load()) {
                { std::lock_guard<std::mutex> lock(session_.gate_mutex_); }
                session_.gate_cv_.notify_all();
            }
        }
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
    // Queue the far part of a multi-block read, last block first, ahead of
    // speculative jobs. With W workers the caller keeps the first 1/(W+1) of
    // the blocks, so the caller and the workers finish together; a block the
    // caller reaches first is a cache hit for its worker, and a block a worker
    // is decoding makes the caller wait for it instead of decoding it twice.
    // Split only while callers (admitted reads minus busy workers, this one
    // included) do not outnumber the workers. Past that every core is already
    // decoding a demand, and queued far halves only delay other callers
    // (measured, 4 workers, 8 concurrent cold 1 MiB reads: p99 870 -> 610 us,
    // 64 KiB: 205 -> 118 us; 2 to 4 callers keep their split speedup).
    // Reads only atomics a demand already touches: no added write.
    bool split_helps() const {
        const auto active=active_.load(std::memory_order_relaxed), busy=busy_workers_.load(std::memory_order_relaxed);
        return (active>busy ? active-busy : 0)<=workers_.size();
    }
    void split(FileId file,std::uint64_t offset,std::size_t length) {
        const auto info=archive_.file_info(file);
        if (!info || offset>info.value->size || length>info.value->size-offset) return;
        const auto shift=info.value->chunk_log2;
        if ((1ull<<shift)>options_.reader.max_cached_block_bytes) return;
        const auto first=offset>>shift, last=(offset+length-1)>>shift;
        if (first==last) return;
        const auto keep=(last-first+1+workers_.size())/(workers_.size()+1);  // rounded up
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
        if (wanted.size()>1) queue_cv_.notify_all(); else queue_cv_.notify_one();
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
            if (!admitted) {
                // Only stop() ends the worker: a user cancellation callback
                // may clear later, so it just drops this speculative job.
                std::lock_guard<std::mutex> lock(queue_mutex_);
                if (stopped_) return;
                ++cancelled_; continue;
            }
            // Declared after `admitted`, so it ends first: callers are never
            // under-counted while this job still holds its slot.
            struct Busy {
                std::atomic<std::size_t>& count;
                explicit Busy(std::atomic<std::size_t>& c):count(c) { count.fetch_add(1,std::memory_order_relaxed); }
                ~Busy() { count.fetch_sub(1,std::memory_order_relaxed); }
            } busy(busy_workers_);
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
    std::atomic<std::size_t> active_{0},waiting_demands_{0},sleepers_{0},busy_workers_{0};
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<Job> queue_;
    std::vector<std::thread> workers_;
    std::uint64_t submitted_=0,completed_=0,cancelled_=0,failed_=0,dropped_=0;
};
} // namespace nx
#endif
