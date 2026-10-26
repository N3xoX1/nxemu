// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/pipeline_cache_store.h"
#include "yuzu_video_core/shader_profile_codec.h"
#include "yuzu_video_core/shader_profile_file_stamp.h"
#include "yuzu_video_core/shader_profile_context.h"
#include "yuzu_video_core/shader_profile_limits.h"
#include "common/sha256.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace VideoCommon::CacheStore
{
namespace
{
constexpr size_t MaxFile = ShaderProfile::MaxLocalCacheBytes;
constexpr size_t MaxFrame = 4 * 1024 * 1024;
constexpr size_t EnvHeader = 68;
constexpr std::string_view Magic = "NXSCACHE";
// Frames may be compressed: this bit is set in their type, the length is the
// stored size and the identity still digests the uncompressed bytes.
// Revision 3 adds blocks: frames that earlier sessions appended, regrouped while
// loading and compressed together.
constexpr u32 Revision = 3;
constexpr unsigned char Packed = 0x80;
constexpr unsigned char BlockType = 4;
constexpr size_t BlockTarget = 4 * 1024 * 1024, MaxBlock = 16 * 1024 * 1024;
constexpr int BlockLevel = 9;
void Check(bool ok)
{
    if (!ok) throw std::runtime_error("Invalid NXEmu local shader cache");
}
void Put(std::string & out, u64 value, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i)
        out.push_back(static_cast<char>(value >> (8 * i)));
}
u64 Get(std::string_view bytes, size_t offset, size_t size)
{
    Check(offset <= bytes.size() && size <= bytes.size() - offset);
    u64 value{};
    for (size_t i = 0; i < size; ++i)
        value |= u64{static_cast<unsigned char>(bytes[offset + i])} << (8 * i);
    return value;
}
std::string Digest(std::string_view bytes)
{
    const auto digest = ShaderProfile::ResourceDigest(bytes);
    return {reinterpret_cast<const char*>(digest.data()), digest.size()};
}
// A same-size rewrite can keep the old write time, which only moves once per
// clock tick. The writers also keep the last bytes they wrote and compare them
// before each append: a replaced or corrupted store differs there, and a full
// parse still only happens when something changed.
constexpr size_t TailBytes = 64 * 1024;
std::string ReadTail(const std::filesystem::path& path, size_t size) {
    const auto length = std::min(size, TailBytes);
    if (!length) return {};
    std::string tail(length, '\0');
    std::ifstream in(path, std::ios::binary);
    if (!in.seekg(static_cast<std::streamoff>(size - length)) ||
        !in.read(tail.data(), static_cast<std::streamsize>(length))) tail.clear();
    return tail;
}
void KeepTail(std::string& tail, std::string_view written) {
    tail += written;
    if (tail.size() > TailBytes) tail.erase(0, tail.size() - TailBytes);
}
struct ContextIndex { std::set<std::string> ids; size_t valid_bytes{}; std::filesystem::file_time_type modified{}; std::string tail; };
// One index per game version and cache file ("vulkan.idx" for vulkan.nxc), in
// that version's folder next to its recording. NXCTX002 names its full context
// so the folder can be identified from it alone.
constexpr size_t ContextHeader = 8 + 64;
std::filesystem::path ContextPath(const std::filesystem::path& cache, std::string_view context, bool create) {
    Check(ShaderProfile::ValidContext(context));
    const auto directory = create ? ShaderProfile::VersionDirectory(cache.parent_path(), context)
                                  : ShaderProfile::FindVersionDirectory(cache.parent_path(), context);
    return directory.empty() ? std::filesystem::path{} : directory / (cache.stem().string() + ".idx");
}
// A torn header or a frame that fails its digest ends the valid prefix; the
// writer truncates there instead of rejecting every later association.
ContextIndex ParseContext(const std::filesystem::path& path, std::string_view context) {
    ContextIndex index;
    if (path.empty() || !std::filesystem::exists(path)) return index;
    const auto size=std::filesystem::file_size(path);
    index.modified=std::filesystem::last_write_time(path);
    Check(size<=ContextHeader+ShaderProfile::MaxVersionPipelines*64ULL);
    std::ifstream in(path,std::ios::binary);
    std::string header(ContextHeader,'\0');
    if(!in.read(header.data(),ContextHeader) || header!="NXCTX002"+std::string(context)) return index;
    index.valid_bytes=ContextHeader;
    std::string frame(64,'\0');
    while(in.read(frame.data(),64)) {
        const auto id=frame.substr(0,32);
        if(Digest(std::string(context)+id)!=frame.substr(32)) return index;
        index.ids.insert(id); index.valid_bytes+=64;
    }
    Check(in.eof());
    return index;
}
void AppendContext(const std::filesystem::path& cache, std::string_view context, const std::string& id) {
    const auto path=ContextPath(cache,context,true);
    static std::filesystem::path active;
    static ContextIndex index;
    const auto size=std::filesystem::exists(path)?std::filesystem::file_size(path):0;
    const auto modified=size ? std::filesystem::last_write_time(path) : std::filesystem::file_time_type{};
    if(active!=path || index.valid_bytes!=size || index.modified!=modified || ReadTail(path,size)!=index.tail) {
        index=ParseContext(path,context); active=path;
        if(index.valid_bytes!=size) std::filesystem::resize_file(path,index.valid_bytes);
        index.tail=ReadTail(path,index.valid_bytes);
    }
    if(index.ids.contains(id)) return;
    Check(index.ids.size()<ShaderProfile::MaxVersionPipelines);
    try {
        std::ofstream out(path,std::ios::binary|std::ios::app);
        const auto frame=(index.valid_bytes?std::string{}:"NXCTX002"+std::string(context))+id+Digest(std::string(context)+id);
        out.write(frame.data(),frame.size());out.flush();out.close();Check(bool(out));
        index.valid_bytes+=frame.size();
        index.ids.insert(id);
        KeepTail(index.tail,frame);
        index.modified=std::filesystem::last_write_time(path);
    } catch(...) { active.clear();index={};throw; }
}
struct Index
{
    size_t valid_bytes{};
    std::map<std::string, std::shared_ptr<const std::string>> code;
    std::map<std::string, SharedShaderBytes> environments;
    std::map<std::string, size_t> code_sizes;
    std::set<std::string> pipelines;
    std::vector<Record> records;
};
std::string Header(u32 abi)
{
    std::string out(Magic);
    Put(out, Revision, 4);
    Put(out, abi, 4);
    return out;
}
void PutFrameHeader(std::string& out, unsigned char type, size_t length, std::string_view id)
{
    out.push_back(static_cast<char>(type));
    Put(out, length, 4);
    out += id;
}
void PutFrame(std::string& out, unsigned char type, std::string_view id, std::string_view data)
{
    const auto packed = ShaderProfile::PackFrame(data, type == BlockType ? BlockLevel : 1);
    const std::string_view stored = packed.empty() ? data : std::string_view(packed);
    PutFrameHeader(out, packed.empty() ? type : type | Packed, stored.size(), id);
    out += stored;
}
// Where the blocks are, and the frames appended after them, for Compact.
struct Frame
{
    unsigned char type;
    std::string id, data;
};
struct Layout
{
    std::vector<std::pair<size_t, size_t>> blocks; // offset, size
    std::vector<Frame> loose;
    size_t loose_bytes{};
};
// valid_bytes is 0 when the header is missing or torn. A frame that fails its
// digest or references ends the valid prefix, like a torn append: every frame
// before it stays usable and the writer truncates there. Budget limits still throw.
Index Parse(const std::filesystem::path & path, u32 abi, bool records, const ReadLimits& limits = {},
            bool* foreign_header = nullptr, Layout* layout = nullptr)
{
    Index index;
    if (!std::filesystem::exists(path)) return index;
    const auto size = std::filesystem::file_size(path);
    Check(size <= MaxFile);
    std::ifstream file(path, std::ios::binary);
    std::string header(16, '\0');
    if (size < header.size()) return index;
    file.read(header.data(), header.size());
    Check(bool(file));
    if (header != Header(abi))
    {
        if (!foreign_header) Check(false);
        *foreign_header = true;
        return index;
    }
    size_t cursor = 16;
    index.valid_bytes = cursor;
    size_t table_bytes{}, record_bytes{};
    // Adds one verified frame; false when it does not fit what precedes it.
    // `collect` keeps the new objects of frames outside blocks for Compact.
    const auto accept = [&](unsigned char type, std::string_view id, std::string data, bool collect) {
        if (type == 1)
        {
            if (data.empty() || data.size() % 8 != 0 || data.size() > 1024 * 1024) return false;
            if (!index.code.contains(std::string(id)))
            {
                Check(data.size() <= limits.index_bytes - table_bytes);
                table_bytes += data.size();
                if (collect && layout) layout->loose.push_back({type, std::string(id), data});
                index.code_sizes.emplace(id, data.size());
                index.code.emplace(id, records ? std::make_shared<const std::string>(std::move(data)) : nullptr);
            }
        }
        else if (type == 2)
        {
            if (data.size() < EnvHeader + 32) return false;
            const auto found = index.code.find(data.substr(EnvHeader, 32));
            if (found == index.code.end() || Get(data, 0, 8) != index.code_sizes.at(found->first)) return false;
            if (!index.environments.contains(std::string(id)))
            {
                const auto expanded = data.size() - 32;
                Check(expanded <= limits.index_bytes - table_bytes);
                table_bytes += expanded;
                if (collect && layout) layout->loose.push_back({type, std::string(id), data});
                auto env = records ? SharedShaderBytes::FromStore(data,found->second) : SharedShaderBytes{};
                index.environments.emplace(id, std::move(env));
            }
        }
        else
        {
            if (data.size() < 8) return false;
            const auto count = static_cast<size_t>(Get(data, 0, 4));
            if (count < 1 || count > 6 || data.size() < 8 + 32 * count) return false;
            const auto key_size = static_cast<size_t>(Get(data, 4 + 32 * count, 4));
            if (key_size == 0 || key_size > 65536 || data.size() != 8 + 32 * count + key_size) return false;
            for (size_t i = 0; i < count; ++i)
                if (!index.environments.contains(data.substr(4 + 32 * i, 32))) return false;
            const bool fresh = index.pipelines.emplace(id).second;
            if (collect && layout && fresh) layout->loose.push_back({type, std::string(id), data});
            if (records && fresh)
            {
                Record record;
                for (size_t i = 0; i < count; ++i)
                {
                    Check(sizeof(SharedShaderBytes) <= limits.record_bytes - record_bytes);
                    record_bytes += sizeof(SharedShaderBytes);
                    record.environments.push_back(index.environments.at(data.substr(4 + 32 * i, 32)));
                }
                Check(key_size <= limits.record_bytes - record_bytes);
                record_bytes += key_size;
                record.key = data.substr(8 + 32 * count);
                record.id = id;
                index.records.push_back(std::move(record));
            }
        }
        Check(index.code.size() + index.environments.size() + index.pipelines.size() <= ShaderProfile::MaxLocalCacheObjects);
        return true;
    };
    std::string frame_header(37, '\0'), stored, data;
    while (cursor < size)
    {
        // Read bounded frames rather than retaining an additional full-file copy.
        if (size - cursor < 37) break;
        file.read(frame_header.data(), frame_header.size());
        Check(bool(file));
        const auto stored_type = static_cast<unsigned char>(frame_header[0]);
        const bool packed = (stored_type & Packed) != 0;
        const auto type = static_cast<unsigned char>(stored_type & ~Packed);
        const auto length = static_cast<size_t>(Get(frame_header, 1, 4));
        const auto limit = type == BlockType ? MaxBlock : MaxFrame;
        if (length > limit || type < 1 || type > BlockType || length > size - cursor - 37) break;
        const std::string_view id = std::string_view(frame_header).substr(5, 32);
        stored.resize(length);
        file.read(stored.data(), length);
        Check(bool(file));
        data.assign(1, static_cast<char>(type));
        if (packed)
        {
            std::string raw;
            if (!ShaderProfile::UnpackFrame(stored, limit, raw)) break;
            data += raw;
        }
        else
            data += stored;
        if (Digest(data) != id) break;
        data.erase(0, 1);
        if (type == BlockType)
        {
            // A block is checked whole first, then every frame in it must fit: a
            // block is written in one piece, so a mismatch is never a torn append.
            std::vector<std::pair<size_t, size_t>> frames; // offset, length
            bool whole = true;
            for (size_t at = 0; whole && at < data.size();)
            {
                const auto inner = static_cast<unsigned char>(data[at]);
                const auto inner_length = data.size() - at >= 37 ? static_cast<size_t>(Get(data, at + 1, 4)) : 0;
                whole = data.size() - at >= 37 && inner >= 1 && inner <= 3 && inner_length <= MaxFrame &&
                        inner_length <= data.size() - at - 37 &&
                        Digest(data.substr(at, 1) + data.substr(at + 37, inner_length)) == data.substr(at + 5, 32);
                frames.emplace_back(at, inner_length);
                at += 37 + inner_length;
            }
            if (!whole || frames.empty()) break;
            for (const auto & [at, inner_length] : frames)
                Check(accept(static_cast<unsigned char>(data[at]), std::string_view(data).substr(at + 5, 32),
                             data.substr(at + 37, inner_length), false));
            if (layout) layout->blocks.emplace_back(cursor, 37 + length);
        }
        else
        {
            if (!accept(type, id, std::move(data), true)) break;
            if (layout) layout->loose_bytes += 37 + length;
        }
        cursor += 37 + length;
        index.valid_bytes = cursor;
    }
    return index;
}
std::mutex writer_mutex;
struct Writer
{
    Index index;
    size_t disk_size{};
    std::filesystem::file_time_type modified{};
    std::string tail;
    u32 abi{};
    // Weak ownership distinguishes reused addresses without retaining shader
    // bytes after an import. Memoization never trusts a mutable flat buffer.
    std::map<std::weak_ptr<const std::string>,std::string,std::owner_less<>> shared_code_ids;
};
std::filesystem::path writer_path;
Writer writer;

void SyncWriter(const std::filesystem::path& path,u32 shader_abi) {
    if (writer_path != path)
    {
        writer = Writer{};
        writer_path = path;
    }
    const auto size = std::filesystem::exists(path) ? std::filesystem::file_size(path) : 0;
    const auto modified = size ? std::filesystem::last_write_time(path) : std::filesystem::file_time_type{};
    if (!writer.disk_size || writer.disk_size != size || writer.modified != modified || writer.abi != shader_abi ||
        ReadTail(path, size) != writer.tail)
    {
        bool foreign_header{};
        writer.index = Parse(path, shader_abi, false, {}, &foreign_header);
        writer.shared_code_ids.clear();
        writer.abi = shader_abi;
        if (foreign_header)
        {
            // Another storage or shader ABI: keep it aside once and start a new store,
            // as the legacy cache did, instead of failing every later append.
            auto previous = path;
            previous += ".old";
            std::filesystem::rename(path, previous);
        }
        else if (writer.index.valid_bytes != size)
            std::filesystem::resize_file(path, writer.index.valid_bytes);
        writer.tail = ReadTail(path, writer.index.valid_bytes);
    }
    writer.disk_size=writer.index.valid_bytes;
    writer.modified=writer.disk_size?std::filesystem::last_write_time(path):std::filesystem::file_time_type{};
}
} // namespace
std::vector<Record> Read(const std::filesystem::path & path, u32 shader_abi, const ReadLimits& limits,
                         std::set<std::string>* record_ids)
{
    auto parsed=Parse(path, shader_abi, true, limits);
    if(record_ids)*record_ids=std::move(parsed.pipelines);
    return std::move(parsed.records);
}
void Append(const std::filesystem::path & path, u32 shader_abi, const Record & record,
            std::string* record_id, std::string_view context)
{
    std::scoped_lock lock(writer_mutex);
    Check(!record.environments.empty() && record.environments.size() <= 6 &&
          !record.key.empty() && record.key.size() <= 65536);
    SyncWriter(path,shader_abi);
    try
    {
        std::string pending;
        if (!writer.index.valid_bytes) pending = Header(shader_abi);
        const auto frame = [&](unsigned char type, const std::string & data, auto & table) {
            Check(data.size() <= MaxFrame);
            const auto id = Digest(std::string(1, static_cast<char>(type)) + data);
            if (!table.contains(id)) PutFrame(pending, type, id, data);
            return id;
        };
        // Build all frames before touching the file; validate the complete environment layout first.
        std::string pipeline;
        Put(pipeline, record.environments.size(), 4);
        for (const auto & env : record.environments)
        {
            Check(env.size() >= EnvHeader);
            const auto code_size = static_cast<size_t>(Get(env.substr(0,8), 0, 8));
            Check(code_size && code_size % 8 == 0 && code_size <= 1024 * 1024 && code_size <= env.size() - EnvHeader);
            const auto program=env.ProgramStorage();
            Check(!program || program->size()==code_size);
            const std::weak_ptr<const std::string> owner(program);
            const auto known=program ? writer.shared_code_ids.find(owner) : writer.shared_code_ids.end();
            const auto code_id = known!=writer.shared_code_ids.end() ? known->second :
                frame(1, env.substr(EnvHeader, code_size), writer.index.code);
            if(program && known==writer.shared_code_ids.end()) {
                if(writer.shared_code_ids.size()>=16384)writer.shared_code_ids.clear();
                writer.shared_code_ids.emplace(owner,code_id);
            }
            // Include additions immediately in a temporary index to deduplicate within this record too.
            if (!writer.index.code.contains(code_id))
            {
                writer.index.code.emplace(code_id, nullptr);
                writer.index.code_sizes.emplace(code_id, code_size);
            }
            const auto metadata = env.substr(0, EnvHeader) + code_id + env.substr(EnvHeader + code_size);
            const auto env_id = frame(2, metadata, writer.index.environments);
            if (!writer.index.environments.contains(env_id))
            {
                writer.index.environments.emplace(env_id, SharedShaderBytes{});
            }
            pipeline += env_id;
        }
        Put(pipeline, record.key.size(), 4);
        pipeline += record.key;
        const auto pipeline_id = frame(3, pipeline, writer.index.pipelines);
        if (writer.index.code.size()+writer.index.environments.size()+writer.index.pipelines.size()+1>ShaderProfile::MaxLocalCacheObjects ||
            writer.index.valid_bytes + pending.size() > MaxFile)
            throw std::runtime_error("Local shader cache reached its 1 GiB limit; new entries were not written");
        std::ofstream out(path, std::ios::binary | std::ios::app);
        out.write(pending.data(), static_cast<std::streamsize>(pending.size()));
        out.flush();
        Check(bool(out));
        writer.index.pipelines.insert(pipeline_id);
        writer.index.valid_bytes += pending.size();
        writer.disk_size = writer.index.valid_bytes;
        writer.modified = std::filesystem::last_write_time(path);
        KeepTail(writer.tail, pending);
        if (!context.empty()) AppendContext(path,context,pipeline_id);
        if(record_id)*record_id=pipeline_id;
    }
    catch (...)
    {
        writer = Writer{};
        writer_path.clear();
        throw;
    }
}

bool AssociateContext(const std::filesystem::path& path,u32 shader_abi,
                      const std::string& record_id,std::string_view context) {
    std::scoped_lock lock(writer_mutex);
    Check(record_id.size()==32 && ShaderProfile::ValidContext(context));
    SyncWriter(path,shader_abi);
    if(!writer.index.pipelines.contains(record_id))return false;
    AppendContext(path,context,record_id);
    return true;
}

bool Compact(const std::filesystem::path& path, u32 shader_abi, size_t min_loose_bytes) {
    std::scoped_lock lock(writer_mutex);
    if (!std::filesystem::exists(path)) return false;
    // Most loads have nothing to regroup: frame headers alone tell, without
    // decompressing or digesting anything.
    const auto worth = [&] {
        const auto size = std::filesystem::file_size(path);
        std::ifstream in(path, std::ios::binary);
        std::string header(16, '\0');
        if (size < header.size() || !in.read(header.data(), header.size()) || header != Header(shader_abi)) return false;
        size_t loose{};
        std::string frame(37, '\0');
        for (u64 at = header.size(); size - at >= 37 && in.read(frame.data(), frame.size());)
        {
            const auto length = Get(frame, 1, 4);
            if (length > size - at - 37) break;
            if ((static_cast<unsigned char>(frame[0]) & ~Packed) != BlockType) loose += 37 + length;
            at += 37 + length;
            in.seekg(static_cast<std::streamoff>(at));
        }
        return loose >= min_loose_bytes;
    };
    if (!worth()) return false;
    Layout layout;
    bool foreign{};
    const auto index = Parse(path, shader_abi, false, {}, &foreign, &layout);
    if (foreign || !index.valid_bytes || layout.loose_bytes < min_loose_bytes) return false;
    // Blocks are copied as they are. The other frames become new blocks, each
    // kind together (code, then environments, then pipelines: every frame still
    // follows what it names), which compresses about twice as well.
    std::string out = Header(shader_abi);
    {
        std::ifstream in(path, std::ios::binary);
        for (const auto & [offset, size] : layout.blocks)
        {
            std::string bytes(size, '\0');
            in.seekg(static_cast<std::streamoff>(offset));
            in.read(bytes.data(), static_cast<std::streamsize>(size));
            Check(bool(in));
            out += bytes;
        }
    }
    std::stable_sort(layout.loose.begin(), layout.loose.end(), [](const Frame & a, const Frame & b) { return a.type < b.type; });
    std::string block;
    const auto flush = [&] {
        if (block.empty()) return;
        PutFrame(out, BlockType, Digest(std::string(1, static_cast<char>(BlockType)) + block), block);
        block.clear();
    };
    for (const auto & frame : layout.loose)
    {
        PutFrameHeader(block, frame.type, frame.data.size(), frame.id);
        block += frame.data;
        if (block.size() >= BlockTarget) flush();
    }
    flush();
    // Written aside and renamed over the store: a crash keeps the old file.
    auto staging = path;
    staging += ".tmp";
    {
        std::ofstream file(staging, std::ios::binary | std::ios::trunc);
        file.write(out.data(), static_cast<std::streamsize>(out.size()));
        file.flush();
        Check(bool(file));
    }
    std::filesystem::rename(staging, path);
    writer = Writer{};
    writer_path.clear();
    return true;
}

std::filesystem::path ContextIndexPath(const std::filesystem::path& path, std::string_view context) {
    return ContextPath(path,context,false);
}

std::set<std::string> ReadContext(const std::filesystem::path& path, std::string_view context) {
    return ParseContext(ContextPath(path,context,false),context).ids;
}

} // namespace VideoCommon::CacheStore
