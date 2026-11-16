// SPDX-License-Identifier: MIT
#ifndef NX_MAPPED_VIEWS_HPP_INCLUDED
#define NX_MAPPED_VIEWS_HPP_INCLUDED
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

namespace nx { namespace detail {
// File-backed pages are shared with the OS, not copied into a private payload
// cache. Bound the mapped address ranges as well: whole multi-GiB views consume
// page-table commitment even when only a few members are accessed.
struct MappedRange {
    const std::uint8_t* data=nullptr;
    std::shared_ptr<const void> owner;
};
class MappedViews {
    static constexpr std::uint64_t window=16ull*1024*1024;
    struct Section {
        HANDLE file=INVALID_HANDLE_VALUE, mapping=nullptr;
        ~Section() { if(mapping)CloseHandle(mapping); if(file!=INVALID_HANDLE_VALUE)CloseHandle(file); }
    };
    struct Region {
        std::shared_ptr<Section> section;
        std::uint64_t offset=0;
        std::size_t length=0;
        const std::uint8_t* data=nullptr;
        ~Region() { if(data) UnmapViewOfFile(data); }
    };
    std::shared_ptr<Section> section_;
    std::uint64_t size_=0;
    mutable std::mutex mutex_;
    mutable std::array<std::shared_ptr<Region>,64> regions_{};
    mutable std::array<bool,64> referenced_{};
    mutable std::size_t cursor_=0;
public:
    using ReadOperation = void (*)(void*);
#if defined(_MSC_VER)
    static constexpr bool fault_guard_supported=true;
#else
    static constexpr bool fault_guard_supported=false;
#endif
    // Leaf SEH frames intentionally own no C++ objects. Owners and operation
    // state stay in the caller so an asynchronous IO fault cannot skip RAII.
    static int mapped_fault_filter(const EXCEPTION_POINTERS* fault,const std::uint8_t* base,std::size_t length) noexcept {
        const auto* record=fault->ExceptionRecord;
        if (record->ExceptionCode!=EXCEPTION_IN_PAGE_ERROR || record->NumberParameters<3 ||
            record->ExceptionInformation[0]!=0) return EXCEPTION_CONTINUE_SEARCH;
        const auto address=static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
        const auto begin=reinterpret_cast<std::uintptr_t>(base);
        return address>=begin && address-begin<length ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
    }
    static bool guarded_access(const std::uint8_t* base,std::size_t length,ReadOperation operation,void* context) {
#if defined(_MSC_VER)
        __try { operation(context); return true; }
        __except(mapped_fault_filter(GetExceptionInformation(),base,length)) { return false; }
#else
        (void)base; (void)length; (void)operation; (void)context;
        return false; // never expose fallible views without a compiler SEH guard
#endif
    }
    static bool copy(const std::uint8_t* source,std::uint8_t* output,std::size_t length) {
#if defined(_MSC_VER)
        __try { std::memcpy(output,source,length); return true; }
        __except(mapped_fault_filter(GetExceptionInformation(),source,length)) { return false; }
#else
        (void)source; (void)output; (void)length;
        return false;
#endif
    }
    MappedViews()=default;
    MappedViews(const MappedViews&)=delete;
    MappedViews& operator=(const MappedViews&)=delete;
    ~MappedViews()=default;
    bool open(HANDLE pinned_file,std::uint64_t size) {
        if(!fault_guard_supported || section_)return false;
        size_=size;
        if(!size)return true;
        auto section=std::make_shared<Section>();
        if(!DuplicateHandle(GetCurrentProcess(),pinned_file,GetCurrentProcess(),&section->file,0,FALSE,DUPLICATE_SAME_ACCESS))return false;
        section->mapping=CreateFileMappingW(section->file,nullptr,PAGE_READONLY,0,0,nullptr);
        if(!section->mapping)return false;
        section_=std::move(section);
        return true;
    }
    MappedRange pin(std::uint64_t offset,std::size_t length) const noexcept {
        if(!section_ || !length || offset>size_ || length>size_-offset || length>window)return {};
        const auto base=offset/window*window;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            for(std::size_t i=0;i<regions_.size();++i) {
                const auto& region=regions_[i];
                if(region && region->offset==base && offset-base<=region->length && length<=region->length-(offset-base)) {
                    referenced_[i]=true;
                    return {region->data+(offset-base),region};
                }
            }
            auto region=std::make_shared<Region>();
            region->section=section_;
            region->offset=base;
            region->length=static_cast<std::size_t>(std::min<std::uint64_t>(size_-base,std::max<std::uint64_t>(window,offset-base+length)));
            region->data=static_cast<const std::uint8_t*>(MapViewOfFile(section_->mapping,FILE_MAP_READ,
                static_cast<DWORD>(base>>32),static_cast<DWORD>(base),region->length));
            if(!region->data)return {};
            // CLOCK eviction drops only our owner. An in-flight reader's lease
            // keeps its view alive, including under concurrent eviction.
            while(regions_[cursor_] && referenced_[cursor_]) {
                referenced_[cursor_]=false;
                cursor_=(cursor_+1)%regions_.size();
            }
            regions_[cursor_]=region; referenced_[cursor_]=true;
            cursor_=(cursor_+1)%regions_.size();
            return {region->data+(offset-base),region};
        } catch(...) { return {}; } // ordinary read_at remains the fallback
    }
};
}}
#endif
#endif
