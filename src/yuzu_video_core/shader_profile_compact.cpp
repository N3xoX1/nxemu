// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile_compact.h"
#include "yuzu_video_core/shader_profile_codec.h"
#include "yuzu_video_core/shader_profile_container.h"
#include "yuzu_video_core/shader_profile_export.h"
#include "yuzu_video_core/shader_profile_hash.h"
#include "yuzu_video_core/shader_profile_metrics.h"
#include "yuzu_video_core/shader_profile_resources.h"
#include "yuzu_video_core/shader_profile_validation.h"
#include "yuzu_video_core/shader_profile_context.h"
#include "yuzu_video_core/shader_profile_nx.h"
#include "yuzu_video_core/shader_profile_guest_state.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <system_error>
#include <thread>
#include <set>
#include <span>
#include <tuple>
#include <utility>

namespace VideoCommon::ShaderProfile
{
namespace
{
void Check(bool b, const char * reason)
{
    if (!b) throw std::runtime_error(reason);
}
void Allowed(const JsonValue & value, std::initializer_list<const char *> fields)
{
    Check(value.isObject(), "Expected compact metadata object");
    for (const auto & field : value.GetMemberNames())
        Check(std::find(fields.begin(), fields.end(), field) != fields.end(), "Unknown compact metadata field");
}
std::string Hash(std::string_view bytes, Timing::Metrics * timings)
{
    return Timing::Measure(timings, Timing::Phase::ResourceVerification, [&] { return ResourceSha256(bytes); });
}
u64 Number(const JsonValue & n)
{
    if (n.isString())
    {
        const auto s = n.asString();
        Check(!s.empty() && s.size() <= 20 && s.find_first_not_of("0123456789") == std::string::npos, "Invalid compact integer");
        return std::stoull(s);
    }
    Check(n.Type() == JsonValueType::UnsignedInt || (n.Type() == JsonValueType::Int && n.asInt64() >= 0), "Expected compact integer");
    return n.asUInt64();
}
struct Writer
{
    std::string bytes;
    void Fixed(u64 n, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
            bytes += static_cast<char>(n >> (i * 8));
    }
    void Var(u64 n)
    {
        while (n >= 128)
        {
            bytes += static_cast<char>((n & 127) | 128);
            n >>= 7;
        }
        bytes += static_cast<char>(n);
    }
    void Text(std::string_view s)
    {
        Var(s.size());
        bytes += s;
    }
    void Hash(const std::string & h)
    {
        Check(h.size() == 64 && h.find_first_not_of("0123456789abcdef") == std::string::npos, "Invalid compact SHA-256");
        for (size_t i = 0; i < 64; i += 2)
        {
            const auto hex = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
            bytes += static_cast<char>((hex(h[i]) << 4) | hex(h[i + 1]));
        }
    }
};
struct Reader
{
    std::string_view data;
    size_t pos{};
    std::string_view Take(size_t n)
    {
        Check(pos <= data.size() && n <= data.size() - pos, "Truncated compact table");
        const auto result = data.substr(pos, n);
        pos += n;
        return result;
    }
    u64 Fixed(size_t n)
    {
        const auto b = Take(n);
        u64 v{};
        for (size_t i = 0; i < n; ++i)
            v |= u64{static_cast<unsigned char>(b[i])} << (i * 8);
        return v;
    }
    u64 Var()
    {
        u64 value{};
        for (unsigned i = 0; i < 10; ++i)
        {
            const auto b = Fixed(1);
            Check(i < 9 || b <= 1, "Compact varint overflow");
            value |= (b & 127) << (7 * i);
            if (b < 128)
            {
                Check(i == 0 || b != 0, "Noncanonical compact varint");
                return value;
            }
        }
        throw std::runtime_error("Compact varint overflow");
    }
    u32 Word()
    {
        const auto n = Var();
        Check(n <= UINT32_MAX, "Compact word overflow");
        return static_cast<u32>(n);
    }
    size_t Count(size_t maximum)
    {
        const auto n = Var();
        Check(n <= maximum && n <= data.size() - pos, "Compact table limit");
        return static_cast<size_t>(n);
    }
    std::string Text(size_t maximum = 4096)
    {
        const auto n = Count(maximum);
        return std::string(Take(n));
    }
    std::string Hash()
    {
        const auto b = Take(32);
        constexpr char hex[] = "0123456789abcdef";
        std::string h;
        h.reserve(64);
        for (unsigned char c : b)
        {
            h += hex[c >> 4];
            h += hex[c & 15];
        }
        return h;
    }
};
const std::array<const char *, 7> Fields{"local_memory_size", "texture_bound", "guest_start_address", "guest_read_lowest", "guest_read_highest", "viewport_transform_state", "stage"};
// Not stored: both follow from the start address and the program size.
constexpr size_t StartAddress = 2, ReadLowest = 3, ReadHighest = 4;
const std::array<const char *, 4> Maps{"texture_types", "texture_pixel_formats", "cbuf_values", "cbuf_replacements"};
const std::array<const char *, 7> Encodings{"raw", "yaz0", "nso-lz4", "bktr-raw", "bktr-yaz0", "zstd", "bktr-zstd"};
// Resources stored as BKTR spans of their bytes, rather than one range of one NCA.
bool Spanned(u64 kind)
{
    return kind == 3 || kind == 4 || kind == 6;
}
bool Zstd(u64 kind)
{
    return kind == 5 || kind == 6;
}
// The zstd dictionaries games ship are small: TotK's three are 128 KiB each.
constexpr u64 MaxZstdDictionary = 16 * 1024 * 1024;
// The resource a dictionary locator reads from: a zstd resource with no dictionary of its own.
JsonValue DictionaryResource(const JsonValue & dictionary)
{
    Allowed(dictionary, {"encoding", "file", "container_offset", "container_size", "decoded_size", "physical_spans", "resource_size", "resource_sha256", "offset", "size", "sha256"});
    const auto encoding = dictionary["encoding"].asString();
    Check(encoding == "zstd" || encoding == "bktr-zstd", "Compact dictionary encoding");
    const auto size = Number(dictionary["size"]);
    Check(size && size <= MaxZstdDictionary && dictionary["sha256"].isString(), "Compact dictionary bounds");
    auto resource = dictionary;
    resource.removeMember("offset");
    resource.removeMember("size");
    resource.removeMember("sha256");
    return resource;
}
struct Span
{
    std::string file;
    u64 offset{}, size{};
};
struct Resource
{
    unsigned kind{};
    std::string file, hash, segment_hash;
    u64 offset{}, size{}, decoded_size{};
    std::vector<Span> spans;
    // A zstd frame's dictionary: bytes [offset, offset + size) of another zstd
    // resource, decoded, with their SHA-256. dictionary is that resource's index + 1.
    u64 dictionary{}, dictionary_offset{}, dictionary_size{};
    std::string dictionary_hash;
};
struct Locator
{
    u32 resource{};
    u64 offset{};
};
struct Program
{
    // Named by the first 8 bytes of the SHA-256 of its bytes; hash is the
    // emulator's own, found once loaded.
    std::string sha;
    u64 hash{};
    u64 size{};
    std::vector<Locator> locators;
};
bool Verify(std::string_view code, const Program & program, Timing::Metrics * timings)
{
    return Timing::Measure(timings, Timing::Phase::ResourceVerification, [&] {
        const auto digest = ResourceDigest(code);
        return code.size() == program.size && std::equal(program.sha.begin(), program.sha.end(), digest.begin(),
                                                         [](char a, unsigned char b) { return static_cast<unsigned char>(a) == b; });
    });
}
constexpr u64 NoSlot = ~u64{};
// Recipe slot of a graphics environment stage: VertexA (6) first, then stages 0-4.
u64 GraphicsSlot(u64 stage)
{
    return stage == 6 ? 0 : stage < 5 ? stage + 1 : NoSlot;
}
struct Env
{
    u32 program{};
    std::array<u32, 7> fields{};
    std::array<std::vector<std::pair<u64, u32>>, 4> maps;
    std::array<u32, 3> workgroup{};
    u32 shared{};
    std::array<u32, 8> passthrough{};
};
// Where one NCA named by a profile is, in the game containers beside the game.
struct ContentLocation
{
    // In an .nx the NCA is read through the archive: path is then the archive
    // path joined with the NCA name, a key only, and offset is 0.
    std::filesystem::path path;
    u64 offset{}, size{};
    std::shared_ptr<NxGame> nx;
    u32 file{};
};
// Reads the file that holds one content: a container, or an NCA of an .nx.
class ContentInput
{
public:
    void Open(const ContentLocation & content)
    {
        nx = content.nx;
        file = content.file;
        if (nx) return;
        input.close();
        input.clear();
        input.open(content.path, std::ios::binary);
        Check(bool(input), "Cannot open compact resource");
    }
    void Read(u64 offset, std::span<char> bytes)
    {
        if (nx)
        {
            nx->Read(file, offset, bytes);
            return;
        }
        input.clear();
        input.seekg(static_cast<std::streamoff>(offset));
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        Check(bool(input), "Cannot read compact resource");
    }

private:
    std::ifstream input;
    std::shared_ptr<NxGame> nx;
    u32 file{};
};
using ContentMap = std::map<std::string, ContentLocation>;
const ContentLocation & Locate(const ContentMap & contents, const std::string & name)
{
    const auto found = contents.find(name);
    if (found == contents.end())
        throw std::filesystem::filesystem_error("Game content not found", std::filesystem::u8path(name),
                                                std::make_error_code(std::errc::no_such_file_or_directory));
    return found->second;
}
std::string Bytes(const ContentMap & contents, const std::string & name, u64 off, u64 size, Timing::Metrics * timings)
{
    Timing::Timer timer(timings, Timing::Phase::ResourceIO);
    const auto & content = Locate(contents, name);
    Check(size <= MaxDecodedResource && off <= content.size && size <= content.size - off, "Compact resource range");
    std::string bytes(static_cast<size_t>(size), '\0');
    ContentInput in;
    in.Open(content);
    in.Read(content.offset + off, std::span(bytes));
    if (timings) timings->Add(Timing::Counter::ResourceReadBytes, size);
    return bytes;
}
// Limit read amplification and temporary memory while avoiding a seek/read for
// every small shader or patch span. These limits do not affect recipe ordering.
constexpr u64 ReadBatchBytes = 4 * 1024 * 1024;
constexpr u64 ReadBatchGap = 64 * 1024;
void ReadInto(ContentInput& input, u64 offset, std::span<char> bytes, Timing::Metrics* timings)
{
    Timing::Timer io(timings, Timing::Phase::ResourceIO);
    input.Read(offset, bytes);
    if (timings) timings->Add(Timing::Counter::ResourceReadBytes, bytes.size());
}
std::string SpanBytes(const ContentMap& contents, const Resource& resource, Timing::Metrics* timings)
{
    struct Read { const ContentLocation* content; u64 offset, size, destination; };
    std::vector<Read> reads;
    u64 destination{};
    // Resolve every dependency before allocating or reading a partial resource.
    // In particular, an absent older update must not cause large base reads.
    for (const auto& span : resource.spans) {
        const auto& content = Locate(contents, span.file);
        Check(span.offset <= content.size && span.size <= content.size - span.offset,
              "Compact resource range");
        reads.push_back({&content, content.offset + span.offset, span.size, destination});
        destination += span.size;
    }
    std::sort(reads.begin(), reads.end(), [](const Read& a, const Read& b) {
        return std::tie(a.content->path, a.offset) < std::tie(b.content->path, b.offset);
    });
    std::string raw(static_cast<size_t>(resource.size), '\0');
    ContentInput input;
    std::optional<std::filesystem::path> opened;
    for (size_t first = 0; first < reads.size();) {
        const auto& read = reads[first];
        if (opened != read.content->path) {
            input.Open(*read.content);
            opened = read.content->path;
        }
        size_t last = first + 1;
        u64 end = read.offset + read.size;
        while (last < reads.size()) {
            const auto& next = reads[last];
            const auto next_end = std::max(end, next.offset + next.size);
            if (next.content->path != opened || (next.offset > end && next.offset - end > ReadBatchGap) ||
                next_end - read.offset > ReadBatchBytes) break;
            end = next_end; ++last;
        }
        if (last == first + 1) {
            ReadInto(input, read.offset, std::span(raw).subspan(read.destination, read.size), timings);
        } else {
            std::string batch(static_cast<size_t>(end - read.offset), '\0');
            ReadInto(input, read.offset, std::span(batch), timings);
            for (size_t i = first; i < last; ++i) {
                const auto& part = reads[i];
                std::copy_n(batch.data() + part.offset - read.offset, part.size,
                            raw.data() + part.destination);
            }
        }
        first = last;
    }
    return raw;
}
// A Yaz0 archive can include large textures after its last requested shader.
// Decode only enough local bytes to reconstruct every requested program. The
// caller still verifies each program hash; the full archive SHA is checked
// whenever decoding required the entire stored resource.
std::string Yaz0Programs(const ContentMap& contents, const Resource& resource, size_t prefix,
                         Timing::Metrics* timings)
{
    struct InputSpan { const ContentLocation* content; u64 offset, size, logical; };
    std::vector<InputSpan> spans;
    // Validate the complete locator/dependency set before trying a prefix.
    const auto add = [&](const std::string& file, u64 offset, u64 size, u64 logical) {
        const auto& content = Locate(contents, file);
        Check(offset <= content.size && size <= content.size - offset, "Compact resource range");
        spans.push_back({&content, content.offset + offset, size, logical});
    };
    if (Spanned(resource.kind)) {
        u64 logical{};
        for (const auto& span : resource.spans) {
            add(span.file, span.offset, span.size, logical); logical += span.size;
        }
    } else add(resource.file, resource.offset, resource.size, 0);
    std::map<std::filesystem::path, ContentInput> inputs;
    std::string raw;
    raw.reserve(static_cast<size_t>(resource.size));
    u64 count = std::min<u64>(resource.size, 64 * 1024);
    for (;;) {
        const auto loaded = raw.size();
        raw.resize(static_cast<size_t>(count));
        for (const auto& span : spans) {
            const auto begin = std::max<u64>(loaded, span.logical);
            const auto end = std::min(count, span.logical + span.size);
            if (begin >= end) continue;
            auto [it, fresh] = inputs.try_emplace(span.content->path);
            if (fresh) it->second.Open(*span.content);
            ReadInto(it->second, span.offset + begin - span.logical,
                     std::span(raw).subspan(begin, end - begin), timings);
        }
        if (count == resource.size && Spanned(resource.kind))
            Check(Hash(raw, timings) == resource.hash, "Compact resource fingerprint mismatch");
        try {
            return Timing::Measure(timings, Timing::Phase::ResourceDecompression, [&] {
                return DecodeYaz0(raw, prefix);
            });
        } catch (const std::runtime_error& error) {
            const std::string_view reason = error.what();
            // Retry only an incomplete input window. Malformed headers and
            // invalid backreferences are errors, not reasons to read more data.
            if (count == resource.size || (reason != "Truncated Yaz0" && reason != "Truncated Yaz0 literals")) throw;
            count = std::min(resource.size, count * 2);
        }
    }
}
// A zstd resource decoded whole: the frame names its dictionary and size.
std::string ZstdBytes(const ContentMap& contents, const Resource& resource, const ZstdDictionary* dictionary,
                      Timing::Metrics* timings)
{
    auto raw = Spanned(resource.kind) ? SpanBytes(contents, resource, timings) :
        Bytes(contents, resource.file, resource.offset, resource.size, timings);
    if (Spanned(resource.kind)) Check(Hash(raw, timings) == resource.hash, "Compact resource fingerprint mismatch");
    return Timing::Measure(timings, Timing::Phase::ResourceDecompression, [&] {
        return DecodeZstd(raw, static_cast<size_t>(resource.decoded_size), dictionary);
    });
}
SharedShaderBytes Environment(const Env & env, std::shared_ptr<const std::string> program, Timing::Metrics * timings)
{
    const std::string_view code=*program;
    Check(env.fields[6] <= 6 && env.fields[2] == env.fields[3] && env.fields[4] >= env.fields[3] && u64{env.fields[4]} - env.fields[3] + 8 == code.size(), "Compact environment bounds");
    Writer out;
    out.Fixed(code.size(), 8);
    for (const auto & m : env.maps)
        out.Fixed(m.size(), 8);
    for (auto n : env.fields)
        out.Fixed(n, 4);
    for (size_t i = 0; i < 4; ++i)
        for (const auto & [key, value] : env.maps[i])
        {
            out.Fixed(key, i < 2 ? 4 : 8);
            out.Fixed(value, 4);
        }
    if (env.fields[6] == 5)
    {
        for (auto n : env.workgroup)
            out.Fixed(n, 4);
        out.Fixed(env.shared, 4);
    }
    else
    {
        Check(code.size() >= 80, "Compact graphics header size");
        out.bytes += code.substr(0, 80);
        if (env.fields[6] == 3)
            for (auto n : env.passthrough)
                out.Fixed(n, 4);
    }
    return SharedShaderBytes::FromMetadata(std::move(out.bytes),std::move(program));
}
} // namespace

namespace
{
// Payload sections: four ASCII characters, read as a little-endian u32.
constexpr u32 SectionTag(const char (&name)[5])
{
    return u32(u8(name[0])) | u32(u8(name[1])) << 8 | u32(u8(name[2])) << 16 | u32(u8(name[3])) << 24;
}
constexpr u32 MetaSection = SectionTag("META"), ContextSection = SectionTag("CTXT"), SourceSection = SectionTag("SRCF"),
              ResourceSection = SectionTag("RSRC"), ProgramSection = SectionTag("PROG"), EnvironmentSection = SectionTag("ENVS"),
              StateSection = SectionTag("STAT"), BindingSection = SectionTag("BIND"), RecipeSection = SectionTag("RCPS");
constexpr std::array RequiredSections{MetaSection, ContextSection, SourceSection, ResourceSection, ProgramSection,
                                      EnvironmentSection, StateSection, BindingSection, RecipeSection};
constexpr u64 SectionRequired = 1;
// Compiler facts of an environment. Tags below OptionalFacts change compilation:
// a reader that does not know one must refuse the file.
enum Fact : u32
{
    StartAddressFact = 1,
    LocalMemoryFact = 2,
    TextureBoundFact = 3,
    ViewportTransformFact = 4,
    SharedMemoryFact = 5,
    WorkgroupFact = 6,     // key: axis 0-2
    PassthroughFact = 7,   // key: geometry passthrough mask word 0-7
    CbufValueFact = 8,     // key: bank << 32 | byte offset
    CbufSizeFact = 9,      // key: bank
    TextureTableFact = 10, // compute: texture table entries
    TexturePoolFact = 11,  // graphics: texture pool entries
    SamplerPoolFact = 12,  // graphics: sampler pool entries | linked (bit 31)
    TextureTypeFact = 13,  // key: texture handle; value: GuestState::TextureTypeCode
    TextureFormatFact = 14, // key: texture handle; value: GuestState::TextureFormatCode
    CbufReplacementFact = 15, // key: bank << 32 | byte offset; value: 0 base instance, 1 base vertex, 2 draw ID
    OptionalFacts = 0x4000,
};
constexpr u64 PoolTexturesKey = (u64{UINT32_MAX} << 32) | (UINT32_MAX - 1);
constexpr u64 PoolSamplersKey = (u64{UINT32_MAX} << 32) | (UINT32_MAX - 2);
constexpr u64 TextureTableKey = UINT64_MAX;
// Portable stage numbers: pipeline order, then compute.
constexpr std::array<u32, 7> PortableStage{1, 2, 3, 4, 5, 6, 0}; // indexed by Shader::Stage
constexpr std::array<u32, 7> EmulatorStage{6, 0, 1, 2, 3, 4, 5}; // indexed by portable stage
constexpr u32 PortableCompute = 6;
struct FactEntry
{
    u32 tag{};
    u64 key{}, value{};
    auto operator<=>(const FactEntry &) const = default;
};
std::vector<FactEntry> Facts(const Env & env)
{
    std::vector<FactEntry> facts;
    const auto field = [&](u32 tag, u64 value) {
        if (value) facts.push_back({tag, 0, value});
    };
    field(StartAddressFact, env.fields[StartAddress]);
    field(LocalMemoryFact, env.fields[0]);
    field(TextureBoundFact, env.fields[1]);
    field(ViewportTransformFact, env.fields[5]);
    const auto stage = env.fields[6];
    if (stage == 5)
    {
        field(SharedMemoryFact, env.shared);
        for (u32 axis = 0; axis < 3; ++axis)
            if (env.workgroup[axis]) facts.push_back({WorkgroupFact, axis, env.workgroup[axis]});
    }
    if (stage == 3)
        for (u32 word = 0; word < 8; ++word)
            if (env.passthrough[word]) facts.push_back({PassthroughFact, word, env.passthrough[word]});
    for (const auto & [handle, type] : env.maps[0])
        facts.push_back({TextureTypeFact, handle, GuestState::TextureTypeCode(type)});
    for (const auto & [handle, format] : env.maps[1])
    {
        const auto code = GuestState::TextureFormatCode(format);
        Check(code.has_value(), "Texture format without a guest descriptor");
        facts.push_back({TextureFormatFact, handle, *code});
    }
    for (const auto & [key, value] : env.maps[2])
    {
        const auto bank = key >> 32;
        if (key == TextureTableKey)
            facts.push_back({TextureTableFact, 0, value});
        else if (key == PoolTexturesKey)
            facts.push_back({TexturePoolFact, 0, value});
        else if (key == PoolSamplersKey)
            facts.push_back({SamplerPoolFact, 0, value});
        else
        {
            Check(bank < UINT32_MAX, "Unknown environment query");
            if (static_cast<u32>(key) == UINT32_MAX)
                facts.push_back({CbufSizeFact, bank, value});
            else
                facts.push_back({CbufValueFact, key, value});
        }
    }
    for (const auto & [key, value] : env.maps[3])
        facts.push_back({CbufReplacementFact, key, value});
    std::sort(facts.begin(), facts.end());
    return facts;
}
// The inverse of Facts; false for a fact this version does not know.
void Apply(Env & env, const FactEntry & fact)
{
    const auto single = [&](u32 & target) {
        Check(fact.key == 0 && fact.value <= UINT32_MAX, "Invalid environment fact");
        target = static_cast<u32>(fact.value);
    };
    Check(fact.value <= UINT32_MAX, "Invalid environment fact");
    const auto value = static_cast<u32>(fact.value);
    const auto stage = env.fields[6];
    switch (fact.tag)
    {
    case StartAddressFact: single(env.fields[StartAddress]); return;
    case LocalMemoryFact: single(env.fields[0]); return;
    case TextureBoundFact: single(env.fields[1]); return;
    case ViewportTransformFact: single(env.fields[5]); return;
    case SharedMemoryFact: Check(stage == 5, "Invalid environment fact"); single(env.shared); return;
    case WorkgroupFact: Check(stage == 5 && fact.key < 3, "Invalid environment fact"); env.workgroup[fact.key] = value; return;
    case PassthroughFact: Check(stage == 3 && fact.key < 8, "Invalid environment fact"); env.passthrough[fact.key] = value; return;
    case TextureTypeFact:
    {
        const auto type = GuestState::ShaderTextureType(value);
        Check(fact.key <= UINT32_MAX && type.has_value(), "Invalid texture type fact");
        env.maps[0].emplace_back(fact.key, *type);
        return;
    }
    case TextureFormatFact:
    {
        const auto format = GuestState::PixelFormat(value);
        Check(fact.key <= UINT32_MAX && format.has_value(), "Invalid texture format fact");
        env.maps[1].emplace_back(fact.key, *format);
        return;
    }
    case CbufValueFact:
        Check((fact.key >> 32) < UINT32_MAX && static_cast<u32>(fact.key) != UINT32_MAX, "Invalid constant buffer fact");
        env.maps[2].emplace_back(fact.key, value);
        return;
    case CbufSizeFact:
        Check(fact.key < UINT32_MAX, "Invalid constant buffer fact");
        env.maps[2].emplace_back(fact.key << 32 | UINT32_MAX, value);
        return;
    case TextureTableFact: Check(fact.key == 0, "Invalid environment fact"); env.maps[2].emplace_back(TextureTableKey, value); return;
    case TexturePoolFact: Check(fact.key == 0, "Invalid environment fact"); env.maps[2].emplace_back(PoolTexturesKey, value); return;
    case SamplerPoolFact: Check(fact.key == 0, "Invalid environment fact"); env.maps[2].emplace_back(PoolSamplersKey, value); return;
    case CbufReplacementFact:
        Check((fact.key >> 32) < UINT32_MAX && ValidEnvironmentMapValue(3, value), "Invalid constant buffer fact");
        env.maps[3].emplace_back(fact.key, value);
        return;
    }
    Check(fact.tag >= OptionalFacts, "Shader file made by a newer version");
}
// Constant buffer values are mostly floats: 4 bytes as they are, not a varint.
bool FixedValue(u32 tag)
{
    return tag == CbufValueFact;
}
// An environment's facts, in order: the tag (the gap after the previous tag), the
// key (the gap after the previous key of the same tag, else the key), the value.
void WriteFacts(Writer & out, const std::vector<FactEntry> & facts)
{
    out.Var(facts.size());
    for (size_t i = 0; i < facts.size(); ++i)
    {
        const auto & fact = facts[i];
        const bool same = i && facts[i - 1].tag == fact.tag;
        out.Var(fact.tag - (i ? facts[i - 1].tag : 0));
        out.Var(same ? fact.key - facts[i - 1].key - 1 : fact.key);
        if (FixedValue(fact.tag))
            out.Fixed(fact.value, 4);
        else
            out.Var(fact.value);
    }
}
std::vector<FactEntry> ReadFacts(Reader & in, size_t maximum)
{
    std::vector<FactEntry> facts(in.Count(maximum));
    for (size_t i = 0; i < facts.size(); ++i)
    {
        auto & fact = facts[i];
        const auto previous = i ? facts[i - 1].tag : 0;
        const auto delta = in.Var();
        Check(delta <= UINT32_MAX - previous, "Invalid environment fact");
        fact.tag = previous + static_cast<u32>(delta);
        const auto gap = in.Var();
        if (i && facts[i - 1].tag == fact.tag)
        {
            const auto key = facts[i - 1].key;
            Check(key < UINT64_MAX && gap <= UINT64_MAX - key - 1, "Invalid environment fact");
            fact.key = key + 1 + gap;
        }
        else
            fact.key = gap;
        fact.value = FixedValue(fact.tag) ? in.Fixed(4) : in.Var();
    }
    return facts;
}
// One table of graphics or compute recipe bindings, stored once.
std::string GraphicsBinding(const JsonValue & r)
{
    Writer out;
    Check(r["cbuf_sizes"].isArray() && r["cbuf_sizes"].size() == 5 && r["texture_pool"].isArray() && r["texture_pool"].size() == 2,
          "Invalid graphics recipe arrays");
    for (const auto & stage : r["cbuf_sizes"])
    {
        Check(stage.isArray() && stage.size() == 18, "Invalid constant buffer array");
        u64 mask{};
        for (unsigned bank = 0; bank < 18; ++bank)
            if (Number(stage[bank])) mask |= u64{1} << bank;
        out.Var(mask);
        for (unsigned bank = 0; bank < 18; ++bank)
            if (const auto size = Number(stage[bank])) out.Var(size);
    }
    for (const auto & n : r["texture_pool"])
        out.Var(Number(n));
    return out.bytes;
}
std::string ComputeBinding(const JsonValue & r)
{
    Writer out;
    Check(r["cbuf_sizes"].isArray() && r["cbuf_sizes"].size() == 8, "Invalid compute recipe arrays");
    u64 mask{};
    for (unsigned bank = 0; bank < 8; ++bank)
        if (Number(r["cbuf_sizes"][bank])) mask |= u64{1} << bank;
    out.Var(mask);
    for (unsigned bank = 0; bank < 8; ++bank)
        if (const auto size = Number(r["cbuf_sizes"][bank])) out.Var(size);
    out.Var(Number(r["texture_table_size"]));
    return out.bytes;
}
// The program identity a shader key starts with: the start of its SHA-256.
std::string ProgramIdentityFromKey(const std::string & key)
{
    Check(key.size() == 64 && key.find_first_not_of("0123456789abcdef") == std::string::npos, "Invalid program key");
    std::string identity;
    const auto hex = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (size_t i = 0; i < 2 * ProgramIdentityBytes; i += 2)
        identity += static_cast<char>((hex(key[i]) << 4) | hex(key[i + 1]));
    return identity;
}
// Offsets are stored relative to the end of the previous program read from the same
// resource: programs that follow each other there store 0.
u64 ZigZag(s64 n) { return (static_cast<u64>(n) << 1) ^ static_cast<u64>(n >> 63); }
s64 UnZigZag(u64 n) { return static_cast<s64>(n >> 1) ^ -static_cast<s64>(n & 1); }
Env EnvironmentFromJson(const JsonValue & env, u32 program, u64 program_size)
{
    Allowed(env, {"shader", "local_memory_size", "texture_bound", "guest_start_address", "guest_read_lowest", "guest_read_highest",
                  "viewport_transform_state", "stage", "texture_types", "texture_pixel_formats", "cbuf_values", "cbuf_replacements",
                  "workgroup_size", "shared_memory_size", "program_header_sha256", "gp_passthrough_mask"});
    Env result;
    result.program = program;
    for (size_t field = 0; field < Fields.size(); ++field)
    {
        const auto n = Number(env[Fields[field]]);
        Check(n <= UINT32_MAX, "Compact environment bounds");
        result.fields[field] = static_cast<u32>(n);
    }
    const auto start = u64{result.fields[StartAddress]};
    Check(result.fields[6] <= 6 && result.fields[ReadLowest] == start && program_size >= 8 &&
              u64{result.fields[ReadHighest]} == start + program_size - 8,
          "Compact environment bounds");
    for (size_t m = 0; m < Maps.size(); ++m)
    {
        Check(env[Maps[m]].isArray(), "Invalid environment map");
        for (const auto & pair : env[Maps[m]])
        {
            Check(pair.isArray() && pair.size() == 2, "Invalid environment map entry");
            const auto key = Number(pair[0]), value = Number(pair[1]);
            Check((m >= 2 || key <= UINT32_MAX) && value <= UINT32_MAX && ValidEnvironmentMapValue(m, static_cast<u32>(value)),
                  "Invalid environment map entry");
            result.maps[m].emplace_back(key, static_cast<u32>(value));
        }
        std::sort(result.maps[m].begin(), result.maps[m].end());
        Check(std::adjacent_find(result.maps[m].begin(), result.maps[m].end(),
                                 [](const auto & a, const auto & b) { return a.first == b.first; }) == result.maps[m].end(),
              "Duplicate environment map key");
    }
    if (result.fields[6] == 5)
    {
        Check(env["workgroup_size"].isArray() && env["workgroup_size"].size() == 3, "Invalid compute dimensions");
        for (unsigned i = 0; i < 3; ++i)
            result.workgroup[i] = static_cast<u32>(Number(env["workgroup_size"][i]));
        result.shared = static_cast<u32>(Number(env["shared_memory_size"]));
    }
    else if (result.fields[6] == 3)
    {
        Check(env["gp_passthrough_mask"].isArray() && env["gp_passthrough_mask"].size() == 8, "Invalid geometry mask");
        for (unsigned i = 0; i < 8; ++i)
            result.passthrough[i] = static_cast<u32>(Number(env["gp_passthrough_mask"][i]));
    }
    return result;
}
} // namespace

std::string PackCompact(const JsonValue & profile, int compression_level)
{
    Allowed(profile, {"format", "version", "title_id", "guest_state_version", "content_label", "revision", "note", "full_game", "versions",
                      "shaders", "environments", "recipes", "unresolved_recipes", "source_files", "source_context"});
    Check(profile["shaders"].isObject() && profile["shaders"].size() <= ProfileLimits{}.programs && profile["environments"].isObject() &&
              profile["environments"].size() <= ProfileLimits{}.environments && profile["recipes"].isArray() &&
              profile["recipes"].size() <= ProfileLimits{}.recipes,
          "Compact table limit");
    const auto title = profile["title_id"].asString();
    Check(title.size() == 16 && title.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos, "Compact title syntax");
    const auto text = [&](const char * field) {
        Check(profile[field].isNull() || profile[field].isString(), "Invalid profile description");
        return profile[field].isNull() ? std::string{} : profile[field].asString();
    };
    const auto label = text("content_label"), revision = text("revision"), note = text("note");
    Check(ValidProfileText(label, 4096) && ValidProfileText(revision, MaxRevision) && ValidProfileText(note, MaxNote),
          "Invalid profile description");
    Check(Number(profile["guest_state_version"]) == 1, "Compact guest state version");
    Check(profile["version"].asUInt64() == 2 && profile["format"].asString() == "nxshader", "Compact encoder requires v2 metadata");
    Check(profile["full_game"].isNull() || profile["full_game"].isBool(), "Invalid profile flags");
    std::vector<std::pair<u32, std::string>> sections;
    {
        Writer meta;
        meta.Fixed(std::stoull(title, nullptr, 16), 8);
        meta.Text(label);
        meta.Text(revision);
        meta.Text(note);
        // Bit 0: the recordings cover the whole game.
        meta.Var(profile["full_game"].asBool() ? 1 : 0);
        sections.emplace_back(MetaSection, std::move(meta.bytes));
    }
    std::set<std::string> source_names;
    {
        Writer files;
        Check((profile["source_files"].isNull() || profile["source_files"].isArray()) && profile["source_files"].size() <= 1024,
              "Invalid source file table");
        files.Var(profile["source_files"].size());
        for (const auto & file : profile["source_files"])
        {
            Allowed(file, {"name", "size", "header"});
            source_names.insert(file["name"].asString());
            files.Text(file["name"].asString());
            files.Var(Number(file["size"]));
            files.Hash(file["header"].asString());
        }
        sections.emplace_back(SourceSection, std::move(files.bytes));
    }
    const auto recipe_contexts = [&](const JsonValue & r) {
        std::vector<std::string> result;
        if (!r["source_contexts"].isNull())
        {
            Check(r["source_context"].isNull() && r["source_contexts"].isArray(), "Invalid recipe contexts");
            for (const auto & context : r["source_contexts"])
                if (std::find(result.begin(), result.end(), context.asString()) == result.end()) result.push_back(context.asString());
        }
        else if (const auto context = r["source_context"].isNull() ? profile["source_context"].asString() : r["source_context"].asString();
                 !context.empty())
            result.push_back(context);
        Check(std::find(result.begin(), result.end(), std::string{}) == result.end(), "Invalid recipe contexts");
        return result;
    };
    std::map<std::string, size_t> context_ids;
    std::vector<std::string> contexts;
    const auto intern = [&](const std::string & context) {
        if (!context.empty() && context_ids.emplace(context, contexts.size() + 1).second) contexts.push_back(context);
    };
    intern(profile["source_context"].asString());
    for (const auto & r : profile["recipes"])
        for (const auto & context : recipe_contexts(r))
            intern(context);
    Check(contexts.size() <= 65536, "Compact context limit");
    const auto context_index = [&](const std::string & context) { return context.empty() ? 0 : context_ids.at(context); };
    {
        Writer table;
        Check(profile["versions"].isNull() || profile["versions"].isObject(), "Invalid profile versions");
        table.Var(contexts.size());
        for (const auto & context : contexts)
        {
            table.Hash(context);
            // Base and update version numbers, for people only.
            const auto & version = profile["versions"][context];
            for (const auto field : {"base", "update"})
            {
                const auto value = version[field].isString() ? version[field].asString() : std::string{};
                Check(ValidProfileText(value, MaxVersionText), "Invalid profile version");
                table.Text(value);
            }
        }
        table.Var(context_index(profile["source_context"].asString()));
        sections.emplace_back(ContextSection, std::move(table.bytes));
    }
    // Programs: one per distinct SHA-256, only those a recipe uses.
    std::map<std::string, std::string> program_of_key; // shader key -> identity
    std::set<std::string> used_environments;
    for (const auto & r : profile["recipes"])
    {
        Check(r["environments"].isArray() && !r["environments"].empty() && r["environments"].size() <= 6, "Invalid recipe environments");
        for (const auto & id : r["environments"])
        {
            const auto & env = profile["environments"][id.asString()];
            Check(env.isObject() && env["shader"].isString(), "Unknown recipe environment");
            const auto key = env["shader"].asString();
            Check(profile["shaders"][key].isObject(), "Unknown recipe program");
            program_of_key.emplace(key, ProgramIdentityFromKey(key));
            used_environments.insert(id.asString());
        }
    }
    std::vector<std::string> keys;
    for (const auto & [key, identity] : program_of_key)
        keys.push_back(key);
    std::map<std::string, std::string> locator_keys;
    for (const auto & key : keys)
        locator_keys.emplace(key, Canonical(profile["shaders"][key]["references"]));
    std::sort(keys.begin(), keys.end(), [&](const auto & a, const auto & b) { return std::tie(locator_keys.at(a), a) < std::tie(locator_keys.at(b), b); });
    std::vector<JsonValue> resources;
    std::map<std::string, size_t> resource_ids;
    struct Ref
    {
        std::string identity;
        u64 size{};
        std::vector<std::pair<size_t, u64>> locators; // resource, offset
    };
    std::vector<Ref> programs;
    std::map<std::string, size_t> program_ids; // identity -> index
    for (const auto & key : keys)
    {
        const auto & shader = profile["shaders"][key];
        Allowed(shader, {"size", "references"});
        Check(shader["references"].isArray() && shader["references"].size() >= 1 && shader["references"].size() <= MaxProgramLocators,
              "Compact program locator count");
        const auto & identity = program_of_key.at(key);
        const auto size = Number(shader["size"]);
        Check(size && size % 8 == 0 && size <= 1024 * 1024, "Compact program bounds");
        auto [found, inserted] = program_ids.emplace(identity, programs.size());
        if (inserted) programs.push_back({identity, size, {}});
        auto & ref = programs[found->second];
        Check(ref.size == size, "Two programs share one SHA-256");
        for (unsigned i = 0; i < shader["references"].size(); ++i)
        {
            auto descriptor = shader["references"][i];
            Allowed(descriptor, {"encoding", "file", "offset", "size", "container_offset", "container_size", "decoded_size", "container_sha256",
                                 "segment_sha256", "physical_spans", "resource_size", "resource_sha256", "dictionary"});
            const auto offset = Number(descriptor["offset"]);
            Check(Number(descriptor["size"]) == size, "Compact locator size");
            descriptor.removeMember("offset");
            descriptor.removeMember("size");
            // A dictionary's resource is listed before the resources that use it.
            if (descriptor.isMember("dictionary"))
            {
                auto dictionary = DictionaryResource(descriptor["dictionary"]);
                auto dictionary_key = Canonical(dictionary);
                if (resource_ids.emplace(std::move(dictionary_key), resources.size()).second) resources.push_back(std::move(dictionary));
            }
            const auto resource_key = Canonical(descriptor);
            auto [it, fresh] = resource_ids.emplace(resource_key, resources.size());
            if (fresh) resources.push_back(std::move(descriptor));
            const std::pair locator{it->second, offset};
            // A program both game versions contain keeps one locator per version.
            if (std::find(ref.locators.begin(), ref.locators.end(), locator) == ref.locators.end() && ref.locators.size() < MaxProgramLocators)
                ref.locators.push_back(locator);
        }
    }
    {
        Writer table;
        table.Var(resources.size());
        for (const auto & resource : resources)
        {
            const auto encoding = resource["encoding"].asString();
            const auto it = std::find(Encodings.begin(), Encodings.end(), encoding);
            Check(it != Encodings.end(), "Unknown compact resource encoding");
            const auto kind = static_cast<u64>(it - Encodings.begin());
            table.Var(kind);
            Check(Spanned(kind) || source_names.contains(resource["file"].asString()), "Resource outside the game contents");
            for (const auto & span : resource["physical_spans"])
                Check(source_names.contains(span["file"].asString()), "Resource outside the game contents");
            if (!Spanned(kind)) table.Text(resource["file"].asString());
            if (kind == 1 || kind == 2 || kind == 5)
            {
                table.Var(Number(resource["container_offset"]));
                table.Var(Number(resource["container_size"]));
            }
            if (kind == 2)
            {
                table.Var(Number(resource["decoded_size"]));
                table.Hash(resource["container_sha256"].asString());
                table.Hash(resource["segment_sha256"].asString());
            }
            if (Spanned(kind))
            {
                table.Var(Number(resource["resource_size"]));
                table.Hash(resource["resource_sha256"].asString());
                table.Var(resource["physical_spans"].size());
                for (const auto & span : resource["physical_spans"])
                {
                    table.Text(span["file"].asString());
                    table.Var(Number(span["offset"]));
                    table.Var(Number(span["size"]));
                }
            }
            if (Zstd(kind))
            {
                table.Var(Number(resource["decoded_size"]));
                if (!resource.isMember("dictionary"))
                {
                    table.Var(0);
                    continue;
                }
                const auto & dictionary = resource["dictionary"];
                table.Var(resource_ids.at(Canonical(DictionaryResource(dictionary))) + 1);
                table.Var(Number(dictionary["offset"]));
                table.Var(Number(dictionary["size"]));
                table.Hash(dictionary["sha256"].asString());
            }
        }
        sections.emplace_back(ResourceSection, std::move(table.bytes));
    }
    // Programs in the order they are found in the game, so their offsets mostly store 0.
    {
        std::vector<size_t> order(programs.size());
        std::iota(order.begin(), order.end(), size_t{});
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return programs[a].locators.front() < programs[b].locators.front(); });
        std::vector<Ref> sorted;
        for (const auto i : order)
            sorted.push_back(std::move(programs[i]));
        programs = std::move(sorted);
        for (size_t i = 0; i < programs.size(); ++i)
            program_ids[programs[i].identity] = i;
    }
    {
        Writer table;
        table.Var(programs.size());
        std::map<size_t, u64> ends;
        for (const auto & p : programs)
        {
            table.Var(p.size / 8);
            table.bytes += p.identity;
            table.Var(p.locators.size());
            for (const auto & [resource, offset] : p.locators)
            {
                table.Var(resource);
                auto & end = ends[resource];
                table.Var(ZigZag(static_cast<s64>(offset - end)));
                end = offset + p.size;
            }
        }
        sections.emplace_back(ProgramSection, std::move(table.bytes));
    }
    // Environments: program, portable stage, then their facts; identical ones once.
    // Sorted by program: environments of one program differ little, which zstd sees.
    std::map<std::string, size_t> env_ids;
    {
        struct Encoded
        {
            u32 program{}, stage{};
            std::vector<FactEntry> facts;
            auto operator<=>(const Encoded &) const = default;
        };
        std::vector<std::pair<Encoded, std::string>> encoded; // environment, id
        for (const auto & id : used_environments)
        {
            const auto & json = profile["environments"][id];
            const auto program = program_ids.at(program_of_key.at(json["shader"].asString()));
            const auto env = EnvironmentFromJson(json, static_cast<u32>(program), programs[program].size);
            Check(env.fields[6] == 5 || programs[program].size >= 80, "Compact graphics header size");
            encoded.emplace_back(Encoded{static_cast<u32>(program), PortableStage[env.fields[6]], Facts(env)}, id);
        }
        std::sort(encoded.begin(), encoded.end());
        std::vector<const Encoded *> unique;
        for (const auto & [env, id] : encoded)
        {
            if (unique.empty() || *unique.back() != env) unique.push_back(&env);
            env_ids[id] = unique.size() - 1;
        }
        Writer table;
        table.Var(unique.size());
        u32 previous{};
        for (const auto * env : unique)
        {
            // Sorted by program: the gap after the previous environment's program.
            table.Var(env->program - previous);
            previous = env->program;
            table.Var(env->stage);
            WriteFacts(table, env->facts);
        }
        sections.emplace_back(EnvironmentSection, std::move(table.bytes));
    }
    // Guest states as Maxwell 3D registers: the most common value of each register
    // once, then each state's differences from it.
    std::map<std::string, size_t> state_ids;
    {
        std::vector<GuestState::Registers> states;
        for (const auto & r : profile["recipes"])
        {
            if (r["kind"].asString() != "graphics") continue;
            Check(r["guest_words"].isArray(), "Compact guest state ABI");
            const auto key = Canonical(r["guest_words"]);
            if (state_ids.contains(key)) continue;
            std::vector<u32> words;
            for (const auto & n : r["guest_words"])
            {
                Check(Number(n) <= UINT32_MAX, "State word overflow");
                words.push_back(static_cast<u32>(Number(n)));
            }
            auto registers = GuestState::FromWords(words);
            Check(registers.has_value(), "Guest state without Maxwell registers");
            state_ids[key] = states.size();
            states.push_back(std::move(*registers));
        }
        std::map<u32, std::map<u32, size_t>> counts; // register -> value -> states
        for (const auto & state : states)
            for (const auto & [key, value] : state)
                ++counts[key][value];
        GuestState::Registers base;
        for (const auto & [key, values] : counts)
        {
            size_t present{}, best{};
            u32 value{};
            for (const auto & [v, n] : values)
            {
                present += n;
                if (n > best) { best = n; value = v; }
            }
            // A register a state leaves out is 0, which is then the most common value.
            if (best > states.size() - present) base.emplace_back(key, value);
        }
        Writer table;
        const auto write = [&](const GuestState::Registers & registers) {
            table.Var(registers.size());
            u32 previous{};
            for (size_t i = 0; i < registers.size(); ++i)
            {
                table.Var(i ? registers[i].first - previous - 1 : registers[i].first);
                table.Var(registers[i].second);
                previous = registers[i].first;
            }
        };
        write(base);
        table.Var(states.size());
        for (const auto & state : states)
        {
            // Registers where the state differs from the base: its value, 0 included.
            GuestState::Registers patch;
            size_t a{}, b{};
            while (a < state.size() || b < base.size())
            {
                if (b == base.size() || (a < state.size() && state[a].first < base[b].first))
                    patch.push_back(state[a++]);
                else if (a == state.size() || base[b].first < state[a].first)
                    patch.emplace_back(base[b++].first, 0);
                else
                {
                    if (state[a].second != base[b].second) patch.push_back(state[a]);
                    ++a;
                    ++b;
                }
            }
            write(patch);
        }
        sections.emplace_back(StateSection, std::move(table.bytes));
    }
    // Recipe bindings: constant buffer sizes and pool sizes, each distinct table once.
    std::map<std::string, size_t> graphics_bindings, compute_bindings;
    for (const auto & r : profile["recipes"])
    {
        auto & table = r["kind"].asString() == "compute" ? compute_bindings : graphics_bindings;
        table.emplace(r["kind"].asString() == "compute" ? ComputeBinding(r) : GraphicsBinding(r), table.size());
    }
    {
        Writer table;
        for (const auto * bindings : {&graphics_bindings, &compute_bindings})
        {
            std::vector<const std::string *> ordered(bindings->size());
            for (const auto & [bytes, index] : *bindings)
                ordered[index] = &bytes;
            table.Var(ordered.size());
            for (const auto * bytes : ordered)
                table.bytes += *bytes;
        }
        sections.emplace_back(BindingSection, std::move(table.bytes));
    }
    {
        // One entry per distinct recipe, at its first position, with the game versions
        // of every copy: synthetic only if every copy is, played if any copy was.
        struct Entry
        {
            bool compute{};
            size_t state{}, binding{};
            std::vector<std::pair<u32, size_t>> envs; // portable stage, environment
            std::vector<size_t> contexts;
            bool synthetic{}, played{};
        };
        std::map<std::string, size_t> seen;
        std::vector<Entry> table;
        for (const auto & r : profile["recipes"])
        {
            Allowed(r, {"kind", "guest_words", "shader_hashes", "cbuf_sizes", "texture_pool", "environments", "shader_hash", "shared_memory_size",
                        "workgroup_size", "texture_table_size", "source_context", "source_contexts", "synthetic", "played"});
            Check((r["synthetic"].isNull() || r["synthetic"].isBool()) && (r["played"].isNull() || r["played"].isBool()), "Invalid recipe origin");
            const auto kind = r["kind"].asString();
            Check(kind == "compute" || kind == "graphics", "Invalid recipe kind");
            const bool compute = kind == "compute";
            Writer body;
            body.Var(compute ? 1 : 0);
            Entry recipe{compute};
            u32 previous_stage{};
            for (unsigned i = 0; i < r["environments"].size(); ++i)
            {
                const auto id = r["environments"][i].asString();
                const auto stage = PortableStage.at(static_cast<size_t>(Number(profile["environments"][id]["stage"])));
                Check(compute ? (r["environments"].size() == 1 && stage == PortableCompute)
                              : (stage < PortableCompute && (i == 0 || stage > previous_stage)),
                      "Recipe stage order mismatch");
                previous_stage = stage;
                recipe.envs.emplace_back(stage, env_ids.at(id));
            }
            if (compute)
            {
                recipe.binding = compute_bindings.at(ComputeBinding(r));
                const auto & env = profile["environments"][r["environments"][0].asString()];
                Check(Number(r["shared_memory_size"]) == Number(env["shared_memory_size"]) &&
                          Canonical(r["workgroup_size"]) == Canonical(env["workgroup_size"]),
                      "Compute recipe/environment dimensions mismatch");
            }
            else
            {
                Check(r["shader_hashes"].isArray() && r["shader_hashes"].size() == 6, "Invalid graphics recipe arrays");
                size_t active{};
                for (const auto & n : r["shader_hashes"])
                    active += Number(n) != 0;
                Check(active == recipe.envs.size(), "Recipe program count mismatch");
                recipe.state = state_ids.at(Canonical(r["guest_words"]));
                recipe.binding = graphics_bindings.at(GraphicsBinding(r));
            }
            body.Var(recipe.state);
            body.Var(recipe.binding);
            for (const auto & [stage, id] : recipe.envs)
            {
                body.Var(stage);
                body.Var(id);
            }
            const bool synthetic = r["synthetic"].asBool(), played = r["played"].asBool();
            const auto [found, inserted] = seen.emplace(body.bytes, table.size());
            if (inserted)
            {
                recipe.synthetic = synthetic;
                recipe.played = played;
                table.push_back(std::move(recipe));
            }
            auto & entry = table[found->second];
            entry.synthetic = entry.synthetic && synthetic;
            entry.played = entry.played || played;
            for (const auto & context : recipe_contexts(r))
                if (const auto index = context_index(context); std::find(entry.contexts.begin(), entry.contexts.end(), index) == entry.contexts.end())
                    entry.contexts.push_back(index);
        }
        Writer out;
        out.Var(table.size());
        for (const auto & entry : table)
        {
            // Origin: bit 0 synthetic, bit 1 played. Recorded recipes are seen in play by definition.
            out.Var((entry.synthetic ? 1 : 0) | (entry.synthetic && entry.played ? 2 : 0));
            out.Var(entry.contexts.size());
            for (const auto index : entry.contexts)
                out.Var(index);
            out.Var(entry.compute ? 1 : 0);
            if (!entry.compute)
            {
                out.Var(entry.state);
                // Stages present: bit n for portable stage n.
                u64 mask{};
                for (const auto & [stage, id] : entry.envs)
                    mask |= u64{1} << stage;
                out.Var(mask);
            }
            out.Var(entry.binding);
            for (const auto & [stage, id] : entry.envs)
            {
                out.Var(id);
            }
        }
        sections.emplace_back(RecipeSection, std::move(out.bytes));
    }
    Writer payload;
    payload.Var(sections.size());
    for (const auto & [tag, body] : sections)
    {
        payload.Fixed(tag, 4);
        payload.Var(SectionRequired);
        payload.Var(body.size());
    }
    for (const auto & [tag, body] : sections)
        payload.bytes += body;
    Check(payload.bytes.size() <= MaxProfilePayloadBytes, "Compact profile size limit");
    return Pack(payload.bytes, compression_level);
}

namespace
{
// One NCA of the game versions the profile was exported from.
struct SourceFile
{
    std::string name;
    u64 size{};
    std::string header; // SHA-256 of its first 4 KiB.
};
// A header is only a lookup hint. Decryption tools can rewrite it while keeping
// shader resources at the same offsets. Probe small, fingerprinted resources in
// same-sized NCAs, then retain the normal verification of every loaded shader.
struct ContentProbe
{
    std::vector<Span> spans;
    std::string hash;
    const Program * program{}; // Null for a resource fingerprint.
    u64 size{};
};
using ContentProbes = std::map<std::string, std::vector<ContentProbe>>;

ContentProbes MakeContentProbes(const std::vector<Resource>& resources,
                               const std::vector<Program>& programs,
                               const std::vector<size_t>& needed_programs)
{
    constexpr u64 MaxProbeBytes = 4 * 1024 * 1024;
    ContentProbes probes;
    std::set<size_t> seen;
    for (const auto id : needed_programs) {
        const auto& program = programs[id];
        for (const auto& locator : program.locators) {
            const auto& resource = resources[locator.resource];
            if (resource.kind == 0) {
                if (program.size <= MaxProbeBytes)
                    probes[resource.file].push_back({{{resource.file, locator.offset, program.size}},
                                                     {}, &program, program.size});
            } else if (!resource.hash.empty() && resource.size <= MaxProbeBytes &&
                       seen.insert(locator.resource).second) {
                auto spans = Spanned(resource.kind) ? resource.spans :
                    std::vector<Span>{{resource.file, resource.offset, resource.size}};
                if (spans.empty()) continue;
                const auto file = spans.front().file;
                if (std::all_of(spans.begin(), spans.end(), [&](const auto& span) { return span.file == file; }))
                    probes[file].push_back({std::move(spans), resource.hash, nullptr, resource.size});
            }
        }
    }
    for (auto& [file, choices] : probes) {
        std::stable_sort(choices.begin(), choices.end(), [](const auto& a, const auto& b) { return a.size < b.size; });
        if (choices.size() > 4) choices.resize(4);
    }
    return probes;
}

bool MatchesContentProbe(const ContentLocation& content, const ContentProbe& probe,
                         Timing::Metrics* timings)
{
    ContentInput input;
    input.Open(content);
    std::string bytes(static_cast<size_t>(probe.size), '\0');
    size_t destination{};
    for (const auto& span : probe.spans) {
        if (span.offset > content.size || span.size > content.size - span.offset) return false;
        ReadInto(input, content.offset + span.offset,
                 std::span(bytes).subspan(destination, span.size), timings);
        destination += span.size;
    }
    return probe.hash.empty() ? Verify(bytes, *probe.program, timings) : Hash(bytes, timings) == probe.hash;
}

// Finds the needed NCAs in any NSP or XCI directly inside root. Size and the hash
// of the first 4 KiB identify an NCA; every program read from it is still verified.
ContentMap LocateContents(const std::filesystem::path & root, u64 title_id, const std::vector<SourceFile> & sources,
                          const std::set<std::string> & needed, Timing::Metrics * timings, const ResourceProgress & progress,
                          const ContentProbes& probes)
{
    ContentMap found;
    std::map<std::string, std::vector<ContentLocation>> alternatives;
    std::vector<const SourceFile *> pending;
    for (const auto & source : sources)
        if (needed.contains(source.name)) pending.push_back(&source);
    if (pending.empty()) return found;
    std::vector<std::filesystem::path> containers;
    size_t entries{};
    std::error_code error;
    for (auto it = std::filesystem::directory_iterator(root, error); !error && it != std::filesystem::directory_iterator{}; it.increment(error))
    {
        Check(++entries <= 4096, "Local resource directory limit");
        std::error_code ignored;
        const auto name = it->path().filename().u8string();
        const std::string utf8(name.begin(), name.end());
        if (it->is_regular_file(ignored) && (IsGameContainerName(utf8) || IsNxName(utf8)))
            containers.push_back(it->path());
    }
    // A shared game folder holds many titles: dumps named after this one first.
    char title[17];
    std::snprintf(title, sizeof(title), "%016llx", static_cast<unsigned long long>(title_id));
    const auto named = [&](const std::filesystem::path & path) {
        const auto utf8 = path.filename().u8string();
        std::string name(utf8.begin(), utf8.end());
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return c < 128 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c); });
        return name.find(std::string_view(title, 13)) != std::string::npos;
    };
    std::sort(containers.begin(), containers.end(), [&](const auto & a, const auto & b) {
        return std::make_pair(!named(a), a) < std::make_pair(!named(b), b);
    });
    for (const auto & path : containers)
    {
        if (pending.empty()) break;
        Check(!progress || progress(0, 1), "Shader profile operation cancelled");
        try
        {
            Timing::Timer timer(timings, Timing::Phase::ResourceIO);
            const auto utf8 = path.filename().u8string();
            if (IsNxName(std::string(utf8.begin(), utf8.end())))
            {
                // Every snapshot: a profile may name the NCAs of any version kept.
                auto game = std::make_shared<NxGame>(path);
                for (const auto & nca : game->AllContents())
                {
                    std::string header;
                    for (auto it = pending.begin(); it != pending.end();)
                    {
                        if ((*it)->size != nca.size) { ++it; continue; }
                        if (header.empty())
                        {
                            std::string bytes(static_cast<size_t>(std::min<u64>(nca.size, 4096)), '\0');
                            game->Read(nca.file, 0, std::span(bytes));
                            header = Hash(bytes, timings);
                        }
                        if (header != (*it)->header) { ++it; continue; }
                        found[(*it)->name] = {path / std::u8string(nca.name.begin(), nca.name.end()), 0, nca.size, game, nca.file};
                        it = pending.erase(it);
                    }
                }
                continue;
            }
            const auto length = std::filesystem::file_size(path);
            std::ifstream in(path, std::ios::binary);
            const auto read = [&](u64 off, u64 size) {
                Check(off <= length && size <= length - off && size <= 64 * 1024 * 1024, "Game container range");
                std::string bytes(static_cast<size_t>(size), '\0');
                in.clear();
                in.seekg(static_cast<std::streamoff>(off));
                in.read(bytes.data(), size);
                Check(bool(in), "Cannot read game container");
                return bytes;
            };
            for (const auto & entry : ContainerEntries(length, read))
            {
                std::string header;
                for (auto it = pending.begin(); it != pending.end();)
                {
                    if ((*it)->size != entry.size) { ++it; continue; }
                    if (header.empty()) header = Hash(read(entry.offset, std::min<u64>(entry.size, 4096)), timings);
                    if (header != (*it)->header) {
                        alternatives[(*it)->name].push_back({path, entry.offset, entry.size});
                        ++it;
                        continue;
                    }
                    found[(*it)->name] = {path, entry.offset, entry.size};
                    it = pending.erase(it);
                }
            }
        }
        catch (const std::exception &) { /* Not a readable plaintext container. */ }
    }
    for (const auto* source : pending) {
        const auto choices = probes.find(source->name);
        if (choices == probes.end()) continue;
        for (const auto& candidate : alternatives[source->name]) {
            Check(!progress || progress(0, 1), "Shader profile operation cancelled");
            bool matched{};
            for (const auto& probe : choices->second) {
                try { matched = MatchesContentProbe(candidate, probe, timings); }
                catch (const std::runtime_error&) { matched = false; }
                if (matched) break;
            }
            if (matched) {
                found[source->name] = candidate;
                break;
            }
        }
    }
    return found;
}
struct Pending
{
    Recipe recipe;
    std::vector<u32> ids;
    std::string content; // identity inputs after the context
};
// Same for a recipe whichever profile carries it, given the version it is used as.
std::string RecipeIdentity(const std::string & context, const std::string & content)
{
    Writer identity;
    identity.Text("nxshader-recipe-v3"); // Fixed: recipes already imported keep their identity.
    identity.Text(context);
    identity.bytes += content;
    return ResourceSha256(identity.bytes);
}
// Every table of a profile, checked and cross-referenced, before any local file is read.
struct Parsed
{
    std::string label, revision, note, source_context;
    bool full_game{};
    std::vector<GameVersion> versions; // One per context, in table order.
    std::vector<SourceFile> source_files;
    std::vector<Resource> resources;
    std::vector<Program> programs;
    std::vector<Env> envs;
    std::vector<Pending> pending;
    size_t unresolved{};
};
std::vector<SourceFile> ReadSourceFiles(Reader & in, std::set<std::string> & names)
{
    std::vector<SourceFile> files;
    const auto count = in.Count(1024);
    for (size_t i = 0; i < count; ++i)
    {
        SourceFile file{in.Text(), in.Var(), in.Hash()};
        Check(ValidResourceName(file.name) && names.insert(file.name).second, "Invalid source file name");
        files.push_back(std::move(file));
    }
    return files;
}
std::vector<Resource> ReadResources(Reader & in, const std::set<std::string> & source_names)
{
    const auto resource_count = in.Count(65536);
    std::vector<Resource> resources;
    resources.reserve(resource_count);
    size_t total_spans{};
    for (size_t i = 0; i < resource_count; ++i)
    {
        Resource r;
        r.kind = in.Word();
        Check(r.kind < Encodings.size(), "Compact resource kind");
        if (!Spanned(r.kind))
        {
            r.file = in.Text();
            Check(source_names.contains(r.file), "Compact resource outside the game contents");
        }
        if (r.kind == 1 || r.kind == 2 || r.kind == 5)
        {
            r.offset = in.Var();
            r.size = in.Var();
        }
        if (r.kind == 2)
        {
            r.decoded_size = in.Var();
            r.hash = in.Hash();
            r.segment_hash = in.Hash();
        }
        if (Spanned(r.kind))
        {
            r.size = in.Var();
            r.hash = in.Hash();
            const auto count = in.Count(65536);
            total_spans += count;
            Check(total_spans <= 1000000, "Compact span budget");
            for (size_t n = 0; n < count; ++n)
                r.spans.push_back({in.Text(), in.Var(), in.Var()});
            u64 span_bytes{};
            for (const auto & span : r.spans)
            {
                Check(source_names.contains(span.file) && span.size <= r.size - span_bytes && span.offset <= UINT64_MAX - span.size,
                      "Invalid compact span");
                span_bytes += span.size;
            }
            Check(span_bytes == r.size, "Incomplete compact spans");
        }
        if (Zstd(r.kind))
        {
            r.decoded_size = in.Var();
            r.dictionary = in.Var();
            if (r.dictionary)
            {
                // Dictionaries come first and have none of their own.
                Check(r.dictionary <= i, "Compact dictionary resource");
                const auto & source = resources[r.dictionary - 1];
                Check(Zstd(source.kind) && !source.dictionary, "Compact dictionary resource");
                r.dictionary_offset = in.Var();
                r.dictionary_size = in.Var();
                r.dictionary_hash = in.Hash();
                Check(r.dictionary_size && r.dictionary_size <= MaxZstdDictionary && r.dictionary_offset <= source.decoded_size &&
                          r.dictionary_size <= source.decoded_size - r.dictionary_offset,
                      "Compact dictionary bounds");
            }
        }
        Check(r.size <= MaxDecodedResource && r.decoded_size <= MaxDecodedResource, "Compact decoded limit");
        Check(r.offset <= UINT64_MAX - r.size, "Compact resource offset overflow");
        resources.push_back(std::move(r));
    }
    return resources;
}
void CheckLocator(const std::vector<Resource> & resources, const Program & p, const Locator & l)
{
    Check(l.resource < resources.size(), "Compact program bounds");
    for (const auto & other : p.locators)
        Check(other.resource != l.resource || other.offset != l.offset, "Duplicate compact locator");
    const auto & resource = resources[l.resource];
    Check(l.offset <= UINT64_MAX - p.size, "Compact program offset overflow");
    if (resource.kind != 0)
        Check(l.offset <= MaxDecodedResource && p.size <= MaxDecodedResource - l.offset, "Compact program resource bounds");
    if (resource.kind == 2 || resource.kind == 3 || Zstd(resource.kind))
    {
        const auto limit = resource.kind == 3 ? resource.size : resource.decoded_size;
        Check(l.offset <= limit && p.size <= limit - l.offset, "Compact program resource range");
    }
}
// The read range always starts at the program and covers it: derived, not stored.
void CheckEnvironment(Env & env, const std::vector<Program> & programs)
{
    Check(env.program < programs.size(), "Compact environment program");
    const auto start = u64{env.fields[StartAddress]}, size = programs[env.program].size;
    Check(size >= 8 && start + size - 8 <= UINT32_MAX, "Compact environment bounds");
    env.fields[ReadLowest] = env.fields[StartAddress];
    env.fields[ReadHighest] = static_cast<u32>(start + size - 8);
    Check(env.fields[6] <= 6, "Compact stage");
    Check(env.fields[6] == 5 || size >= 80, "Compact graphics header size");
}
// Recipe sizes the environments also answer must agree with them.
void CheckRecipeSizes(const Recipe & r, const std::vector<Env> & envs, const std::vector<u32> & ids)
{
    if (r.compute)
    {
        const auto & env = envs[ids.front()];
        Check(env.workgroup == r.workgroup_size && env.shared == r.shared_memory_size, "Compute recipe/environment dimensions mismatch");
    }
    for (const auto id : ids)
    {
        const auto & env = envs[id];
        const auto slot = r.compute ? 0 : GraphicsSlot(env.fields[6]);
        for (const auto & [key, value] : env.maps[2])
        {
            const auto bank = key >> 32;
            if (static_cast<u32>(key) == UINT32_MAX && bank < (r.compute ? 8U : 18U))
            {
                const auto size = r.compute ? r.compute_cbuf_sizes[bank] : r.cbuf_sizes[slot <= 1 ? 0 : slot - 1][bank];
                Check(value == size, "Recipe constant buffer size mismatch");
            }
            if (r.compute && key == TextureTableKey) Check(value == r.texture_table_size, "Recipe texture table size mismatch");
            if (!r.compute && key == PoolTexturesKey) Check(value == r.texture_pool[0], "Recipe texture pool mismatch");
            if (!r.compute && key == PoolSamplersKey) Check(value == r.texture_pool[1], "Recipe sampler pool mismatch");
        }
    }
}
// Identity holds semantic compilation inputs only, never table indices, physical
// locators, compression, labels or the community revision's digest.
void Identify(Parsed & parsed)
{
    std::vector<std::string> environment_identities;
    environment_identities.reserve(parsed.envs.size());
    for (const auto & env : parsed.envs)
    {
        Writer identity;
        const auto words = [&](const auto & values) {
            for (auto value : values)
                identity.Fixed(value, 4);
        };
        const auto & program = parsed.programs[env.program];
        identity.bytes += program.sha;
        identity.Fixed(program.size, 8);
        words(env.fields);
        for (auto map : env.maps)
        {
            std::sort(map.begin(), map.end());
            identity.Var(map.size());
            for (const auto & [key, value] : map)
            {
                identity.Fixed(key, 8);
                identity.Fixed(value, 4);
            }
        }
        words(env.workgroup);
        identity.Fixed(env.shared, 4);
        words(env.passthrough);
        environment_identities.push_back(std::move(identity.bytes));
    }
    for (auto & p : parsed.pending)
    {
        Writer identity;
        const auto words = [&](const auto & values) {
            for (auto value : values)
                identity.Fixed(value, 4);
        };
        identity.Var(p.recipe.compute);
        words(p.recipe.guest_words);
        for (const auto & sizes : p.recipe.cbuf_sizes)
            words(sizes);
        words(p.recipe.texture_pool);
        identity.Fixed(p.recipe.shared_memory_size, 4);
        words(p.recipe.workgroup_size);
        words(p.recipe.compute_cbuf_sizes);
        identity.Fixed(p.recipe.texture_table_size, 4);
        for (auto id : p.ids)
            identity.bytes += environment_identities[id];
        p.content = std::move(identity.bytes);
        p.recipe.identity = RecipeIdentity(p.recipe.source_context, p.content);
    }
}
// A directory of tagged sections, then their bodies.
Parsed Parse(std::string_view payload, u64 title_id)
{
    Parsed parsed;
    Reader in{payload};
    std::map<u32, std::string_view> sections;
    {
        const auto count = in.Count(64);
        std::vector<std::tuple<u32, bool, u64>> directory;
        std::set<u32> tags;
        for (size_t i = 0; i < count; ++i)
        {
            const auto tag = static_cast<u32>(in.Fixed(4));
            const auto flags = in.Var();
            const auto size = in.Var();
            Check(flags <= SectionRequired && tags.insert(tag).second, "Invalid shader file section");
            directory.emplace_back(tag, flags & SectionRequired, size);
        }
        for (const auto & [tag, required, size] : directory)
        {
            Check(size <= payload.size() - in.pos, "Truncated shader file section");
            const auto body = in.Take(static_cast<size_t>(size));
            if (std::find(RequiredSections.begin(), RequiredSections.end(), tag) != RequiredSections.end())
                sections.emplace(tag, body);
            else
                // A section this version does not know: skipped, unless it changes how the file is read.
                Check(!required, "Shader file made by a newer version");
        }
        Check(in.pos == payload.size(), "Trailing compact bytes");
        for (const auto tag : RequiredSections)
            Check(sections.contains(tag), "Missing shader file section");
    }
    const auto section = [&](u32 tag, const auto & read) {
        Reader body{sections.at(tag)};
        read(body);
        Check(body.pos == body.data.size(), "Trailing section bytes");
    };
    section(MetaSection, [&](Reader & in) {
        Check(in.Fixed(8) == title_id, "Shader file for a different game");
        parsed.label = in.Text();
        parsed.revision = in.Text(MaxRevision);
        parsed.note = in.Text(MaxNote);
        Check(ValidProfileText(parsed.label, 4096) && ValidProfileText(parsed.revision, MaxRevision) &&
                  ValidProfileText(parsed.note, MaxNote),
              "Invalid profile description");
        const auto flags = in.Var();
        Check(flags <= 1, "Unknown profile flags");
        parsed.full_game = flags & 1;
    });
    std::set<std::string> source_names;
    section(SourceSection, [&](Reader & in) { parsed.source_files = ReadSourceFiles(in, source_names); });
    std::vector<std::string> contexts;
    section(ContextSection, [&](Reader & in) {
        const auto count = in.Count(65536);
        std::set<std::string> distinct;
        for (size_t i = 0; i < count; ++i)
        {
            GameVersion version{in.Hash(), in.Text(MaxVersionText), in.Text(MaxVersionText)};
            Check(distinct.insert(version.context).second, "Duplicate profile context");
            Check(ValidProfileText(version.base, MaxVersionText) && ValidProfileText(version.update, MaxVersionText), "Invalid profile version");
            contexts.push_back(version.context);
            parsed.versions.push_back(std::move(version));
        }
        const auto index = in.Var();
        Check(index <= contexts.size(), "Invalid profile context");
        parsed.source_context = index ? contexts[index - 1] : std::string{};
    });
    section(ResourceSection, [&](Reader & in) { parsed.resources = ReadResources(in, source_names); });
    const auto & resources = parsed.resources;
    auto & programs = parsed.programs;
    section(ProgramSection, [&](Reader & in) {
        const auto count = in.Count(ProfileLimits{}.programs);
        programs.reserve(count);
        std::set<std::string> identities;
        std::map<u32, u64> ends;
        size_t code_budget{};
        for (size_t i = 0; i < count; ++i)
        {
            Program p;
            const auto words = in.Var();
            Check(words && words <= 1024 * 1024 / 8, "Compact program bounds");
            p.size = words * 8;
            p.sha = std::string(in.Take(ProgramIdentityBytes));
            Check(identities.insert(p.sha).second, "Two programs share one SHA-256");
            const auto locator_count = in.Count(MaxProgramLocators);
            Check(locator_count != 0, "Compact program without locator");
            for (size_t n = 0; n < locator_count; ++n)
            {
                Locator l;
                l.resource = in.Word();
                auto & end = ends[l.resource];
                const auto delta = UnZigZag(in.Var());
                Check(delta >= 0 ? static_cast<u64>(delta) <= UINT64_MAX - end : static_cast<u64>(-(delta + 1)) < end,
                      "Compact program offset overflow");
                l.offset = end + static_cast<u64>(delta);
                CheckLocator(resources, p, l);
                end = l.offset + p.size;
                p.locators.push_back(l);
            }
            code_budget += p.size;
            Check(code_budget <= ProfileLimits{}.code_bytes, "Compact code budget");
            programs.push_back(std::move(p));
        }
    });
    auto & envs = parsed.envs;
    section(EnvironmentSection, [&](Reader & in) {
        const auto count = in.Count(ProfileLimits{}.environments);
        envs.resize(count);
        u64 program{};
        size_t map_budget{};
        for (auto & env : envs)
        {
            program += in.Var();
            Check(program < programs.size(), "Compact environment program");
            env.program = static_cast<u32>(program);
            const auto stage = in.Var();
            Check(stage < EmulatorStage.size(), "Compact stage");
            env.fields[6] = EmulatorStage[stage];
            const auto facts = ReadFacts(in, 65536);
            map_budget += facts.size();
            Check(map_budget <= ProfileLimits{}.map_entries, "Compact map budget");
            for (const auto & fact : facts)
                Apply(env, fact);
            for (auto & map : env.maps)
                std::sort(map.begin(), map.end());
            CheckEnvironment(env, programs);
        }
    });
    std::vector<std::vector<u32>> states;
    section(StateSection, [&](Reader & in) {
        const auto read = [&] {
            GuestState::Registers registers(in.Count(GuestState::RegisterCount + 2));
            for (size_t i = 0; i < registers.size(); ++i)
            {
                const auto gap = in.Var();
                const auto key = i ? u64{registers[i - 1].first} + 1 + gap : gap;
                Check(key <= GuestState::EngineHintKey, "Invalid guest state register");
                registers[i] = {static_cast<u32>(key), in.Word()};
            }
            return registers;
        };
        const auto base = read();
        const auto count = in.Count(ProfileLimits{}.recipes);
        states.reserve(count);
        GuestState::Registers state;
        for (size_t i = 0; i < count; ++i)
        {
            const auto patch = read();
            state.clear();
            size_t a{}, b{};
            while (a < base.size() || b < patch.size())
            {
                if (b == patch.size() || (a < base.size() && base[a].first < patch[b].first))
                    state.push_back(base[a++]);
                else
                {
                    if (a < base.size() && base[a].first == patch[b].first) ++a;
                    state.push_back(patch[b++]);
                }
                if (!state.back().second) state.pop_back();
            }
            auto words = GuestState::ToWords(state);
            Check(words.has_value(), "Invalid guest state");
            states.push_back(std::move(*words));
        }
    });
    struct GraphicsBindings
    {
        std::array<std::array<u32, 18>, 5> cbuf_sizes{};
        std::array<u32, 2> texture_pool{};
    };
    struct ComputeBindings
    {
        std::array<u32, 8> cbuf_sizes{};
        u32 texture_table_size{};
    };
    std::vector<GraphicsBindings> graphics_bindings;
    std::vector<ComputeBindings> compute_bindings;
    section(BindingSection, [&](Reader & in) {
        const auto sizes = [&](auto & banks) {
            const auto mask = in.Var();
            Check(mask < (u64{1} << banks.size()), "Invalid constant buffer sizes");
            for (size_t bank = 0; bank < banks.size(); ++bank)
                if (mask >> bank & 1)
                {
                    banks[bank] = in.Word();
                    Check(banks[bank] != 0, "Invalid constant buffer sizes");
                }
        };
        graphics_bindings.resize(in.Count(ProfileLimits{}.recipes));
        for (auto & binding : graphics_bindings)
        {
            for (auto & stage : binding.cbuf_sizes)
                sizes(stage);
            for (auto & n : binding.texture_pool)
                n = in.Word();
        }
        compute_bindings.resize(in.Count(ProfileLimits{}.recipes));
        for (auto & binding : compute_bindings)
        {
            sizes(binding.cbuf_sizes);
            binding.texture_table_size = in.Word();
        }
    });
    section(RecipeSection, [&](Reader & in) {
        const auto count = in.Count(ProfileLimits{}.recipes);
        parsed.pending.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            Pending p;
            auto & r = p.recipe;
            const auto origin = in.Var();
            Check(origin == 0 || origin == 1 || origin == 3, "Unknown recipe origin");
            r.synthetic = origin & 1;
            r.played = origin & 2;
            const auto context_count = in.Count(contexts.size());
            for (size_t n = 0; n < context_count; ++n)
            {
                const auto index = in.Var();
                Check(index != 0 && index <= contexts.size(), "Invalid profile context");
                Check(std::find(r.source_contexts.begin(), r.source_contexts.end(), contexts[index - 1]) == r.source_contexts.end(),
                      "Duplicate recipe context");
                r.source_contexts.push_back(contexts[index - 1]);
            }
            // Until import chooses, the latest version recorded.
            if (!r.source_contexts.empty()) r.source_context = r.source_contexts.back();
            const auto kind = in.Var();
            Check(kind <= 1, "Compact recipe kind");
            r.compute = kind == 1;
            u64 mask = u64{1} << PortableCompute;
            if (!r.compute)
            {
                const auto state = in.Var();
                Check(state < states.size(), "Compact state reference");
                r.guest_words = states[static_cast<size_t>(state)];
                mask = in.Var();
                Check(mask && mask < (u64{1} << PortableCompute), "Compact recipe stages");
            }
            const auto binding = in.Var();
            if (r.compute)
            {
                Check(binding < compute_bindings.size(), "Compact binding reference");
                r.compute_cbuf_sizes = compute_bindings[binding].cbuf_sizes;
                r.texture_table_size = compute_bindings[binding].texture_table_size;
            }
            else
            {
                Check(binding < graphics_bindings.size(), "Compact binding reference");
                r.cbuf_sizes = graphics_bindings[binding].cbuf_sizes;
                r.texture_pool = graphics_bindings[binding].texture_pool;
            }
            for (u32 stage = 0; stage <= PortableCompute; ++stage)
            {
                if (!(mask >> stage & 1)) continue;
                const auto id = in.Var();
                Check(id < envs.size() && envs[id].fields[6] == EmulatorStage[stage], "Compact environment reference");
                p.ids.push_back(static_cast<u32>(id));
            }
            if (r.compute)
            {
                r.workgroup_size = envs[p.ids.front()].workgroup;
                r.shared_memory_size = envs[p.ids.front()].shared;
            }
            CheckRecipeSizes(r, envs, p.ids);
            parsed.pending.push_back(std::move(p));
        }
    });
    Identify(parsed);
    return parsed;
}
// Runs independent jobs on a few threads with at most Budget bytes in flight,
// unless one job alone needs more. Progress and cancellation stay on the
// calling thread; the first exception of a job is rethrown after all stop.
void RunBounded(size_t count, const std::vector<u64>& footprints, const ResourceProgress& progress,
                const std::function<void(size_t)>& job)
{
    constexpr u64 Budget = 768ULL << 20;
    const size_t threads = std::min<size_t>({count, std::max<size_t>(1, std::thread::hardware_concurrency()), 4});
    std::mutex mutex;
    std::condition_variable changed;
    size_t next{}, done{}, running{};
    u64 in_flight{};
    bool stop{}, cancelled{};
    std::exception_ptr failure;
    const auto worker = [&] {
        std::unique_lock lock(mutex);
        for (;;) {
            changed.wait(lock, [&] { return stop || next == count || running == 0 || in_flight + footprints[next] <= Budget; });
            if (stop || next == count) return;
            const size_t index = next++;
            ++running;
            in_flight += footprints[index];
            lock.unlock();
            std::exception_ptr error;
            try { job(index); } catch (...) { error = std::current_exception(); }
            lock.lock();
            --running;
            in_flight -= footprints[index];
            ++done;
            if (error && !failure) { failure = error; stop = true; }
            changed.notify_all();
        }
    };
    std::vector<std::thread> pool;
    try {
        for (size_t i = 0; i < threads; ++i) pool.emplace_back(worker);
    } catch (const std::system_error&) { /* Fewer threads; at least the caller works. */ }
    if (pool.empty()) worker();
    {
        std::unique_lock lock(mutex);
        for (size_t reported = SIZE_MAX; ; ) {
            if (done != reported && !stop) {
                reported = done;
                lock.unlock();
                const bool keep = !progress || progress(reported, count);
                lock.lock();
                if (!keep) { stop = cancelled = true; changed.notify_all(); }
            }
            if (done == count || (stop && running == 0)) break;
            changed.wait(lock, [&] { return done != reported || done == count || (stop && running == 0); });
        }
    }
    for (auto& thread : pool) thread.join();
    if (failure) std::rethrow_exception(failure);
    Check(!cancelled, "Shader profile operation cancelled");
}
// The emulator hash of program bytes: what the shader cache names a program by.
u64 EmulatorHash(std::string_view code)
{
    const auto candidates = ProgramHashCandidates(code);
    return candidates[1] ? candidates[1] : candidates[0];
}
struct LoadedPrograms
{
    std::vector<std::shared_ptr<const std::string>> codes; // Null: not available.
    size_t missing_resources{}, incompatible_resources{}, groups{};
};
// Reads and verifies the needed programs from the game, or from programs already
// in memory. Every byte loaded is checked against the program's identity.
LoadedPrograms LoadPrograms(Parsed & parsed, const std::set<size_t> & needed_programs, const std::filesystem::path & root, u64 title_id,
                            Timing::Metrics * timings, const ResourceProgress & progress, const ProgramMap * existing_programs)
{
    const auto & source_files = parsed.source_files;
    auto & resources = parsed.resources;
    const auto & programs = parsed.programs;
    LoadedPrograms loaded;
    auto & codes = loaded.codes;
    codes.resize(programs.size());
    std::vector<size_t> unresolved_programs;
    for (auto id : needed_programs)
    {
        if (existing_programs)
        {
            const auto found = existing_programs->find(programs[id].sha);
            if (found != existing_programs->end() && found->second && Verify(*found->second, programs[id], timings)) codes[id] = found->second;
        }
        if (!codes[id]) unresolved_programs.push_back(id);
    }
    if (unresolved_programs.empty()) return loaded;
    // Every resource a pass may read, for the alias search and the progress total.
    std::set<size_t> candidate_resources;
    for (auto id : unresolved_programs)
        for (const auto & locator : programs[id].locators)
            candidate_resources.insert(locator.resource);
    // Find the NCAs these resources name, in whichever containers hold them, then
    // still verify every requested program/resource. No guessed shader is accepted.
    ContentMap contents;
    {
        std::set<std::string> needed;
        const auto need = [&](const Resource & resource) {
            if (!Spanned(resource.kind)) needed.insert(resource.file);
            for (const auto & span : resource.spans)
                needed.insert(span.file);
        };
        for (const auto rid : candidate_resources)
        {
            need(resources[rid]);
            if (resources[rid].dictionary) need(resources[resources[rid].dictionary - 1]);
        }
        contents = LocateContents(root, title_id, source_files, needed, timings,
                                  progress ? ResourceProgress([&](size_t, size_t) { return progress(0, candidate_resources.size()); })
                                           : ResourceProgress{},
                                  MakeContentProbes(resources, programs, unresolved_programs));
    }
    Timing::Timer resources_timer(timings, Timing::Phase::ResourceTotal);
    // Zstd dictionaries load once, before the resources that use them. One that
    // cannot load makes only those resources unavailable.
    using DictionaryKey = std::tuple<u64, u64, u64, std::string>; // resource + 1, offset, size, SHA-256
    const auto dictionary_key = [](const Resource & r) { return DictionaryKey{r.dictionary, r.dictionary_offset, r.dictionary_size, r.dictionary_hash}; };
    struct LoadedDictionary
    {
        std::shared_ptr<const ZstdDictionary> value;
        bool missing{};
    };
    std::map<DictionaryKey, LoadedDictionary> dictionaries;
    for (const auto rid : candidate_resources)
        if (resources[rid].dictionary) dictionaries.try_emplace(dictionary_key(resources[rid]));
    {
        u64 source{};
        std::string decoded;
        bool source_missing{};
        for (auto & [key, dictionary] : dictionaries)
        {
            const auto & [resource, offset, size, hash] = key;
            try
            {
                if (resource != source)
                {
                    source = resource;
                    decoded.clear();
                    source_missing = false;
                    decoded = ZstdBytes(contents, resources[resource - 1], nullptr, timings);
                }
                Check(decoded.size() == resources[resource - 1].decoded_size, "Compact dictionary resource unavailable");
                const auto bytes = std::string_view(decoded).substr(offset, size);
                Check(Hash(bytes, timings) == hash, "Compact dictionary fingerprint mismatch");
                dictionary.value = std::make_shared<const ZstdDictionary>(bytes);
            }
            catch (const std::filesystem::filesystem_error & error)
            {
                source_missing = error.code() == std::errc::no_such_file_or_directory;
                dictionary.missing = source_missing;
            }
            catch (const std::runtime_error &)
            {
                dictionary.missing = source_missing;
            }
        }
    }
    const auto resource_dictionary = [&](const Resource & r) -> const ZstdDictionary * {
        if (!r.dictionary) return nullptr;
        const auto & entry = dictionaries.at(dictionary_key(r));
        if (entry.missing)
            throw std::filesystem::filesystem_error("Game content not found", std::make_error_code(std::errc::no_such_file_or_directory));
        Check(entry.value != nullptr, "Compact dictionary unavailable");
        return entry.value.get();
    };
    using Request = std::pair<size_t, u64>; // program, offset
    struct Job
    {
        size_t resource;
        std::vector<Request> requests;
        std::vector<size_t> missing, incompatible; // programs this job failed to load
    };
    std::vector<Job> finished;
    // Pass n tries the n-th locator of the programs still unresolved, so a program
    // shared by several game versions loads from whichever version is installed.
    for (size_t pass = 0; pass < MaxProgramLocators && !unresolved_programs.empty(); ++pass)
    {
        std::map<size_t, std::vector<Request>> groups;
        for (auto id : unresolved_programs)
            if (pass < programs[id].locators.size())
                groups[programs[id].locators[pass].resource].emplace_back(id, programs[id].locators[pass].offset);
        if (groups.empty()) break;
        // Groups are independent: each reads and decodes its own resource and fills
        // only its own programs. Largest first, so the longest decode starts first.
        std::vector<Job> jobs;
        std::vector<u64> footprints;
        for (auto & [rid, requests] : groups)
        {
            const auto & resource = resources[rid];
            // Raw bytes plus the decoded copy; Yaz0 decodes up to the last requested program.
            u64 prefix{};
            for (const auto & [id, offset] : requests)
                prefix = std::max(prefix, offset + programs[id].size);
            footprints.push_back(resource.kind == 0 ? 0
                                                    : resource.size + (resource.kind == 2 || Zstd(resource.kind) ? resource.decoded_size
                                                                       : resource.kind == 3                     ? 0
                                                                                                                : prefix));
            jobs.push_back({rid, std::move(requests), {}, {}});
        }
        {
            std::vector<size_t> order(jobs.size());
            std::iota(order.begin(), order.end(), size_t{});
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return footprints[a] > footprints[b]; });
            std::vector<Job> sorted_jobs;
            std::vector<u64> sorted_footprints;
            for (const auto i : order)
            {
                sorted_jobs.push_back(std::move(jobs[i]));
                sorted_footprints.push_back(footprints[i]);
            }
            jobs = std::move(sorted_jobs);
            footprints = std::move(sorted_footprints);
        }
        const size_t base = finished.size();
        const auto total = std::max(candidate_resources.size(), base + jobs.size());
        const ResourceProgress pass_progress =
            progress ? ResourceProgress([&](size_t done, size_t) { return progress(base + done, total); }) : ResourceProgress{};
        RunBounded(jobs.size(), footprints, pass_progress, [&](size_t index) {
            auto & job = jobs[index];
            const auto & resource = resources[job.resource];
            const auto unavailable = [&](bool missing, size_t id) { (missing ? job.missing : job.incompatible).push_back(id); };
            const auto all_unavailable = [&](bool missing) {
                for (const auto & [id, offset] : job.requests)
                    if (!codes[id]) unavailable(missing, id);
            };
            if (resource.kind == 0)
            {
                try
                {
                    // Open the NCA's container once, then validate each requested
                    // range independently so a truncated tail cannot discard
                    // otherwise useful programs from the same content.
                    const auto & content = Locate(contents, resource.file);
                    const auto length = content.size;
                    ContentInput input;
                    input.Open(content);
                    std::vector<Request> ordered;
                    for (const auto & request : job.requests)
                    {
                        const auto [id, offset] = request;
                        if (offset > length || programs[id].size > length - offset)
                            unavailable(false, id);
                        else
                            ordered.push_back(request);
                    }
                    std::stable_sort(ordered.begin(), ordered.end(), [](const Request & a, const Request & b) { return a.second < b.second; });
                    const auto accept = [&](size_t id, std::string_view bytes) {
                        if (!Verify(bytes, programs[id], timings))
                        {
                            unavailable(false, id);
                            return;
                        }
                        codes[id] = std::make_shared<const std::string>(bytes);
                    };
                    for (size_t first = 0; first < ordered.size();)
                    {
                        const auto begin = ordered[first].second;
                        auto end = begin + programs[ordered[first].first].size;
                        size_t last = first + 1;
                        while (last < ordered.size())
                        {
                            const auto [id, offset] = ordered[last];
                            const auto next_end = std::max(end, offset + programs[id].size);
                            if ((offset > end && offset - end > ReadBatchGap) || next_end - begin > ReadBatchBytes) break;
                            end = next_end;
                            ++last;
                        }
                        bool read{};
                        std::string batch(static_cast<size_t>(end - begin), '\0');
                        try
                        {
                            ReadInto(input, content.offset + begin, std::span(batch), timings);
                            read = true;
                        }
                        catch (const std::runtime_error &)
                        {
                            // A short or failed combined read must not discard
                            // independently readable shaders from that batch.
                        }
                        for (size_t i = first; i < last; ++i)
                        {
                            const auto [id, offset] = ordered[i];
                            if (read)
                                accept(id, std::string_view(batch).substr(offset - begin, programs[id].size));
                            else
                                try
                                {
                                    std::string code(static_cast<size_t>(programs[id].size), '\0');
                                    ReadInto(input, content.offset + offset, std::span(code), timings);
                                    accept(id, code);
                                }
                                catch (const std::runtime_error &)
                                {
                                    unavailable(false, id);
                                }
                        }
                        first = last;
                    }
                }
                catch (const std::filesystem::filesystem_error & error)
                {
                    all_unavailable(error.code() == std::errc::no_such_file_or_directory);
                }
                catch (const std::runtime_error &)
                {
                    all_unavailable(false);
                }
            }
            else
            {
                try
                {
                    size_t prefix{};
                    for (const auto & [id, offset] : job.requests)
                        prefix = std::max(prefix, static_cast<size_t>(offset + programs[id].size));
                    std::string decoded;
                    if (resource.kind == 1 || resource.kind == 4)
                        decoded = Yaz0Programs(contents, resource, prefix, timings);
                    else if (Zstd(resource.kind))
                        decoded = ZstdBytes(contents, resource, resource_dictionary(resource), timings);
                    else
                    {
                        auto raw = resource.kind >= 3 ? SpanBytes(contents, resource, timings)
                                                      : Bytes(contents, resource.file, resource.offset, resource.size, timings);
                        if (resource.kind >= 2) Check(Hash(raw, timings) == resource.hash, "Compact resource fingerprint mismatch");
                        decoded = Timing::Measure(timings, Timing::Phase::ResourceDecompression,
                                                  [&] { return resource.kind == 2 ? DecodeLz4(raw, resource.decoded_size) : std::move(raw); });
                    }
                    if (timings) timings->Add(Timing::Counter::ResourceDecodedBytes, decoded.size());
                    if (resource.kind == 2) Check(Hash(decoded, timings) == resource.segment_hash, "Compact NSO fingerprint mismatch");
                    for (const auto & [id, offset] : job.requests)
                    {
                        const auto & p = programs[id];
                        if (offset > decoded.size() || p.size > decoded.size() - offset)
                        {
                            job.incompatible.push_back(id);
                            continue;
                        }
                        auto code = decoded.substr(offset, p.size);
                        if (!Verify(code, p, timings))
                        {
                            job.incompatible.push_back(id);
                            continue;
                        }
                        codes[id] = std::make_shared<const std::string>(std::move(code));
                    }
                }
                catch (const std::filesystem::filesystem_error & error)
                {
                    all_unavailable(error.code() == std::errc::no_such_file_or_directory);
                }
                catch (const std::runtime_error &)
                {
                    all_unavailable(false);
                }
            }
        });
        std::erase_if(unresolved_programs, [&](size_t id) { return codes[id] != nullptr; });
        std::move(jobs.begin(), jobs.end(), std::back_inserter(finished));
    }
    if (progress) Check(progress(finished.size(), finished.size()), "Shader profile operation cancelled");
    // A resource counts as missing or incompatible only when a program it should
    // have provided stayed unresolved: another version's locator is not a failure.
    const std::set<size_t> unresolved(unresolved_programs.begin(), unresolved_programs.end());
    const auto lost = [&](const std::vector<size_t> & ids) {
        return std::any_of(ids.begin(), ids.end(), [&](size_t id) { return unresolved.contains(id); });
    };
    for (const auto & job : finished)
    {
        loaded.missing_resources += lost(job.missing);
        loaded.incompatible_resources += lost(job.incompatible);
    }
    loaded.groups = finished.size();
    return loaded;
}
} // namespace
static std::vector<Recipe> ReadCompactImpl(std::string_view payload, const std::filesystem::path & root, u64 title_id, size_t * unresolved_count,
                                           Timing::Metrics * timings, const ResourceProgress & progress, ImportReport * report,
                                           std::string_view target_context, const std::set<std::string> * existing_recipes,
                                           const ProgramMap * existing_programs, bool allow_cross_version)
{
    Timing::Timer schema_timer(timings, Timing::Phase::SchemaAndLocators);
    auto parsed = Parse(payload, title_id);
    auto & programs = parsed.programs;
    const auto & envs = parsed.envs;
    auto & pending = parsed.pending;
    size_t total_environment_bytes{};
    if (unresolved_count) *unresolved_count = 0;
    // Parsing and all table references finish before touching local resources.
    schema_timer.Stop();
    ImportReport outcome;
    // Reject only incompatible recipe contexts. A merged community revision can
    // contain recipes from multiple updates without forcing irrelevant reads.
    std::set<size_t> needed_programs;
    // A recipe recorded with the running version is used as such; otherwise as
    // a candidate of the latest other version it was recorded with.
    std::vector<bool> compatible(pending.size());
    for (size_t i = 0; i < pending.size(); ++i)
    {
        auto & r = pending[i].recipe;
        const auto & contexts = r.source_contexts;
        if (target_context.empty())
            compatible[i] = contexts.empty();
        else if (std::find(contexts.begin(), contexts.end(), target_context) != contexts.end())
        {
            r.source_context = std::string(target_context);
            compatible[i] = true;
        }
        else if (allow_cross_version && ValidContext(target_context))
        {
            const auto other = std::find_if(contexts.rbegin(), contexts.rend(), [](const auto & c) { return ValidContext(c); });
            if (other != contexts.rend())
            {
                r.source_context = *other;
                compatible[i] = true;
            }
        }
        if (compatible[i]) r.identity = RecipeIdentity(r.source_context, pending[i].content);
    }
    for (size_t i = 0; i < pending.size(); ++i)
    {
        const auto & p = pending[i];
        if (!compatible[i])
        {
            ++outcome.skipped;
            ++outcome.incompatible_contexts;
            continue;
        }
        if (existing_recipes && existing_recipes->contains(p.recipe.identity))
        {
            ++outcome.reused;
            outcome.reused_ids.push_back(p.recipe.identity);
            continue;
        }
        for (auto id : p.ids)
            needed_programs.insert(envs[id].program);
    }
    if (needed_programs.empty())
    {
        if (report) *report = outcome;
        if (outcome.reused) return {};
        throw std::runtime_error("No recipes match the installed base/update identity; export a profile from an identified capture");
    }
    auto loaded = LoadPrograms(parsed, needed_programs, root, title_id, timings, progress, existing_programs);
    const auto & codes = loaded.codes;
    outcome.missing_resources = loaded.missing_resources;
    outcome.incompatible_resources = loaded.incompatible_resources;
    // Recipes name programs as the shader cache does, by the hash of the bytes it reads.
    for (const auto id : needed_programs)
        if (codes[id]) programs[id].hash = EmulatorHash(*codes[id]);
    Timing::Timer environment_timer(timings, Timing::Phase::EnvironmentAssembly);
    std::vector<SharedShaderBytes> assembled(envs.size());
    size_t env_budget{};
    for (size_t id = 0; id < envs.size(); ++id)
    {
        const auto & env = envs[id];
        if (!codes[env.program]) continue;
        assembled[id] = Environment(env, codes[env.program], timings);
        env_budget += assembled[id].MetadataBytes();
        Check(env_budget <= 512 * 1024 * 1024, "Compact environment budget");
    }
    environment_timer.Stop();
    Timing::Timer recipe_timer(timings, Timing::Phase::RecipeAssembly);
    std::vector<Recipe> result;
    result.reserve(pending.size());
    for (size_t i = 0; i < pending.size(); ++i)
    {
        auto & p = pending[i];
        if (!compatible[i] || (existing_recipes && existing_recipes->contains(p.recipe.identity))) continue;
        bool available = true;
        for (auto id : p.ids)
            available = available && codes[envs[id].program];
        if (!available)
        {
            ++outcome.skipped;
            continue;
        }
        for (auto id : p.ids)
        {
            total_environment_bytes += sizeof(SharedShaderBytes);
            Check(total_environment_bytes <= 512 * 1024 * 1024, "Compact recipe memory budget");
            p.recipe.environments.push_back(assembled[id]);
            p.recipe.shader_hashes[p.recipe.compute ? 0 : GraphicsSlot(envs[id].fields[6])] = programs[envs[id].program].hash;
        }
        // A synthetic recipe's environments hold no answers a live draw could confirm, so the dependency proof
        // of another context (another version, or ExeFS patches and cheats) cannot pass. They are trusted by
        // their exact pipeline key, as recipes of the running context are.
        p.recipe.requires_validation = !p.recipe.synthetic && p.recipe.source_context != target_context;
        outcome.candidates += p.recipe.requires_validation;
        result.push_back(std::move(p.recipe));
    }
    outcome.accepted = result.size();
    if (report) *report = outcome;
    Check(!result.empty() || outcome.reused, "No compatible shader recipes: local resources are missing or incompatible");
    if (timings)
    {
        timings->Add(Timing::Counter::Programs, programs.size());
        timings->Add(Timing::Counter::Environments, envs.size());
        timings->Add(Timing::Counter::Recipes, result.size());
        timings->Add(Timing::Counter::ResourceGroups, loaded.groups);
    }
    return result;
}
std::vector<Recipe> ReadCompact(std::string_view payload, const std::filesystem::path & root, u64 title_id, size_t * unresolved_count,
                                Timing::Metrics * timings, const ResourceProgress & progress, ImportReport * report, std::string_view target_context,
                                const std::set<std::string> * existing_recipes, const ProgramMap * existing_programs, bool allow_cross_version)
{
    return ReadCompactImpl(payload, root, title_id, unresolved_count, timings, progress, report, target_context, existing_recipes,
                           existing_programs, allow_cross_version);
}
std::string ProgramIdentity(std::string_view code)
{
    const auto digest = ResourceDigest(code);
    return std::string(reinterpret_cast<const char *>(digest.data()), ProgramIdentityBytes);
}
std::string ProgramKey(std::string_view code)
{
    return ResourceSha256(code).substr(0, 2 * ProgramIdentityBytes) + std::string(64 - 2 * ProgramIdentityBytes, '0');
}
u64 ProgramKeyHash(std::string_view key)
{
    Check(key.size() >= 16 && key.substr(0, 16).find_first_not_of("0123456789abcdef") == std::string_view::npos, "Invalid program key");
    return std::stoull(std::string(key.substr(0, 16)), nullptr, 16) | 1;
}
ProfileInspection InspectCompact(std::string_view payload, u64 title_id)
{
    const auto parsed = Parse(payload, title_id);
    ProfileInspection result;
    std::set<std::string> versions;
    for (const auto & p : parsed.pending)
    {
        if (p.recipe.compute)
            ++result.compute;
        else
            ++result.graphics;
        versions.insert(p.recipe.source_contexts.begin(), p.recipe.source_contexts.end());
    }
    result.versions = versions.size();
    result.programs = parsed.programs.size();
    result.environments = parsed.envs.size();
    result.label = parsed.label;
    result.revision = parsed.revision;
    result.note = parsed.note;
    result.full_game = parsed.full_game;
    for (const auto & p : parsed.pending)
    {
        result.synthetic += p.recipe.synthetic;
        result.played += p.recipe.synthetic && p.recipe.played;
    }
    // The versions recipes still use, the exported one last; the others follow
    // the recipe order, which keeps the previous profile's versions first.
    for (const auto & version : parsed.versions)
        if (versions.contains(version.context) && version.context != parsed.source_context) result.targets.push_back(version);
    for (const auto & version : parsed.versions)
        if (version.context == parsed.source_context) result.targets.push_back(version);
    result.primary_context = parsed.source_context;
    if (!parsed.source_context.empty()) versions.insert(parsed.source_context);
    result.contexts.assign(versions.begin(), versions.end());
    return result;
}
namespace
{
// The profile model of parsed tables. keys[program] names each program; an empty
// key is a program that could not be read, and the recipes using it are left out.
JsonValue DecodeParsed(const Parsed & parsed, u64 title_id, const std::vector<std::string> & keys)
{
    const auto num = [](u64 n) { return n > UINT32_MAX ? JsonValue(std::to_string(n)) : JsonValue(static_cast<u32>(n)); };
    const auto array = [&](const auto & values) {
        JsonValue result(JsonValueType::Array);
        for (const auto n : values)
            result.Append(num(n));
        return result;
    };
    JsonValue profile(JsonValueType::Object);
    char title[17];
    std::snprintf(title, sizeof(title), "%016llx", static_cast<unsigned long long>(title_id));
    profile["format"] = "nxshader";
    profile["version"] = 2U;
    profile["guest_state_version"] = 1U;
    profile["title_id"] = title;
    profile["content_label"] = parsed.label;
    profile["revision"] = parsed.revision;
    profile["note"] = parsed.note;
    if (parsed.full_game) profile["full_game"] = true;
    profile["versions"] = JsonValue(JsonValueType::Object);
    for (const auto & version : parsed.versions)
    {
        auto & entry = profile["versions"][version.context];
        entry["base"] = version.base;
        entry["update"] = version.update;
    }
    profile["source_context"] = parsed.source_context;
    profile["source_files"] = JsonValue(JsonValueType::Array);
    for (const auto & file : parsed.source_files)
    {
        JsonValue entry(JsonValueType::Object);
        entry["name"] = file.name;
        entry["size"] = num(file.size);
        entry["header"] = file.header;
        profile["source_files"].Append(std::move(entry));
    }
    const auto describe = [&](const Resource & resource) {
        JsonValue locator(JsonValueType::Object);
        locator["encoding"] = Encodings[resource.kind];
        if (!Spanned(resource.kind)) locator["file"] = resource.file;
        if (resource.kind == 1 || resource.kind == 2 || resource.kind == 5)
        {
            locator["container_offset"] = num(resource.offset);
            locator["container_size"] = num(resource.size);
        }
        if (resource.kind == 2 || Zstd(resource.kind)) locator["decoded_size"] = num(resource.decoded_size);
        if (resource.kind == 2)
        {
            locator["container_sha256"] = resource.hash;
            locator["segment_sha256"] = resource.segment_hash;
        }
        if (Spanned(resource.kind))
        {
            locator["resource_size"] = num(resource.size);
            locator["resource_sha256"] = resource.hash;
            locator["physical_spans"] = JsonValue(JsonValueType::Array);
            for (const auto & span : resource.spans)
            {
                JsonValue entry(JsonValueType::Object);
                entry["file"] = span.file;
                entry["offset"] = num(span.offset);
                entry["size"] = num(span.size);
                locator["physical_spans"].Append(std::move(entry));
            }
        }
        return locator;
    };
    profile["shaders"] = JsonValue(JsonValueType::Object);
    for (size_t id = 0; id < parsed.programs.size(); ++id)
    {
        if (keys[id].empty()) continue;
        const auto & p = parsed.programs[id];
        auto & shader = profile["shaders"][keys[id]];
        shader["size"] = num(p.size);
        shader["references"] = JsonValue(JsonValueType::Array);
        for (const auto & l : p.locators)
        {
            const auto & resource = parsed.resources[l.resource];
            auto locator = describe(resource);
            if (resource.dictionary)
            {
                auto dictionary = describe(parsed.resources[resource.dictionary - 1]);
                dictionary["offset"] = num(resource.dictionary_offset);
                dictionary["size"] = num(resource.dictionary_size);
                dictionary["sha256"] = resource.dictionary_hash;
                locator["dictionary"] = std::move(dictionary);
            }
            locator["offset"] = num(l.offset);
            locator["size"] = num(p.size);
            shader["references"].Append(std::move(locator));
        }
    }
    std::vector<std::string> env_ids;
    profile["environments"] = JsonValue(JsonValueType::Object);
    for (const auto & env : parsed.envs)
    {
        if (keys[env.program].empty())
        {
            env_ids.emplace_back();
            continue;
        }
        JsonValue e(JsonValueType::Object);
        e["shader"] = keys[env.program];
        for (size_t i = 0; i < Fields.size(); ++i)
            e[Fields[i]] = num(env.fields[i]);
        for (size_t m = 0; m < Maps.size(); ++m)
        {
            e[Maps[m]] = JsonValue(JsonValueType::Array);
            for (const auto & [key, value] : env.maps[m])
            {
                JsonValue pair(JsonValueType::Array);
                pair.Append(num(key));
                pair.Append(num(value));
                e[Maps[m]].Append(std::move(pair));
            }
        }
        if (env.fields[6] == 5)
        {
            e["workgroup_size"] = array(env.workgroup);
            e["shared_memory_size"] = num(env.shared);
        }
        else if (env.fields[6] == 3)
            e["gp_passthrough_mask"] = array(env.passthrough);
        env_ids.push_back(ResourceSha256(Canonical(e)));
        profile["environments"][env_ids.back()] = std::move(e);
    }
    profile["recipes"] = JsonValue(JsonValueType::Array);
    for (const auto & p : parsed.pending)
    {
        const auto & r = p.recipe;
        if (std::any_of(p.ids.begin(), p.ids.end(), [&](u32 id) { return env_ids[id].empty(); })) continue;
        // Programs as the model names them: from their key, which recipes share.
        std::array<u64, 6> hashes{};
        for (const auto id : p.ids)
            hashes[r.compute ? 0 : GraphicsSlot(parsed.envs[id].fields[6])] = ProgramKeyHash(keys[parsed.envs[id].program]);
        JsonValue result(JsonValueType::Object);
        result["kind"] = r.compute ? "compute" : "graphics";
        result["source_contexts"] = JsonValue(JsonValueType::Array);
        for (const auto & context : r.source_contexts)
            result["source_contexts"].Append(context);
        if (r.compute)
        {
            result["shader_hash"] = num(hashes[0]);
            result["shared_memory_size"] = num(r.shared_memory_size);
            result["workgroup_size"] = array(r.workgroup_size);
            result["cbuf_sizes"] = array(r.compute_cbuf_sizes);
            result["texture_table_size"] = num(r.texture_table_size);
        }
        else
        {
            result["guest_words"] = array(r.guest_words);
            result["shader_hashes"] = array(hashes);
            result["cbuf_sizes"] = JsonValue(JsonValueType::Array);
            for (const auto & sizes : r.cbuf_sizes)
                result["cbuf_sizes"].Append(array(sizes));
            result["texture_pool"] = array(r.texture_pool);
        }
        result["environments"] = JsonValue(JsonValueType::Array);
        for (const auto id : p.ids)
            result["environments"].Append(env_ids[id]);
        if (r.synthetic) result["synthetic"] = true;
        if (r.played) result["played"] = true;
        profile["recipes"].Append(std::move(result));
    }
    profile["unresolved_recipes"] = JsonValue(JsonValueType::Array);
    return profile;
}
std::string Hex(std::string_view bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const unsigned char c : bytes)
    {
        result += digits[c >> 4];
        result += digits[c & 15];
    }
    return result;
}
} // namespace
JsonValue DecodeCompact(std::string_view payload, u64 title_id)
{
    const auto parsed = Parse(payload, title_id);
    std::vector<std::string> keys;
    for (const auto & p : parsed.programs)
        keys.push_back(Hex(p.sha) + std::string(64 - 2 * ProgramIdentityBytes, '0'));
    return DecodeParsed(parsed, title_id, keys);
}
ProfileInspection Inspect(const std::filesystem::path & path, u64 title_id)
{
    const auto size = std::filesystem::file_size(path);
    Check(size <= MaxProfileFileBytes, "Profile size limit");
    std::ifstream file(path, std::ios::binary);
    std::string packed(static_cast<size_t>(size), '\0');
    Check(bool(file.read(packed.data(), static_cast<std::streamsize>(size))) && file.peek() == std::char_traits<char>::eof(),
          "Cannot read profile metadata");
    Check(packed.starts_with(ProfileMagic), "Expected a compressed NXShader profile");
    auto result = InspectCompact(Unpack(packed), title_id);
    result.bytes = size;
    return result;
}
} // namespace VideoCommon::ShaderProfile
