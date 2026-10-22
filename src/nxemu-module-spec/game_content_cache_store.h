#pragma once
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <common/json.h>
#include <common/atomic_file.h>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
// Persistent content metadata is owned by the shared loader/frontend contract,
// not by the UI's game listing. Entries are hints only: callers must verify
// the on-disk size and mtime before trusting an associated Title ID.
namespace GameContentCacheStore
{
// Both indexes are new; unsupported schema versions are cache misses.
constexpr int64_t LibrarySchema = 1;
constexpr int64_t ContentSchema = 1;

// Only a complete well-formed record may be reused. Every caller also
// verifies the file signature against the actual filesystem entry.
inline bool ReadContainer(const JsonValue & item, std::string & path,
                          std::string & signature, std::vector<uint64_t> & applications)
{
    if (!item.isObject() || !item["path"].isString() || !item["file"].isString() ||
        !item["applications"].isArray()) return false;
    std::string name = item["path"].asString();
    std::string file = item["file"].asString();
    if (name.empty() || file.empty()) return false;
    std::vector<uint64_t> parsedIds;
    const JsonValue & ids = item["applications"];
    parsedIds.reserve(ids.size());
    for (uint32_t i = 0; i < ids.size(); ++i)
    {
        if (!ids[i].isString()) return false;
        const std::string text = ids[i].asString();
        if (text.size() != 16) return false;
        uint64_t id = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return false;
        parsedIds.push_back(id);
    }
    if (parsedIds.empty()) return false; // Unknown containers must be retried.
    path = std::move(name);
    signature = std::move(file);
    applications = std::move(parsedIds);
    return true;
}

struct Entry
{
    std::string signature;
    std::vector<uint64_t> applications;
    bool operator==(const Entry &) const = default;
};
using Entries = std::unordered_map<std::string, Entry>;
constexpr uintmax_t MaxBytes = 32 * 1024 * 1024;

inline std::filesystem::path IndexPath(const std::filesystem::path & dataRoot)
{
    return dataRoot / "game_content" / "index.json";
}

inline bool Read(const std::filesystem::path & path, Entries & result)
{
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    if (error || bytes > MaxBytes) return false;
    std::string contents(static_cast<size_t>(bytes), '\0');
    std::ifstream file(path, std::ios::binary);
    if (!file.read(contents.data(), static_cast<std::streamsize>(contents.size()))) return false;
    JsonValue root;
    if (!JsonReader().Parse(contents.data(), contents.data() + contents.size(), root) ||
        !root.isObject() || !root["schema"].isInt() ||
        root["schema"].asInt64() != ContentSchema ||
        !root["containers"].isArray()) return false;

    Entries parsed;
    const JsonValue & containers = root["containers"];
    for (uint32_t i = 0; i < containers.size(); ++i)
    {
        std::string name;
        Entry entry;
        if (ReadContainer(containers[i], name, entry.signature, entry.applications))
            parsed.insert_or_assign(std::move(name), std::move(entry));
    }
    result = std::move(parsed); // A damaged file never destroys the previous state.
    return true;
}

// Keep UI and content indexes readable across repeat saves on Windows as well
// as POSIX; an interrupted write cannot truncate the previous index.

inline bool Write(const std::filesystem::path & path, const Entries & contents)
{
    JsonValue root(JsonValueType::Object);
    root["schema"] = ContentSchema;
    JsonValue containers(JsonValueType::Array);
    std::vector<std::string> names;
    names.reserve(contents.size());
    for (const auto & [name, entry] : contents) names.push_back(name);
    std::sort(names.begin(), names.end());
    for (const std::string & name : names)
    {
        const auto & entry = contents.at(name);
        JsonValue item(JsonValueType::Object);
        item["path"] = name;
        item["file"] = entry.signature;
        JsonValue applications(JsonValueType::Array);
        for (uint64_t application : entry.applications)
        {
            char hex[17];
            std::snprintf(hex, sizeof(hex), "%016" PRIX64, application);
            applications.Append(hex);
        }
        item["applications"] = std::move(applications);
        containers.Append(std::move(item));
    }
    root["containers"] = std::move(containers);
    const std::string serialized = JsonStyledWriter().write(root);
    if (serialized.size() > MaxBytes) return false;

    return Common::WriteFileAtomically(path, serialized.data(), serialized.size());
}
} // namespace GameContentCacheStore
