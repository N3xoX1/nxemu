#ifndef NX_DISK_SOURCE_HPP_INCLUDED
#define NX_DISK_SOURCE_HPP_INCLUDED
#include "nx_reader.hpp"
#include <atomic>
#include <filesystem>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nx {
// An owned, pinned read-only handle. Windows denies concurrent write handles;
// POSIX uses an advisory shared lock: writers must honor it. In-place mutation
// outside that contract is unsupported. Rename/unlink keeps this inode alive.
class DiskSource final : public RandomAccessSource {
public:
    static Result<std::shared_ptr<DiskSource>> open(const std::filesystem::path& path) {
        try {
            auto source=std::shared_ptr<DiskSource>(new DiskSource);
#if defined(_WIN32)
            source->file_=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,
                                     OPEN_EXISTING,FILE_FLAG_OVERLAPPED|FILE_FLAG_RANDOM_ACCESS,nullptr);
            LARGE_INTEGER size{};
            if (source->file_==INVALID_HANDLE_VALUE || !GetFileSizeEx(source->file_,&size) || size.QuadPart<0)
                return Result<std::shared_ptr<DiskSource>>::failure(ErrorCode::io,"cannot pin read-only NX source");
            source->size_=static_cast<std::uint64_t>(size.QuadPart);
#else
            source->file_=::open(path.c_str(),O_RDONLY|O_CLOEXEC);
            struct stat info{};
            if (source->file_<0 || fstat(source->file_,&info) || !S_ISREG(info.st_mode) || info.st_size<0 || flock(source->file_,LOCK_SH|LOCK_NB))
                return Result<std::shared_ptr<DiskSource>>::failure(ErrorCode::io,"cannot pin read-only NX source");
            source->size_=static_cast<std::uint64_t>(info.st_size);
#endif
            return Result<std::shared_ptr<DiskSource>>::success(std::move(source));
        } catch (const std::bad_alloc&) {
            return Result<std::shared_ptr<DiskSource>>::failure(ErrorCode::resource_limit,"disk source allocation failed");
        }
    }
    ~DiskSource() override {
#if defined(_WIN32)
        for (const auto& event:events_) if (event.handle) CloseHandle(event.handle);
        if (file_!=INVALID_HANDLE_VALUE) CloseHandle(file_);
#else
        if (file_>=0) ::close(file_);
#endif
    }
    std::uint64_t size() const noexcept override { return size_; }
    std::uint64_t read_calls() const noexcept { return calls_.load(); }
    std::uint64_t bytes_read() const noexcept { return bytes_.load(); }
    bool read_at(std::uint64_t offset,std::uint8_t* output,std::size_t length) override {
        if (offset>size_ || length>size_-offset || (length && !output)) return false;
        ++calls_;
#if defined(_WIN32)
        if (!length) return true;
        // Source-owned reuse, not TLS: caller threads may outlive the DLL that
        // implements this reader. Overflow events are local to one completed IO.
        EventLease lease(*this);
        const HANDLE event=lease.handle;
        if (!event) return false;
        std::size_t done=0;
        while (done<length) {
            OVERLAPPED operation{};
            const auto at=offset+done;
            operation.Offset=static_cast<DWORD>(at);
            operation.OffsetHigh=static_cast<DWORD>(at>>32);
            operation.hEvent=event;
            ResetEvent(event);
            DWORD read=0;
            const DWORD wanted=static_cast<DWORD>(std::min<std::size_t>(length-done,1024*1024));
            if (!ReadFile(file_,output+done,wanted,&read,&operation)) {
                if (GetLastError()!=ERROR_IO_PENDING || !GetOverlappedResult(file_,&operation,&read,TRUE)) {
                    return false;
                }
            }
            if (!read) return false;
            done+=read; bytes_+=read;
        }
#else
        std::size_t done=0;
        while (done<length) {
            const auto amount=pread(file_,output+done,std::min<std::size_t>(length-done,1024*1024),static_cast<off_t>(offset+done));
            if (amount<0 && errno==EINTR) continue;
            if (amount<=0) return false;
            done+=static_cast<std::size_t>(amount); bytes_+=static_cast<std::uint64_t>(amount);
        }
#endif
        return true;
    }
private:
    DiskSource()=default;
#if defined(_WIN32)
    struct EventSlot { HANDLE handle=nullptr; bool busy=false; };
    std::array<EventSlot,8> events_{};
    std::mutex event_mutex_;
    struct EventLease {
        DiskSource& source;
        HANDLE handle=nullptr;
        std::size_t slot=8;
        explicit EventLease(DiskSource& owner):source(owner) {
            std::lock_guard<std::mutex> lock(source.event_mutex_);
            for (std::size_t i=0;i<source.events_.size();++i) {
                auto& event=source.events_[i];
                if (event.busy) continue;
                if (!event.handle) event.handle=CreateEventW(nullptr,TRUE,FALSE,nullptr);
                if (!event.handle) return;
                event.busy=true; slot=i; handle=event.handle; return;
            }
            handle=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        }
        ~EventLease() {
            if (slot==source.events_.size()) { if (handle) CloseHandle(handle); }
            else { std::lock_guard<std::mutex> lock(source.event_mutex_); source.events_[slot].busy=false; }
        }
        EventLease(const EventLease&)=delete;
        EventLease& operator=(const EventLease&)=delete;
    };
    HANDLE file_=INVALID_HANDLE_VALUE;
#else
    int file_=-1;
#endif
    std::uint64_t size_=0;
    std::atomic<std::uint64_t> calls_{0},bytes_{0};
};
} // namespace nx
#endif
