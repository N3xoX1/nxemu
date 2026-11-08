#include "core/file_sys/vfs/vfs_nx.h"
class ReadOnlyHost final : public FileSys::VfsFile {
public:
    std::string GetName() const override { return "test.nx"; }
    size_t GetSize() const override { return 0; }
    FileSys::VirtualDir GetContainingDirectory() const override { return {}; }
    bool IsWritable() const override { return false; }
    bool IsReadable() const override { return true; }
    bool Resize(size_t) override { return false; }
    bool Rename(std::string_view) override { return false; }
    size_t Write(const u8*,size_t,size_t) override { return 0; }
    size_t Read(u8*,size_t,size_t) const override { return 0; }
};
// Test DLL only. Links the actual loader objects, including the real decoder.
extern "C" __declspec(dllexport) int NXTestRead(const char* path) {
    nx::SessionOptions options; options.sequential_prefetch=false;
    options.reader.max_cached_block_bytes=0; options.reader.max_cached_blocks=0;
    auto opened=FileSys::OpenNxSession(std::make_shared<ReadOnlyHost>(),path,options);
    if (!opened) return 1;
    auto file=(*opened.value)->archive().resolve_file(0,"data.bin");
    std::array<u8,4096> bytes{};
    if (!file || !(*opened.value)->read(file.value->id,0,bytes.size(),bytes.data(),bytes.size()) ||
        !std::all_of(bytes.begin(),bytes.end(),[](u8 b){return b==1;})) return 2;
    return 0;
}
