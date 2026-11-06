// Optional logical VFS observations. No payload data, no wire-format changes.
#ifndef NX_READ_TRACE_HPP
#define NX_READ_TRACE_HPP
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace nx {
class ReadTrace {
public:
    struct Ticket { uint64_t sequence=0,start=0; };
    struct Event { uint64_t sequence,file,handle,thread,offset,bytes,start,latency; };
    struct Stats { uint64_t accepted=0,dropped=0,queue_peak=0,files=0; bool io_failed=false; };
    static constexpr size_t queue_capacity=4096;
    static uint64_t now() noexcept {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    template<class T> static std::string hex(const T& bytes) {
        std::ostringstream s; s<<std::hex<<std::setfill('0');
        for (auto b:bytes) s<<std::setw(2)<<static_cast<unsigned>(static_cast<unsigned char>(b));
        return s.str();
    }
    // Caller must select a new destination. GUI factory creates unique names
    // in an explicitly configured existing private capture directory.
    static std::shared_ptr<ReadTrace> open(const std::filesystem::path& path,uint64_t limit=100000,unsigned seconds=0) noexcept {
        try {
            if (!limit || limit>100000 || std::filesystem::exists(path)) return {};
            auto trace=std::shared_ptr<ReadTrace>(new ReadTrace(path,limit,seconds));
            if (!trace->output_) return {};
            trace->worker_=std::thread([p=trace.get()] {p->drain();});
            return trace;
        } catch (...) { return {}; }
    }
    static std::shared_ptr<ReadTrace> from_environment() noexcept {
        try {
#if defined(_MSC_VER)
            wchar_t* value=nullptr;size_t length=0;
            if (_wdupenv_s(&value,&length,L"NXEMU_VFS_TRACE_DIR")!=0 || !value) return {};
            std::unique_ptr<wchar_t,void(*)(void*)> storage(value,&std::free);
            const std::filesystem::path directory(storage.get());
#else
            const char* dir=std::getenv("NXEMU_VFS_TRACE_DIR");
            if (!dir || !*dir) return {};
            const auto directory=utf8_path(dir);
#endif
            if (directory.empty() || !std::filesystem::is_directory(directory)) return {};
            static std::atomic<uint64_t> suffix{0};
            const auto path=directory/("nxread-"+std::to_string(now())+"-"+std::to_string(++suffix)+".tsv");
            // Auto-finalize without requiring a game restart/shutdown or any
            // privileged system profiler. Existing readers remain operational.
            return open(path,100000,30);
        } catch (...) { return {}; }
    }
    ~ReadTrace() { close(); }
    void close() noexcept {
        { std::lock_guard<std::mutex> lock(mutex_); stop_=true; }
        cv_.notify_one();
        if (worker_.joinable()) worker_.join();
    }
    Ticket begin() noexcept {return {sequence_.fetch_add(1,std::memory_order_relaxed)+1,now()};}
    uint64_t handle() noexcept {return handles_.fetch_add(1,std::memory_order_relaxed)+1;}
    // Header is the complete F row excluding the numeric local file ID.
    uint64_t file(const std::string& header) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto old=file_ids_.find(header);
            if (old!=file_ids_.end()) return old->second;
            if (stop_ || headers_.size()>=512 || header.size()>12000) {++stats_.dropped;return 0;}
            const auto id=headers_.size()+1;
            headers_.push_back("F\t"+std::to_string(id)+"\t"+header+"\n");
            file_ids_.emplace(header,id);stats_.files=headers_.size();return id;
        } catch (...) { std::lock_guard<std::mutex> lock(mutex_); ++stats_.dropped;return 0; }
    }
    void end(Ticket ticket,uint64_t file,uint64_t handle,uint64_t offset,uint64_t bytes) noexcept {
        if (!bytes) return; // Only completed bytes, including a successful prefix.
        const auto latency=now()-ticket.start;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return;
            if (!file || bytes>UINT32_MAX || count_==queue_capacity || stats_.accepted==limit_) {++stats_.dropped;return;}
            const auto thread=std::this_thread::get_id();
            auto it=threads_.find(thread);
            if (it==threads_.end()) {
                if (threads_.size()==16384) {++stats_.dropped;return;}
                it=threads_.emplace(thread,threads_.size()+1).first;
            }
            queue_[(head_+count_)%queue_capacity]={ticket.sequence,file,handle,it->second,offset,bytes,ticket.start,latency};
            ++count_;++stats_.accepted;stats_.queue_peak=std::max<uint64_t>(stats_.queue_peak,count_);
            cv_.notify_one();
        } catch (...) {std::lock_guard<std::mutex> lock(mutex_);++stats_.dropped;}
    }
    Stats stats() {std::lock_guard<std::mutex> lock(mutex_);return stats_;}
    void invalidate() noexcept {std::lock_guard<std::mutex> lock(mutex_);if(!stop_) ++stats_.dropped;}
private:
    static std::filesystem::path utf8_path(const char* path) {
#if defined(__cpp_char8_t)
        return std::filesystem::path(reinterpret_cast<const char8_t*>(path));
#else
        return std::filesystem::u8path(path);
#endif
    }
    explicit ReadTrace(const std::filesystem::path& path,uint64_t limit,unsigned seconds):output_(path,std::ios::out|std::ios::binary),limit_(limit),deadline_(seconds ? std::chrono::steady_clock::now()+std::chrono::seconds(seconds) : std::chrono::steady_clock::time_point::max()) {output_<<"NXREAD1\n";}
    void drain() noexcept {
        size_t emitted=0;
        try {
            for (;;) {
                std::array<Event,64> batch{};size_t n=0;bool finished=false;
                std::vector<std::string> headers;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait_until(lock,deadline_,[&]{return stop_ || count_!=0;});
                    if (std::chrono::steady_clock::now()>=deadline_) stop_=true;
                    while (emitted<headers_.size()) headers.push_back(headers_[emitted++]);
                    n=std::min(batch.size(),count_);
                    for(size_t i=0;i<n;++i) batch[i]=queue_[(head_+i)%queue_capacity];
                    head_=(head_+n)%queue_capacity;count_-=n;finished=stop_ && count_==0;
                }
                for (const auto& h:headers) output_<<h;
                for (size_t i=0;i<n;++i) {const auto& e=batch[i]; output_<<"R\t"<<e.sequence<<'\t'<<e.file<<'\t'<<e.handle<<'\t'<<e.thread<<'\t'<<e.offset<<'\t'<<e.bytes<<'\t'<<e.start<<'\t'<<e.latency<<'\n';}
                if (!output_) {std::lock_guard<std::mutex> lock(mutex_);stats_.io_failed=true;}
                if (finished) break;
            }
            const auto s=stats();output_<<"Z\t"<<s.accepted<<'\t'<<s.dropped<<'\n';output_.flush();
            if (!output_) {std::lock_guard<std::mutex> lock(mutex_);stats_.io_failed=true;}
        } catch (...) {std::lock_guard<std::mutex> lock(mutex_);stats_.io_failed=true;}
    }
    std::ofstream output_;
    uint64_t limit_;
    std::chrono::steady_clock::time_point deadline_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::array<Event,queue_capacity> queue_{};
    size_t head_=0,count_=0;
    bool stop_=false;
    std::vector<std::string> headers_;
    std::map<std::string,uint64_t> file_ids_;
    std::map<std::thread::id,uint64_t> threads_;
    Stats stats_;
    std::atomic<uint64_t> sequence_{0},handles_{0};
    std::thread worker_;
};
} // namespace nx
#endif
