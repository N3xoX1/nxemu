// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_common/common_types.h"
#include "yuzu_common/cityhash.h"
#include "yuzu_video_core/shader_profile_codec.h"
#include "yuzu_video_core/shader_profile_hash.h"
#include "yuzu_video_core/shader_profile_limits.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace VideoCommon::ShaderProfile::Journal
{
// Local metadata only. Each independently checked frame is committed by its trailing
// digest. A torn final frame can be discarded without losing preceding sessions.
using Entry = std::array<u32, 383>; // source feature bits, 278 guest + 104 key words
struct EntryHash {
    size_t operator()(const Entry& entry) const noexcept {
        return Common::CityHash64(reinterpret_cast<const char*>(entry.data()), sizeof(entry));
    }
};
// Each frame holds one written batch: u32 entry count, u32 stored size, the
// entries (compressed when it saves space) and a SHA-256 of all of it.
inline constexpr std::string_view Magic = "NXREC003";
// A compute entry: this flag in word 0, then the 64-byte pipeline key, then zeros.
inline constexpr u32 ComputeFlag = 0x80000000U;
inline constexpr size_t ComputeWords = 16;
inline bool IsCompute(const Entry& entry) { return entry[0] == ComputeFlag; }
inline constexpr size_t EntryBytes = 383 * 4;
inline constexpr size_t BatchHeader = 8, BatchDigest = 32;
// Matches the recipe limit of a profile.
inline constexpr size_t MaxEntries = MaxRecipes;
struct Contents
{
    // Preserve first-use order in the exported profile.
    std::vector<Entry> entries;
    size_t valid_bytes{Magic.size()};
    bool incomplete_tail{};
};
namespace Detail {
// Keep one full copy of each entry. Hash buckets contain indices, so vector
// growth cannot invalidate the index and hash collisions still compare exactly.
class FirstUseIndex {
public:
    explicit FirstUseIndex(std::vector<Entry>& entries_) : entries(entries_) {
        if(!entries.empty())throw std::logic_error("First-use index requires empty storage");
    }
    FirstUseIndex(const FirstUseIndex&)=delete;
    FirstUseIndex& operator=(const FirstUseIndex&)=delete;
    bool Insert(const Entry& entry,size_t hash) {
        const auto found=heads.find(hash);
        const size_t previous=found==heads.end()?End:found->second;
        for(auto i=previous;i!=End;i=next[i])
            if(entries[i]==entry)return false;
        const auto index=entries.size();
        entries.push_back(entry);
        try {
            next.push_back(previous);
            try {heads.insert_or_assign(hash,index);}
            catch(...) {next.pop_back();throw;}
        } catch(...) {entries.pop_back();throw;}
        return true;
    }
private:
    static constexpr size_t End=static_cast<size_t>(-1);
    std::vector<Entry>& entries;
    std::unordered_map<size_t,size_t> heads;
    std::vector<size_t> next;
};
} // namespace Detail
inline void PutEntry(std::string& bytes, const Entry& entry)
{
    for (u32 word : entry)
        for (unsigned shift = 0; shift != 32; shift += 8)
            bytes.push_back(static_cast<char>(word >> shift));
}
inline std::string EncodeBatch(std::span<const Entry> entries)
{
    std::string raw;
    raw.reserve(entries.size() * EntryBytes);
    for (const auto& entry : entries) PutEntry(raw, entry);
    const auto packed = PackFrame(raw);
    const std::string_view stored = packed.empty() ? std::string_view(raw) : std::string_view(packed);
    std::string frame;
    frame.reserve(BatchHeader + stored.size() + BatchDigest);
    for (u32 value : {static_cast<u32>(entries.size()), static_cast<u32>(stored.size())})
        for (unsigned shift = 0; shift != 32; shift += 8)
            frame.push_back(static_cast<char>(value >> shift));
    frame += stored;
    const auto digest = ResourceDigest(frame);
    frame.append(reinterpret_cast<const char*>(digest.data()), digest.size());
    return frame;
}
inline Contents Read(const std::filesystem::path & path)
{
    Contents result;
    if (!std::filesystem::exists(path)) return result;
    const auto size = std::filesystem::file_size(path);
    if (size > Magic.size() + MaxEntries * (BatchHeader + EntryBytes + BatchDigest))
        throw std::runtime_error("Guest recipe journal exceeds limit");
    std::ifstream in(path, std::ios::binary);
    std::string magic(Magic.size(), '\0');
    if (size < Magic.size())
    {
        // Torn creation: nothing was committed yet.
        result.valid_bytes = 0;
        result.incomplete_tail = size != 0;
        return result;
    }
    if (!in.read(magic.data(), magic.size()) || magic != Magic)
        throw std::runtime_error("Invalid guest recipe journal header");
    Detail::FirstUseIndex seen(result.entries);
    const auto add = [&](const char* bytes) {
        Entry entry{};
        for (size_t i = 0; i < entry.size(); ++i)
            for (unsigned j = 0; j != 4; ++j)
                entry[i] |= u32(static_cast<unsigned char>(bytes[i * 4 + j])) << (j * 8);
        const bool valid = IsCompute(entry)
            ? std::all_of(entry.begin() + 1 + ComputeWords, entry.end(), [](u32 word) { return word == 0; })
            : entry[0] <= 127 && entry[1] == 1;
        if (!valid) throw std::runtime_error("Unsupported guest recipe journal ABI");
        // Deduplicate exactly; keep chronological order of the first frame.
        seen.Insert(entry,EntryHash{}(entry));
    };
    // A frame that fails its digest ends the committed prefix, like a torn
    // append; the recorder truncates it instead of disabling recording.
    std::string frame, raw;
    size_t stored_entries{};
    while (size - result.valid_bytes >= BatchHeader + BatchDigest)
    {
        frame.resize(BatchHeader);
        if (!in.read(frame.data(), BatchHeader)) throw std::runtime_error("Cannot read guest recipe journal");
        size_t count{}, stored{};
        for (unsigned j = 0; j != 4; ++j) {
            count |= size_t(static_cast<unsigned char>(frame[j])) << (j * 8);
            stored |= size_t(static_cast<unsigned char>(frame[4 + j])) << (j * 8);
        }
        if (count == 0 || count > MaxEntries || stored > count * EntryBytes ||
            stored > size - result.valid_bytes - BatchHeader - BatchDigest) break;
        frame.resize(BatchHeader + stored + BatchDigest);
        if (!in.read(frame.data() + BatchHeader, stored + BatchDigest)) throw std::runtime_error("Cannot read guest recipe journal");
        const auto digest = ResourceDigest(std::string_view(frame).substr(0, BatchHeader + stored));
        if (!std::equal(digest.begin(), digest.end(), reinterpret_cast<const unsigned char*>(frame.data()) + BatchHeader + stored)) break;
        const auto bytes = std::string_view(frame).substr(BatchHeader, stored);
        if (stored == count * EntryBytes) raw.assign(bytes);
        else if (!UnpackFrame(bytes, count * EntryBytes, raw) || raw.size() != count * EntryBytes) break;
        // Bound the work of a journal whose frames all repeat the same entries.
        stored_entries += count;
        if (stored_entries > MaxEntries) throw std::runtime_error("Guest recipe journal exceeds limit");
        for (size_t i = 0; i < count; ++i) add(raw.data() + i * EntryBytes);
        result.valid_bytes += frame.size();
    }
    result.incomplete_tail = size != result.valid_bytes;
    return result;
}
struct WriteMetrics
{
    u64 frames{}, batches{}, bytes{}, encoding_ns{}, io_ns{}, cpu_ns{}, cpu_samples{}, cpu_cycles{}, cycle_samples{};
};
// Called only by the existing cache serialization worker, never on a draw.
inline void AppendBatch(const std::filesystem::path& path, std::span<const Entry> entries,
                        WriteMetrics* metrics = nullptr)
{
    if (entries.empty()) return;
    const auto started = metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::error_code error;
    const auto existing = std::filesystem::file_size(path, error);
    const bool fresh = error || existing == 0;
    const auto encode_start = metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::string bytes = fresh ? std::string(Magic) : std::string{};
    bytes += EncodeBatch(entries);
    const u64 encoding_ns = metrics ? std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-encode_start).count() : 0;
    std::ofstream out(path, std::ios::binary | std::ios::app);
    out.write(bytes.data(), bytes.size());
    out.flush(); out.close();
    if (!out) throw std::runtime_error("Cannot append guest recipe journal");
    if (metrics) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
        metrics->frames += entries.size(); ++metrics->batches;
        metrics->bytes += bytes.size();
        metrics->encoding_ns += encoding_ns;
        metrics->io_ns += elapsed-encoding_ns;
    }
}
inline void Append(const std::filesystem::path& path, const Entry& entry, WriteMetrics* metrics = nullptr) {
    AppendBatch(path, std::span<const Entry>(&entry, 1), metrics);
}
} // namespace VideoCommon::ShaderProfile::Journal
