#include "vfs_nx.h"
#include "nxformat/nx_disk_source.hpp"
#include "yuzu_common/fs/fs_util.h"
#include <zstd.h>
#include <cstring>
#include <limits>

namespace FileSys {
namespace {
// Decoder contexts belong to the mounted source, not caller-thread TLS. A
// calling emulator thread may outlive an unloaded/reloaded loader DLL.
class DecoderPool {
    struct Slot { ZSTD_DCtx* context=nullptr; bool busy=false; size_t bytes=0; };
    std::vector<Slot> slots_;
    std::mutex mutex_;
    std::condition_variable available_;
    uint64_t bytes_=0,peak_=0;
public:
    explicit DecoderPool(size_t limit):slots_(limit) {}
    ~DecoderPool() { for (auto& slot:slots_) if (slot.context) ZSTD_freeDCtx(slot.context); }
    class Lease {
        DecoderPool& pool_;
        Slot* slot_=nullptr;
    public:
        explicit Lease(DecoderPool& pool):pool_(pool) {
            std::unique_lock<std::mutex> lock(pool_.mutex_);
            pool_.available_.wait(lock,[&] {
                for (auto& slot:pool_.slots_) if (!slot.busy) { slot_=&slot; return true; }
                return false;
            });
            slot_->busy=true;
        }
        ~Lease() {
            {
                std::lock_guard<std::mutex> lock(pool_.mutex_);
                pool_.bytes_-=slot_->bytes;
                slot_->bytes=slot_->context ? ZSTD_sizeof_DCtx(slot_->context) : 0;
                pool_.bytes_+=slot_->bytes; pool_.peak_=std::max(pool_.peak_,pool_.bytes_);
                slot_->busy=false;
            }
            pool_.available_.notify_one();
        }
        ZSTD_DCtx* context() {
            if (!slot_->context) {
                slot_->context=ZSTD_createDCtx();
                if (slot_->context && ZSTD_isError(ZSTD_DCtx_setParameter(slot_->context,ZSTD_d_windowLogMax,20))) {
                    ZSTD_freeDCtx(slot_->context); slot_->context=nullptr;
                }
            }
            return slot_->context;
        }
        Lease(const Lease&)=delete;
        Lease& operator=(const Lease&)=delete;
    };
    NxRuntimeStats stats() {
        std::lock_guard<std::mutex> lock(mutex_);
        NxRuntimeStats result;
        result.decoder_bytes=bytes_; result.decoder_peak_bytes=peak_;
        // bytes, updated at release, can be inspected without racing live codecs.
        for (const auto& slot:slots_) if (slot.bytes) ++result.decoder_contexts;
        result.decoder_pool_bytes=sizeof(*this)+slots_.capacity()*sizeof(Slot);
        return result;
    }
};

bool Decode(void* opaque, const uint8_t* frame, size_t size, uint8_t* output,
            size_t capacity, size_t* written, size_t* consumed) {
    if (size<6 || capacity>1024*1024 || std::memcmp(frame,"\x28\xb5\x2f\xfd",4) ||
        ZSTD_getFrameContentSize(frame,size)!=capacity || ZSTD_getDictID_fromFrame(frame,size) ||
        ZSTD_findFrameCompressedSize(frame,size)!=size) return false;
    const unsigned descriptor=frame[4];
    if (descriptor & 0x1bu) return false; // Reserved bits and any dictionary-ID field.
    if (!(descriptor & 0x20u)) {
        const auto base=uint64_t{1}<<(10+(frame[5]>>3));
        if (base+(base>>3)*(frame[5]&7u)>1024*1024) return false;
    }
    DecoderPool::Lease decoder(*static_cast<DecoderPool*>(opaque));
    auto context=decoder.context();
    if (!context) return false;
    const auto result=ZSTD_decompressDCtx(context,output,capacity,frame,size);
    if (ZSTD_isError(result) || result!=capacity) return false;
    *written=result; *consumed=size; return true;
}
class CodecSource final : public nx::RandomAccessSource {
    std::shared_ptr<nx::RandomAccessSource> source_;
    std::atomic<uint64_t> calls_{0},bytes_{0};
public:
    DecoderPool decoder;
    CodecSource(std::shared_ptr<nx::RandomAccessSource> source,size_t limit)
        :source_(std::move(source)),decoder(limit) {}
    uint64_t size() const noexcept override { return source_->size(); }
    bool read_at(uint64_t offset,uint8_t* output,size_t length) override {
        ++calls_;
        const bool ok=source_->read_at(offset,output,length);
        if (ok) bytes_+=length;
        return ok;
    }
    NxRuntimeStats stats() {
        auto result=decoder.stats(); result.source_read_calls=calls_.load(); result.source_read_bytes=bytes_.load();
        return result;
    }
};
// For non-host, caller-owned immutable VirtualFiles (e.g. an in-memory test).
// The ordinary host launch passes an explicit path and uses a pinned DiskSource.
class Source final : public nx::RandomAccessSource {
public:
    explicit Source(VirtualFile file):file_(std::move(file)),size_(file_->GetSize()) {}
    uint64_t size() const noexcept override { return size_; }
    bool read_at(uint64_t offset,uint8_t* output,size_t length) override {
        if (offset>size_ || length>size_-offset || offset>std::numeric_limits<size_t>::max()) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        return file_->Read(output,length,static_cast<size_t>(offset))==length;
    }
private:
    VirtualFile file_; uint64_t size_; std::mutex mutex_;
};

class NxFile final : public VfsFile {
public:
    NxFile(std::shared_ptr<nx::Session> session,nx::FileInfo info,std::string name,VirtualDir parent)
        :session_(std::move(session)),info_(info),name_(std::move(name)),parent_(std::move(parent)),
         cursor_(std::make_shared<nx::ReadCursor>()) {}
    std::string GetName() const override { return name_; }
    size_t GetSize() const override { return static_cast<size_t>(info_.size); }
    VirtualDir GetContainingDirectory() const override { return parent_; }
    bool IsWritable() const override { return false; }
    bool IsReadable() const override { return true; }
    bool Resize(size_t) override { return false; }
    bool Rename(std::string_view) override { return false; }
    size_t Write(const u8*,size_t,size_t) override { return 0; }
    size_t Read(u8* output,size_t length,size_t offset) const override {
        if (offset>info_.size || (length && !output)) return 0;
        length=static_cast<size_t>(std::min<uint64_t>(length,info_.size-offset));
        size_t done=0;
        while (done<length) {
            // read() commits each verified block straight into the output; on failure,
            // bytes is the verified prefix, which is all this returns.
            const auto part=static_cast<size_t>(std::min<uint64_t>(length-done,session_->max_read_bytes()));
            if (!part) break;
            const auto result=session_->read(info_.id,offset+done,part,output+done,part,cursor_);
            done+=result.bytes;
            if (!result) break;
        }
        return done;
    }
private:
    std::shared_ptr<nx::Session> session_; nx::FileInfo info_; std::string name_; VirtualDir parent_;
    std::shared_ptr<nx::ReadCursor> cursor_;
};

class NxDirectory final : public ReadOnlyVfsDirectory, public std::enable_shared_from_this<NxDirectory> {
public:
    NxDirectory(std::shared_ptr<nx::Session> session,nx::DirectoryHandle handle,std::string name,VirtualDir parent={})
        :session_(std::move(session)),handle_(handle),name_(std::move(name)),parent_(std::move(parent)) {}
    std::string GetName() const override { return name_; }
    VirtualDir GetParentDirectory() const override { return parent_; }
    // lookup() is a binary search over the directory's sorted entries.
    VirtualFile GetFile(std::string_view name) const override {
        const auto entry=session_->archive().lookup(handle_,name);
        if (!entry || entry.value->type!=nx::EntryType::file) return nullptr;
        auto info=session_->archive().file_info(entry.value->file);
        if (!info || info.value->size>std::numeric_limits<size_t>::max()) return nullptr;
        return std::make_shared<NxFile>(session_,*info.value,std::string(entry.value->name),
            std::const_pointer_cast<NxDirectory>(shared_from_this()));
    }
    VirtualDir GetSubdirectory(std::string_view name) const override {
        const auto entry=session_->archive().lookup(handle_,name);
        if (!entry || entry.value->type!=nx::EntryType::directory) return nullptr;
        return std::make_shared<NxDirectory>(session_,entry.value->directory,std::string(entry.value->name),
            std::const_pointer_cast<NxDirectory>(shared_from_this()));
    }
    std::vector<VirtualFile> GetFiles() const override {
        std::vector<VirtualFile> result;
        auto parent=std::const_pointer_cast<NxDirectory>(shared_from_this());
        session_->archive().enumerate(handle_,[&](const nx::DirectoryEntryView& entry) {
            if (entry.type==nx::EntryType::file) {
                auto info=session_->archive().file_info(entry.file);
                if (info && info.value->size<=std::numeric_limits<size_t>::max())
                    result.push_back(std::make_shared<NxFile>(session_,*info.value,std::string(entry.name),parent));
            }
            return true;
        });
        return result;
    }
    std::vector<VirtualDir> GetSubdirectories() const override {
        std::vector<VirtualDir> result;
        auto parent=std::const_pointer_cast<NxDirectory>(shared_from_this());
        session_->archive().enumerate(handle_,[&](const nx::DirectoryEntryView& entry) {
            if (entry.type==nx::EntryType::directory)
                result.push_back(std::make_shared<NxDirectory>(session_,entry.directory,std::string(entry.name),parent));
            return true;
        });
        return result;
    }
private:
    std::shared_ptr<nx::Session> session_; nx::DirectoryHandle handle_; std::string name_; VirtualDir parent_;
};
} // namespace

nx::Result<std::shared_ptr<nx::Session>> OpenNxSession(VirtualFile file,std::string_view host_path,nx::SessionOptions options) {
    if (!options.parallel_reads || options.parallel_reads>64)
        return nx::Result<std::shared_ptr<nx::Session>>::failure(nx::ErrorCode::invalid_argument,"invalid NX parallel-read bound");
    if (!file || file->IsWritable()) return nx::Result<std::shared_ptr<nx::Session>>::failure(nx::ErrorCode::invalid_argument,"NX source must be immutable/read-only");
    try {
    std::shared_ptr<nx::RandomAccessSource> source;
    // Android content:// URIs have no host path to pin: read through the VFS file.
    if (!host_path.empty() && host_path.rfind("content://",0)!=0) {
        auto pinned=nx::DiskSource::open(std::filesystem::path(Common::FS::ToU8String(host_path)));
        if (!pinned) return nx::Result<std::shared_ptr<nx::Session>>::failure(pinned.error.code,pinned.error.message);
        source=std::move(*pinned.value);
    } else source=std::make_shared<Source>(std::move(file));
    auto codec=std::make_shared<CodecSource>(std::move(source),options.parallel_reads);
    options.reader.zstd=&Decode; options.reader.zstd_context=&codec->decoder;
    return nx::Session::open(std::move(codec),options);
    } catch (const std::bad_alloc&) {
        return nx::Result<std::shared_ptr<nx::Session>>::failure(nx::ErrorCode::resource_limit,"NX codec source allocation failed");
    }
}
NxRuntimeStats NxSessionStatistics(const nx::Session& session) {
    return static_cast<CodecSource&>(session.backing_source()).stats();
}
VirtualFile NxSourceResourceFile(std::shared_ptr<nx::Session> session,uint32_t resource,std::string_view name) {
    auto file=session->archive().source_resource_file(resource,name);
    return file ? std::make_shared<NxFile>(std::move(session),*file.value,std::string(name),nullptr) : nullptr;
}
VirtualDir NxSnapshotDirectory(std::shared_ptr<nx::Session> session,uint32_t snapshot) {
    auto root=session->archive().root_directory(snapshot);
    return root ? std::make_shared<NxDirectory>(std::move(session),*root.value,"nx") : nullptr;
}
} // namespace FileSys
