// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile_validation.h"
#include "yuzu_video_core/shader_profile_discovery.h"
#include "yuzu_video_core/shader_profile_container.h"
#include "yuzu_video_core/shader_profile_context.h"
#include <utility>
#include <algorithm>
#include <cctype>
#include "yuzu_video_core/shader_profile_file_stamp.h"
#include "yuzu_video_core/shader_profile_hash.h"
#include "yuzu_video_core/shader_profile_resources.h"
#include "yuzu_video_core/shader_profile_search.h"
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace VideoCommon::ShaderProfile
{
namespace
{
class IndexLock
{
public:
    explicit IndexLock(const std::filesystem::path & index)
    {
        if (index.empty()) return;
        auto path = index;
        path += ".lock";
#ifdef _WIN32
        handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Discovery index is in use");
#else
        handle = open(path.c_str(), O_RDWR | O_CREAT, 0600);
        if (handle < 0) throw std::runtime_error("Cannot lock discovery index");
        if (flock(handle, LOCK_EX | LOCK_NB) != 0)
        {
            close(handle);
            handle = -1;
            throw std::runtime_error("Discovery index is in use");
        }
#endif
    }
    ~IndexLock()
    {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
#else
        if (handle >= 0)
        {
            flock(handle, LOCK_UN);
            close(handle);
        }
#endif
    }

private:
#ifdef _WIN32
    HANDLE handle{INVALID_HANDLE_VALUE};
#else
    int handle{-1};
#endif
};
void Check(bool b, const char * message)
{
    if (!b) throw std::runtime_error(message);
}
void Bounds(u64 offset, u64 size, u64 total)
{
    Check(offset <= total && size <= total - offset, "Discovery storage bounds");
}
u64 Get(std::string_view b, size_t pos, size_t n)
{
    Bounds(pos, n, b.size());
    u64 v{};
    for (size_t i = 0; i < n; ++i)
        v |= u64{static_cast<unsigned char>(b[pos + i])} << (8 * i);
    return v;
}
u64 Num(const JsonValue & v)
{
    return v.isString() ? std::stoull(v.asString()) : v.asUInt64();
}
JsonValue Jnum(u64 n)
{
    return n > UINT32_MAX ? JsonValue(std::to_string(n)) : JsonValue(static_cast<u32>(n));
}
JsonValue Array()
{
    return JsonValue(JsonValueType::Array);
}
std::filesystem::path Local(const std::filesystem::path & root, const std::string & name)
{
    return LocalResourcePath(root, name);
}
struct Storage
{
    u64 size{};
    virtual ~Storage() = default;
    virtual std::string Read(u64 off, u64 length) = 0;
    virtual JsonValue Spans(u64 off, u64 length) = 0;
};
using Store = std::shared_ptr<Storage>;
struct Physical final : Storage
{
    std::string name;
    std::ifstream file;
    Physical(const std::filesystem::path & path, std::string name_) :
        name(std::move(name_)), file(path, std::ios::binary)
    {
        Check(bool(file), "Cannot open game");
        size = std::filesystem::file_size(path);
    }
    std::string Read(u64 off, u64 length) override
    {
        Bounds(off, length, size);
        Check(length <= MaxDecodedResource, "Discovery read limit");
        std::string bytes(static_cast<size_t>(length), '\0');
        file.clear();
        file.seekg(static_cast<std::streamoff>(off));
        file.read(bytes.data(), static_cast<std::streamsize>(length));
        Check(bool(file), "Cannot read game resource");
        return bytes;
    }
    JsonValue Spans(u64 off, u64 length) override
    {
        Bounds(off, length, size);
        JsonValue result = Array();
        if (length)
        {
            JsonValue s;
            s["file"] = name;
            s["offset"] = Jnum(off);
            s["size"] = Jnum(length);
            result.Append(std::move(s));
        }
        return result;
    }
};
struct Slice final : Storage
{
    Store source;
    u64 base;
    Slice(Store src, u64 offset, u64 length) :
        source(std::move(src)), base(offset)
    {
        Bounds(offset, length, source->size);
        size = length;
    }
    std::string Read(u64 off, u64 length) override
    {
        Bounds(off, length, size);
        return source->Read(base + off, length);
    }
    JsonValue Spans(u64 off, u64 length) override
    {
        Bounds(off, length, size);
        return source->Spans(base + off, length);
    }
};
// One NCA inside a game container. Its spans name the NCA and count from its
// start, so a profile resolves with any dump holding that NCA, NSP or XCI.
struct Content final : Storage
{
    Store source;
    u64 base;
    std::string name;
    Content(Store src, u64 offset, u64 length, std::string name_) :
        source(std::move(src)), base(offset), name(std::move(name_))
    {
        Bounds(offset, length, source->size);
        size = length;
    }
    std::string Read(u64 off, u64 length) override
    {
        Bounds(off, length, size);
        return source->Read(base + off, length);
    }
    JsonValue Spans(u64 off, u64 length) override
    {
        Bounds(off, length, size);
        JsonValue result = Array();
        if (length)
        {
            JsonValue s;
            s["file"] = name;
            s["offset"] = Jnum(off);
            s["size"] = Jnum(length);
            result.Append(std::move(s));
        }
        return result;
    }
};
struct Entry
{
    u64 virtual_offset, physical_offset;
    u32 index;
};
struct Indirect final : Storage
{
    std::array<Store, 2> sources;
    std::vector<Entry> entries;
    Indirect(Store a, Store b, std::vector<Entry> es, u64 end) :
        sources{std::move(a), std::move(b)}, entries(std::move(es))
    {
        size = end;
        Check(!entries.empty() && entries.front().virtual_offset == 0 && end > 0, "BKTR coverage");
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const auto & e = entries[i];
            const auto stop = i + 1 < entries.size() ? entries[i + 1].virtual_offset : end;
            Check(e.index < 2 && stop > e.virtual_offset, "BKTR entry order");
            Bounds(e.physical_offset, stop - e.virtual_offset, sources[e.index]->size);
        }
    }
    template <class F>
    void Chunks(u64 off, u64 length, F fn)
    {
        Bounds(off, length, size);
        const auto stop = off + length;
        while (off < stop)
        {
            auto it = std::upper_bound(entries.begin(), entries.end(), off, [](u64 n, const Entry & e) { return n < e.virtual_offset; });
            Check(it != entries.begin(), "BKTR hole");
            --it;
            const auto end = std::min(stop, it + 1 == entries.end() ? size : (it + 1)->virtual_offset);
            fn(sources[it->index], it->physical_offset + off - it->virtual_offset, end - off);
            off = end;
        }
    }
    std::string Read(u64 off, u64 length) override
    {
        Check(length <= MaxDecodedResource, "BKTR read limit");
        std::string result;
        result.reserve(static_cast<size_t>(length));
        Chunks(off, length, [&](Store s, u64 p, u64 n) { result += s->Read(p, n); });
        return result;
    }
    JsonValue Spans(u64 off, u64 length) override
    {
        JsonValue result = Array();
        Chunks(off, length, [&](Store s, u64 p, u64 n) {const auto spans=s->Spans(p,n);for(const auto& span:spans)result.Append(span); });
        Check(result.size() <= 65536, "BKTR span limit");
        return result;
    }
};
struct File
{
    std::string name;
    u64 offset, size;
};
std::vector<File> Files(const std::vector<ContainerEntry>& entries)
{
    std::vector<File> files;
    for (const auto & entry : entries) files.push_back({entry.name, entry.offset, entry.size});
    return files;
}
std::vector<File> PartitionFiles(Store source)
{
    return Files(PartitionEntries(source->size, [&](u64 off, u64 length) { return source->Read(off, length); }));
}
std::vector<File> Pfs(Store source)
{
    return Files(ContainerEntries(source->size, [&](u64 off, u64 length) { return source->Read(off, length); }));
}
std::string ProgramPart(Store source, u64 title)
{
    std::string part;
    for (const auto & nca : Pfs(source))
    {
        if (!nca.name.ends_with(".nca") || nca.size < 0xc00) continue;
        const auto header = source->Read(nca.offset, 0xc00);
        const auto program_title = Get(header, 0x210, 8);
        if (header.substr(0x200, 4) != "DNCA" || static_cast<unsigned char>(header[0x205]) != 0 ||
            (program_title & ~u64{0x800}) != (title & ~u64{0x800}) || Get(header, 0x218, 4) != 0) continue;
        // Patch NCAs can carry the base title ID. The indirect RomFS table,
        // rather than a filename/version or title bit alone, distinguishes them.
        bool patch = (program_title & 0x800) != 0;
        for (size_t i = 0; i < 4; ++i)
            if (Get(header, 0x240 + i * 16, 4) != Get(header, 0x244 + i * 16, 4))
                patch |= Get(header, 0x400 + i * 512 + 0x108, 8) != 0;
        if (patch != ((title & 0x800) != 0)) continue;
        Check(part.empty(), "Ambiguous source program content");
        part = ContentPart(nca.size, header);
    }
    return part;
}
struct Section
{
    std::string name, header;
    Store storage;
    u64 title;
    u32 content_index, index;
    unsigned type;
};
std::vector<Section> Sections(const std::string & name, Store nca)
{
    std::vector<Section> result;
    Bounds(0, 0xc00, nca->size);
    const auto h = nca->Read(0x200, 0x200);
    Check(h.substr(0, 4) == "DNCA", "Offline export currently requires plaintext DNCA containers");
    for (u32 i = 0; i < 4; ++i)
    {
        const auto first = Get(h, 0x40 + i * 16, 4), last = Get(h, 0x44 + i * 16, 4);
        if (first == last) continue;
        Check(last > first, "NCA section order");
        Bounds(first * 512, (last - first) * 512, nca->size);
        result.push_back({name, nca->Read(0x400 + i * 512, 512), std::make_shared<Slice>(nca, first * 512, (last - first) * 512), Get(h, 0x10, 8), static_cast<u32>(Get(h, 0x18, 4)), i, static_cast<unsigned char>(h[5])});
    }
    return result;
}
Store DataLayer(const Section & s)
{
    const auto & h = s.header;
    u64 offset{}, size{};
    if (h[3] == 3)
    {
        const auto layers = Get(h, 20, 4);
        Check(layers >= 2 && layers <= 7, "IVFC levels");
        offset = Get(h, 24 + (layers - 2) * 24, 8);
        size = Get(h, 32 + (layers - 2) * 24, 8);
    }
    else if (h[3] == 2)
    {
        const auto layers = Get(h, 44, 4);
        Check(layers >= 1 && layers <= 5, "Hash levels");
        offset = Get(h, 48 + (layers - 1) * 16, 8);
        size = Get(h, 56 + (layers - 1) * 16, 8);
    }
    else
        return {};
    return std::make_shared<Slice>(s.storage, offset, size);
}
Store Patched(const Section & base, const Section & patch)
{
    const auto & h = patch.header;
    // Newer updates set a metadata hash (type 1) over the BKTR tables. The tables
    // and data are unchanged, and every program found is still verified.
    Check(h[2] == 0 && h[3] == 3 && h[5] <= 1 && Get(h, 0x150, 8) == 0 && Get(h, 0x180, 8) == 0, "Unsupported plaintext BKTR layer");
    const auto off = Get(h, 0x100, 8), size = Get(h, 0x108, 8), count = Get(h, 0x118, 4);
    Check(h.substr(0x110, 4) == "BKTR" && Get(h, 0x114, 4) == 1 && count && count <= 10000000 && Get(h, 0x11c, 4) == 0, "BKTR header");
    constexpr u64 Node = 0x4000, Capacity = (Node - 16) / 20, PerNode = (Node - 16) / 8;
    const auto sets = (count + Capacity - 1) / Capacity;
    u64 l2{};
    if (sets > PerNode)
    {
        const auto upper = (sets + PerNode - 1) / PerNode;
        Check(upper <= PerNode, "BKTR depth");
        l2 = (sets - (PerNode - (upper - 1)) + PerNode - 1) / PerNode;
    }
    const auto nodes = (1 + l2) * Node;
    Check(nodes + sets * Node <= size, "BKTR table size");
    const auto table = patch.storage->Read(off, nodes + sets * Node);
    const auto end = Get(table, 8, 8);
    Check(Get(table, 0, 4) == 0 && Get(table, 4, 4) > 0 && Get(table, 4, 4) <= PerNode && end > 0, "BKTR root");
    std::vector<Entry> entries;
    u64 previous_end{};
    for (u64 leaf = 0; leaf < sets; ++leaf)
    {
        const auto pos = nodes + leaf * Node, n = Get(table, pos + 4, 4), leaf_end = Get(table, pos + 8, 8);
        Check(Get(table, pos, 4) == leaf && n > 0 && n <= Capacity && entries.size() + n <= count, "BKTR leaf");
        for (u64 j = 0; j < n; ++j)
        {
            const auto at = pos + 16 + j * 20;
            Entry entry{Get(table, at, 8), Get(table, at + 8, 8), static_cast<u32>(Get(table, at + 16, 4))};
            if (j == 0 && leaf) Check(entry.virtual_offset == previous_end, "BKTR leaf continuity");
            entries.push_back(entry);
        }
        Check(leaf_end > entries.back().virtual_offset, "BKTR leaf end");
        previous_end = leaf_end;
    }
    Check(entries.size() == count && previous_end == end, "BKTR count/end");
    auto logical = std::make_shared<Indirect>(base.storage, std::make_shared<Slice>(patch.storage, 0, off), std::move(entries), end);
    const auto layers = Get(h, 20, 4);
    Check(layers >= 2 && layers <= 7, "BKTR IVFC levels");
    return std::make_shared<Slice>(logical, Get(h, 24 + (layers - 2) * 24, 8), Get(h, 32 + (layers - 2) * 24, 8));
}
std::vector<File> Romfs(Store storage)
{
    const auto header = storage->Read(0, 80);
    std::array<u64, 10> h;
    for (size_t i = 0; i < 10; ++i)
        h[i] = Get(header, i * 8, 8);
    Check(h[0] == 80, "RomFS header");
    for (size_t i : {1U, 3U, 5U, 7U})
        Bounds(h[i], h[i + 1], storage->size);
    Check(h[4] <= 128 * 1024 * 1024 && h[8] <= 128 * 1024 * 1024 && h[9] <= storage->size, "RomFS metadata limit");
    const auto dirs = storage->Read(h[3], h[4]), files = storage->Read(h[7], h[8]);
    std::set<u64> seen_dirs, seen_files;
    std::vector<std::pair<u64, std::string>> pending{{0, ""}};
    std::vector<File> result;
    while (!pending.empty())
    {
        auto [offset, parent] = pending.back();
        pending.pop_back();
        if (offset == UINT32_MAX) continue;
        Check(seen_dirs.insert(offset).second, "RomFS directory cycle");
        Bounds(offset, 24, dirs.size());
        const auto sibling = Get(dirs, offset + 4, 4), child = Get(dirs, offset + 8, 4), length = Get(dirs, offset + 20, 4);
        Bounds(offset + 24, length, dirs.size());
        const auto name = dirs.substr(offset + 24, length);
        const auto path = parent + (parent.empty() || name.empty() ? "" : "/") + name;
        pending.emplace_back(sibling, parent);
        pending.emplace_back(child, path);
        auto file = Get(dirs, offset + 12, 4);
        while (file != UINT32_MAX)
        {
            Check(seen_files.insert(file).second, "RomFS file cycle");
            Bounds(file, 32, files.size());
            const auto following = Get(files, file + 4, 4), data = Get(files, file + 8, 8), size = Get(files, file + 16, 8), n = Get(files, file + 28, 4);
            Bounds(file + 32, n, files.size());
            Check(data <= storage->size - h[9], "RomFS payload overflow");
            Bounds(h[9] + data, size, storage->size);
            result.push_back({path + "/" + files.substr(file + 32, n), h[9] + data, size});
            file = following;
            Check(result.size() <= 1000000, "RomFS file limit");
        }
    }
    return result;
}
void ReplaceIndex(const std::filesystem::path & path, const JsonValue & value)
{
    auto temp = path;
    temp += ".tmp";
    const auto bytes = Canonical(value);
    Check(bytes.size() <= 64 * 1024 * 1024, "Discovery index size limit");
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), bytes.size());
    out.flush();
    Check(bool(out), "Cannot save discovery index");
    out.close();
#ifdef _WIN32
    Check(MoveFileExW(std::filesystem::path(temp).c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0, "Cannot publish discovery index");
#else
    std::filesystem::rename(temp, path);
#endif
}
} // namespace

void ResolveRecordedSources(ExportOptions & options)
{
    Check(ValidContext(options.context), "No identified shader capture. Play and stop this version before exporting.");
    Check(options.title_id && !options.base.empty(), "Missing game identity");
    const auto base = std::make_shared<Physical>(Local(options.game_root, options.base), options.base);
    const auto base_part = ProgramPart(base, options.title_id);
    Check(!base_part.empty(), "Recorded base game is unavailable");
    options.update.clear();
    if (ContentContext(options.title_id, base_part, {}) == options.context) return;
    const auto embedded_update = ProgramPart(base, options.title_id | 0x800);
    if (!embedded_update.empty() && ContentContext(options.title_id, base_part, embedded_update) == options.context)
    {
        options.update = options.base;
        return;
    }

    std::vector<std::string> candidates;
    size_t entries{};
    for (const auto & entry : std::filesystem::directory_iterator(options.game_root))
    {
        Check(++entries <= 65536, "Too many files in game resource directory");
        if (!entry.is_regular_file()) continue;
        auto extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension != ".dnsp" && extension != ".nsp" && extension != ".dxci" && extension != ".xci") continue;
        const auto filename = entry.path().filename().u8string();
        candidates.emplace_back(filename.begin(), filename.end());
    }
    std::sort(candidates.begin(), candidates.end());
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        if (options.progress) Check(options.progress("identify-content", i, candidates.size()), "Shader profile operation cancelled");
        try
        {
            const auto source = std::make_shared<Physical>(Local(options.game_root, candidates[i]), candidates[i]);
            const auto part = ProgramPart(source, options.title_id | 0x800);
            if (!part.empty() && ContentContext(options.title_id, base_part, part) == options.context)
            {
                options.update = candidates[i];
                return;
            }
        }
        catch (const std::exception &) { /* Unrelated/encrypted containers are not export sources. */ }
    }
    throw std::runtime_error("The update used for this capture was not found beside the game. Place that update there and export again.");
}

void Discover(const ExportOptions & options, JsonValue & profile, const std::map<std::string, std::string> & codes, ExportReport & report)
{
    IndexLock index_lock(options.index);
    // Locators name the NCAs of the base and update, never their container files.
    std::map<std::string, Store> containers, physical;
    std::map<std::string, std::vector<std::string>> contents;
    JsonValue identity;
    identity["title"] = Jnum(options.title_id);
    identity["containers"] = Array();
    identity["files"] = Array();
    for (const auto & name : {options.base, options.update})
        if (!name.empty() && !containers.contains(name))
        {
            const auto path = Local(options.game_root, name);
            const auto container = containers[name] = std::make_shared<Physical>(path, name);
            JsonValue c;
            c["name"] = name;
            c["size"] = Jnum(container->size);
            c["mtime"] = FileTimestamp(std::filesystem::last_write_time(path));
            identity["containers"].Append(std::move(c));
            for (const auto & file : Pfs(container))
            {
                if (!file.name.ends_with(".nca") || !ValidResourceName(file.name)) continue;
                contents[name].push_back(file.name);
                if (physical.contains(file.name)) continue;
                const auto nca = physical[file.name] = std::make_shared<Content>(container, file.offset, file.size, file.name);
                JsonValue f;
                f["name"] = file.name;
                f["size"] = Jnum(nca->size);
                f["header"] = ResourceSha256(nca->Read(0, std::min<u64>(nca->size, 4096)));
                identity["files"].Append(std::move(f));
            }
        }
    Check(identity["files"].size() <= 1024, "Too many game contents");
    profile["source_files"] = identity["files"];
    if (!options.context.empty()) {
        std::array<std::string,2> parts;
        const std::array<std::string,2> names{options.base,options.update};
        for (size_t role=0;role<2;++role) if (!names[role].empty()) {
            const auto storage=containers.at(names[role]);
            parts[role]=ProgramPart(storage,options.title_id | (role?0x800:0));
            Check(!parts[role].empty(),"Selected source program content missing");
        }
        Check(ContentContext(options.title_id,parts[0],parts[1])==options.context,
            "Selected base/update differs from the recorded game content");
    }
    JsonValue targets = Array();
    for (const auto & [digest, code] : codes)
        targets.Append(digest);
    const auto target_hash = ResourceSha256(Canonical(targets) + ":exact-1");
    JsonValue index;
    index["version"] = 1U;
    index["identity"] = identity;
    index["target_hash"] = target_hash;
    index["references"] = JsonValue(JsonValueType::Object);
    index["completed"] = Array();
    std::set<std::string> completed;
    // A changed capture reuses verified locators but invalidates scan completion.
    if (!options.index.empty() && std::filesystem::exists(options.index))
    {
        Check(std::filesystem::file_size(options.index) <= 64 * 1024 * 1024, "Discovery index size limit");
        std::ifstream in(options.index, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), {});
        const auto saved = ParseMetadata(bytes, 64 * 1024 * 1024);
        auto payload = saved;
        payload.removeMember("checksum");
        const bool integrity = saved["checksum"].isNull() || saved["checksum"].asString() == ResourceSha256(Canonical(payload));
        if (integrity && Num(saved["version"]) == 1 && saved["references"].isObject())
        {
            index["references"] = saved["references"];
            if (!saved["checksum"].isNull() && Canonical(saved["identity"]) == Canonical(identity) && saved["target_hash"].asString() == target_hash)
                for (const auto & id : saved["completed"])
                    completed.insert(id.asString());
        }
    }
    const auto progress = [&](const char * phase, u64 done, u64 total) {if(options.progress && !options.progress(phase,done,total))throw std::runtime_error("Shader profile operation cancelled"); };
    auto save = [&] {if(options.index.empty())return;index["completed"]=Array();for(const auto& id:completed)index["completed"].Append(id);index.removeMember("checksum");index["checksum"]=ResourceSha256(Canonical(index));ReplaceIndex(options.index,index); };
    const auto read_reference = [&](const JsonValue & ref, size_t prefix) {
        const auto kind = ref["encoding"].asString();
        std::string raw;
        if (kind == "raw") return physical.at(ref["file"].asString())->Read(Num(ref["offset"]), Num(ref["size"]));
        if (kind == "bktr-raw" || kind == "bktr-yaz0")
        {
            const auto expected = Num(ref["resource_size"]);
            Check(expected <= MaxDecodedResource && ref["physical_spans"].size() <= 65536, "Index resource bounds");
            for (const auto & span : ref["physical_spans"])
            {
                Check(Num(span["size"]) <= expected - raw.size(), "Index span bounds");
                raw += physical.at(span["file"].asString())->Read(Num(span["offset"]), Num(span["size"]));
            }
            Check(raw.size() == expected && ResourceSha256(raw) == ref["resource_sha256"].asString(), "Index resource changed");
        }
        else
            raw = physical.at(ref["file"].asString())->Read(Num(ref["container_offset"]), Num(ref["container_size"]));
        if (kind == "yaz0" || kind == "bktr-yaz0") return DecodeYaz0(raw, prefix);
        if (kind == "nso-lz4")
        {
            Check(ResourceSha256(raw) == ref["container_sha256"].asString(), "Index NSO changed");
            auto decoded = DecodeLz4(raw, Num(ref["decoded_size"]));
            Check(ResourceSha256(decoded) == ref["segment_sha256"].asString(), "Index NSO segment changed");
            return decoded;
        }
        Check(kind == "bktr-raw", "Unknown index encoding");
        return raw;
    };
    // Verify grouped locators once per resource; never trust timestamps as proof.
    std::map<std::string, std::vector<std::string>> groups;
    for (const auto & [h, code] : codes)
        if (index["references"].isMember(h.c_str()))
        {
            auto descriptor = index["references"][h];
            const std::set<std::string> allowed{"encoding", "file", "offset", "size", "container_offset", "container_size", "decoded_size", "container_sha256", "segment_sha256", "physical_spans", "resource_size", "resource_sha256"};
            bool valid = descriptor.isObject();
            for (const auto & field : descriptor.GetMemberNames())
                valid &= allowed.contains(field);
            if (!valid)
            {
                index["references"].removeMember(h.c_str());
                completed.clear();
                continue;
            }
            if (descriptor["encoding"].asString() != "raw")
            {
                descriptor.removeMember("offset");
                descriptor.removeMember("size");
            }
            groups[Canonical(descriptor)].push_back(h);
        }
    for (const auto & [group, hashes] : groups)
    {
        progress("verify-index", report.reused, codes.size());
        try
        {
            size_t prefix{};
            for (const auto & h : hashes)
            {
                const auto & r = index["references"][h];
                if (r["encoding"].asString() == "raw") continue;
                const auto offset = Num(r["offset"]), size = Num(r["size"]);
                Bounds(offset, size, MaxDecodedResource);
                prefix = std::max(prefix, static_cast<size_t>(offset + size));
            }
            const auto & first = index["references"][hashes.front()];
            const auto decoded = read_reference(first, prefix);
            for (const auto & h : hashes)
            {
                const auto & ref = index["references"][h];
                const auto offset = ref["encoding"].asString() == "raw" ? 0 : Num(ref["offset"]);
                const auto size = Num(ref["size"]);
                Check(size == codes.at(h).size(), "Index shader size");
                Bounds(offset, size, decoded.size());
                if (std::string_view(decoded).substr(offset, size) == std::string_view(codes.at(h)).substr(0, size))
                {
                    profile["shaders"][h]["references"].Append(ref);
                    ++report.reused;
                }
                else
                {
                    index["references"].removeMember(h.c_str());
                    completed.clear();
                }
            }
        }
        catch (const std::exception &)
        {
            for (const auto & h : hashes)
                index["references"].removeMember(h.c_str());
            completed.clear();
        }
    }
    u64 found_programs = report.reused;
    progress("programs", found_programs, codes.size());
    const auto done = [&] { return found_programs == codes.size(); };
    if (done())
    {
        save();
        return;
    }
    std::vector<PrefixSearch::Prefix> prefixes;
    std::map<std::string, std::vector<std::string>> by_prefix;
    std::vector<std::string> short_codes;
    for (const auto & [h, code] : codes)
        if (profile["shaders"][h]["references"].empty())
        {
            if (code.size() < 16)
                short_codes.push_back(h);
            else
                by_prefix[code.substr(0, 16)].push_back(h);
        }
    for (const auto & [prefix, entries] : by_prefix)
    {
        PrefixSearch::Prefix p;
        std::memcpy(p.data(), prefix.data(), 16);
        prefixes.push_back(p);
    }
    std::unique_ptr<PrefixSearch> search;
    if (!prefixes.empty()) search = std::make_unique<PrefixSearch>(prefixes);
    const auto bind = [&](const std::string & h, size_t pos, JsonValue locator, size_t size) {if(!profile["shaders"][h]["references"].empty())return;locator["offset"]=Jnum(pos);locator["size"]=Jnum(size);profile["shaders"][h]["references"].Append(locator);index["references"][h]=std::move(locator);progress("programs", ++found_programs, codes.size()); };
    const auto locate = [&](const std::string & data, const JsonValue & locator, u64 base = 0) {
        if (search)
        {
            size_t resume{};
            std::array<size_t, 1024> hits;
            while (resume < data.size())
            {
                const auto count = search->Scan({reinterpret_cast<const unsigned char *>(data.data()), data.size()}, resume, hits);
                for (size_t i = 0; i < count; ++i)
                {
                    const auto pos = hits[i];
                    for (const auto & h : by_prefix.at(data.substr(pos, 16)))
                        if (profile["shaders"][h]["references"].empty())
                        {
                            const auto & code = codes.at(h);
                            if (code.size() <= data.size() - pos && data.compare(pos, code.size(), code) == 0)
                                bind(h, pos + base, locator, code.size());
                        }
                }
            }
        }
        for (const auto & h : short_codes)
            if (profile["shaders"][h]["references"].empty())
            {
                const auto pos = data.find(codes.at(h));
                if (pos != std::string::npos) bind(h, pos + base, locator, codes.at(h).size());
            }
    };
    try
    {
        // Sequential raw scans use enough lookahead for the entire longest program.
        size_t overlap{};
        for (const auto & [h, code] : codes)
            overlap = std::max(overlap, code.size() - 1);
        std::vector<std::string> scanned;
        for (const auto & container : {options.base, options.update})
            for (const auto & name : contents[container])
            {
                if (std::find(scanned.begin(), scanned.end(), name) != scanned.end()) continue;
                scanned.push_back(name);
                auto storage = physical.at(name);
                std::string tail;
                constexpr u64 Chunk = 32 * 1024 * 1024;
                for (u64 off = 0; off < storage->size; off += Chunk)
                {
                    progress("raw", off, storage->size);
                    const auto n = std::min(Chunk, storage->size - off);
                    const auto id = "raw:" + name + ":" + std::to_string(off) + ":" + std::to_string(n);
                    if (completed.contains(id))
                    {
                        tail = storage->Read(off + n - std::min<u64>(overlap, n), std::min<u64>(overlap, n));
                        continue;
                    }
                    auto data = tail + storage->Read(off, n);
                    JsonValue locator;
                    locator["encoding"] = "raw";
                    locator["file"] = name;
                    locate(data, locator, off - tail.size());
                    tail = data.substr(data.size() - std::min(overlap, data.size()));
                    completed.insert(id);
                    if (done())
                    {
                        save();
                        return;
                    }
                    if (off % (256 * 1024 * 1024) == 0) save();
                }
            }
        std::map<std::string, std::vector<Section>> sections;
        for (const auto & [container, names] : contents)
            for (const auto & name : names)
                for (auto & section : Sections(name, physical.at(name)))
                    sections[container].push_back(std::move(section));
        // One resource: a whole file, or a Yaz0 entry of an uncompressed archive.
        auto resource = [&](Store storage, u64 offset, u64 size, const std::string & id, bool logical) {
            if (completed.contains(id)) return;
            progress("resources", report.searched_resources, 0);
            Check(size <= MaxDecodedResource, "Resource decode limit");
            auto raw = storage->Read(offset, size);
            const bool compressed = raw.starts_with("Yaz0");
            auto decoded = compressed ? DecodeYaz0(raw) : raw;
            report.decoded_bytes += decoded.size();
            ++report.searched_resources;
            JsonValue locator;
            const auto spans = storage->Spans(offset, size);
            if (!logical && compressed)
            {
                Check(spans.size() == 1, "Physical Yaz0 span count");
                locator["encoding"] = "yaz0";
                locator["file"] = spans[0]["file"];
                locator["container_offset"] = spans[0]["offset"];
                locator["container_size"] = Jnum(size);
            }
            else
            {
                locator["encoding"] = compressed ? "bktr-yaz0" : "bktr-raw";
                locator["physical_spans"] = spans;
                locator["resource_size"] = Jnum(raw.size());
                locator["resource_sha256"] = ResourceSha256(raw);
            }
            locate(decoded, locator);
            completed.insert(id);
            if (report.searched_resources % 25 == 0) save();
        };
        // Uncompressed SARC archives can hold Yaz0-compressed entries (menu.sarc and
        // race.sarc in Mario Kart 8 Deluxe). Each such entry is searched like a
        // compressed file; an archive that does not parse is skipped.
        auto archive = [&](Store storage, const File & file, const std::string & id, bool logical) {
            std::vector<std::pair<u64, u64>> entries;
            try
            {
                if (file.size < 0x20) return;
                const auto header = storage->Read(file.offset, 0x20);
                if (!header.starts_with("SARC") || header.substr(0x14, 4) != "SFAT") return;
                const bool little = static_cast<unsigned char>(header[6]) == 0xff;
                const auto number = [&](std::string_view bytes, size_t pos, size_t n) {
                    u64 value = Get(bytes, pos, n);
                    if (little) return value;
                    u64 swapped{};
                    for (size_t i = 0; i < n; ++i) swapped = (swapped << 8) | ((value >> (8 * i)) & 0xff);
                    return swapped;
                };
                const auto data = number(header, 12, 4), count = number(header, 0x1a, 2);
                if (data > file.size || 0x20 + count * 16 > file.size) return;
                const auto nodes = storage->Read(file.offset + 0x20, count * 16);
                for (u64 i = 0; i < count; ++i)
                {
                    const auto start = number(nodes, i * 16 + 8, 4), end = number(nodes, i * 16 + 12, 4);
                    if (start > end || end > file.size - data || end - start < 16) continue;
                    if (storage->Read(file.offset + data + start, 4) == "Yaz0") entries.emplace_back(file.offset + data + start, end - start);
                }
            }
            catch (const std::exception &)
            {
                return;
            }
            for (size_t i = 0; i < entries.size() && !done(); ++i)
                resource(storage, entries[i].first, entries[i].second, id + ":" + std::to_string(i), logical);
        };
        for (const auto & name : {options.base, options.update})
            if (!name.empty())
                for (const auto & section : sections[name])
                {
                    if (Get(section.header, 0x108, 8)) continue;
                    auto layer = DataLayer(section);
                    if (!layer) continue;
                    if (section.header[2] == 1)
                        for (const auto & file : Pfs(layer))
                        {
                            if (file.size < 256) continue;
                            const auto header = layer->Read(file.offset, 256);
                            if (!header.starts_with("NSO0")) continue;
                            const auto flags = Get(header, 12, 4);
                            Check(!(flags & 128), "ZBIC NSO not supported");
                            for (unsigned segment = 0; segment < 3; ++segment)
                            {
                                const auto source = Get(header, 16 + segment * 16, 4), size = Get(header, 24 + segment * 16, 4);
                                const bool compressed = (flags & (1 << segment)) != 0;
                                const auto stored = compressed ? Get(header, 96 + segment * 4, 4) : size;
                                Bounds(source, stored, file.size);
                                Check(size <= MaxDecodedResource, "NSO segment size");
                                const auto raw = layer->Read(file.offset + source, stored);
                                auto decoded = compressed ? DecodeLz4(raw, size) : raw;
                                const auto hash = ResourceSha256(decoded);
                                if (flags & (1 << (segment + 3))) Check(hash == [&] {std::string s;constexpr char hex[]="0123456789abcdef";for(unsigned i=0;i<32;++i){const auto b=static_cast<unsigned char>(header[160+segment*32+i]);s+=hex[b>>4];s+=hex[b&15];}return s; }(), "NSO segment hash");
                                if (compressed)
                                {
                                    const auto spans = layer->Spans(file.offset + source, stored);
                                    Check(spans.size() == 1, "NSO physical span count");
                                    JsonValue locator;
                                    locator["encoding"] = "nso-lz4";
                                    locator["file"] = spans[0]["file"];
                                    locator["container_offset"] = spans[0]["offset"];
                                    locator["container_size"] = Jnum(stored);
                                    locator["decoded_size"] = Jnum(size);
                                    locator["container_sha256"] = ResourceSha256(raw);
                                    locator["segment_sha256"] = hash;
                                    locate(decoded, locator);
                                }
                                if (done())
                                {
                                    save();
                                    return;
                                }
                            }
                        }
                    else if (options.update.empty() && section.header[2] == 0)
                        for (const auto & file : Romfs(layer))
                        {
                            const auto id = "physical:" + name + ":" + section.name + ":" + file.name;
                            if (file.size >= 4 && layer->Read(file.offset, 4) == "Yaz0") resource(layer, file.offset, file.size, id, false);
                            else archive(layer, file, id, false);
                            if (done())
                            {
                                save();
                                return;
                            }
                        }
                }
        if (!options.update.empty())
            for (const auto & patch : sections[options.update])
                if (patch.type == 0 && patch.header[2] == 0 && Get(patch.header, 0x108, 8))
                {
                    const Section * base = nullptr;
                    for (const auto & candidate : sections[options.base])
                        if (candidate.type == patch.type && candidate.index == patch.index && candidate.content_index == patch.content_index && candidate.title == (patch.title & ~u64{0x800}) && !Get(candidate.header, 0x108, 8))
                        {
                            Check(!base, "Ambiguous BKTR base");
                            base = &candidate;
                        }
                    Check(base, "BKTR base content missing");
                    auto storage = Patched(*base, patch);
                    for (const auto & file : Romfs(storage))
                    {
                        const auto id = "logical:" + patch.name + ":" + std::to_string(patch.index) + ":" + file.name;
                        resource(storage, file.offset, file.size, id, true);
                        if (!done()) archive(storage, file, id, true);
                        if (done())
                        {
                            save();
                            return;
                        }
                    }
                }
        // Exact-match fallback for obsolete physical resources retained in a capture.
        if (!options.update.empty())
            for (const auto & name : {options.base, options.update})
                for (const auto & section : sections[name])
                    if (section.header[2] == 0 && !Get(section.header, 0x108, 8))
                        if (auto layer = DataLayer(section))
                            for (const auto & file : Romfs(layer))
                            {
                                const auto id = "physical:" + name + ":" + section.name + ":" + file.name;
                                if (file.size >= 4 && layer->Read(file.offset, 4) == "Yaz0") resource(layer, file.offset, file.size, id, false);
                                else archive(layer, file, id, false);
                                if (done())
                                {
                                    save();
                                    return;
                                }
                            }
        save();
    }
    catch (...)
    {
        try
        {
            save();
        }
        catch (...)
        {
        }
        throw;
    }
    for (const auto & h : profile["shaders"].GetMemberNames())
        report.missing += profile["shaders"][h]["references"].empty();
}
} // namespace VideoCommon::ShaderProfile
