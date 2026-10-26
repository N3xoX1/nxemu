// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_video_core/shader_profile_hash.h"
#include "yuzu_common/common_types.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

namespace VideoCommon::ShaderProfile {
inline bool ValidContext(std::string_view context) {
    return context.size() == 64 && context.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}
inline std::string ContentPart(u64 size, std::string_view header) {
    return std::to_string(size) + ":" + ResourceSha256(header);
}
inline std::string ContentContext(u64 title, std::string_view base, std::string_view update) {
    if (base.empty()) return {};
    return ResourceSha256("nxshader-content-2:" + std::to_string(title) + ":" + std::string(base) + ":" + std::string(update));
}
// Local cache association for content without a verified identity. It is never a
// profile, recording or export context.
inline std::string UnidentifiedContext(u64 title) {
    return ResourceSha256("nxshader-unidentified-1:" + std::to_string(title));
}
// A user-visible file or folder name part. Never parsed back.
inline std::string FilenamePart(std::string name) {
    for(char& c:name)if(static_cast<unsigned char>(c)<32 || std::string_view("<>:\"/\\|?*").find(c)!=std::string_view::npos)c='-';
    while(!name.empty() && (name.back()==' ' || name.back()=='.'))name.pop_back();
    if(name.size()>96) {
        size_t size=96;
        while(size && (static_cast<unsigned char>(name[size])&0xc0)==0x80)--size;
        name.resize(size);
    }
    return name.empty()?"Game":name;
}

// Local cache layout of one title:
//   vulkan.nxc                    pipelines shared by every version
//   versions/<label> (<id>)/      one recorded game version, <id> = 12 hex of its context
//     version.json  recipes.nxr  vulkan.idx  export-index.json
//   imported/                     state of the selected shared profile
//   warmup.json                   startup warmup failure backoff
// The folder name is for people only. Each file inside carries or is bound to
// the full context and is verified on reading.
inline constexpr size_t VersionIdChars = 12;
inline std::filesystem::path VersionsDirectory(const std::filesystem::path& cache) { return cache / "versions"; }
inline std::filesystem::path ImportedDirectory(const std::filesystem::path& cache) { return cache / "imported"; }
inline std::string VersionId(const std::filesystem::path& directory) {
    const auto name = directory.filename().string();
    if (name.size() < VersionIdChars + 2 || name.back() != ')' || name[name.size() - VersionIdChars - 2] != '(') return {};
    auto id = name.substr(name.size() - VersionIdChars - 1, VersionIdChars);
    return id.find_first_not_of("0123456789abcdef") == std::string::npos ? id : std::string{};
}
namespace Detail {
struct VersionDirectories {
    std::mutex mutex;
    std::map<std::string, std::filesystem::path> known;
};
inline VersionDirectories& KnownVersionDirectories() {
    static VersionDirectories directories;
    return directories;
}
} // namespace Detail
inline std::filesystem::path FindVersionDirectory(const std::filesystem::path& cache, std::string_view context) {
    if (!ValidContext(context)) throw std::runtime_error("Invalid shader version context");
    auto& state = Detail::KnownVersionDirectories();
    const auto key = cache.string() + "|" + std::string(context);
    std::error_code error;
    {
        std::scoped_lock lock(state.mutex);
        const auto found = state.known.find(key);
        if (found != state.known.end() && std::filesystem::is_directory(found->second, error)) return found->second;
    }
    const auto id = context.substr(0, VersionIdChars);
    std::filesystem::path result;
    for (auto it = std::filesystem::directory_iterator(VersionsDirectory(cache), error);
         !error && it != std::filesystem::directory_iterator{}; it.increment(error))
        if (VersionId(it->path()) == id && it->is_directory(error)) { result = it->path(); break; }
    if (!result.empty()) {
        std::scoped_lock lock(state.mutex);
        state.known[key] = result;
    }
    return result;
}
// Finds or creates the folder of one version. Versions known only by context
// (unidentified content, versions of an imported profile) are "Other version".
// A label given later renames the folder; failures keep the old name.
inline std::filesystem::path VersionDirectory(const std::filesystem::path& cache, std::string_view context,
                                              std::string_view label = {}) {
    const auto suffix = "(" + std::string(context.substr(0, VersionIdChars)) + ")";
    const auto wanted = VersionsDirectory(cache) / std::filesystem::u8path(
        (label.empty() ? std::string("Other version") : FilenamePart(std::string(label))) + " " + suffix);
    auto existing = FindVersionDirectory(cache, context);
    if (existing.empty()) {
        std::filesystem::create_directories(wanted);
        existing = wanted;
    } else if (!label.empty() && existing.filename() != wanted.filename()) {
        std::error_code error;
        std::filesystem::rename(existing, wanted, error);
        if (!error) existing = wanted;
    }
    auto& state = Detail::KnownVersionDirectories();
    std::scoped_lock lock(state.mutex);
    state.known[cache.string() + "|" + std::string(context)] = existing;
    return existing;
}
// The full context of a version folder, from its own files: version.json
// (identified versions) or the pipeline index header (every version).
inline std::string VersionContext(const std::filesystem::path& directory) {
    const auto id = VersionId(directory);
    if (id.empty()) return {};
    const auto read = [](const std::filesystem::path& path, size_t limit) {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > limit) return std::string{};
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), {});
    };
    std::string context;
    const auto json = read(directory / "version.json", 16384);
    constexpr std::string_view field = "\"context\":\"";
    if (const auto at = json.find(field); at != std::string::npos) context = json.substr(at + field.size(), 64);
    std::error_code error;
    for (auto it = std::filesystem::directory_iterator(directory, error);
         !ValidContext(context) && !error && it != std::filesystem::directory_iterator{}; it.increment(error)) {
        if (it->path().extension() != ".idx") continue;
        std::ifstream in(it->path(), std::ios::binary);
        std::string header(72, '\0');
        context.clear();
        if (in.read(header.data(), header.size()) && header.starts_with("NXCTX002")) context = header.substr(8);
    }
    return ValidContext(context) && context.starts_with(id) ? context : std::string{};
}
inline std::filesystem::path RecordingPath(const std::filesystem::path& cache, std::string_view context) {
    if (!ValidContext(context)) return cache / "guest-recipes.nxr";
    return VersionDirectory(cache, context) / "recipes.nxr";
}
// Context of a recording, from its version folder.
inline std::string RecordingContext(const std::filesystem::path& recording) {
    return recording.filename() == "recipes.nxr" ? VersionContext(recording.parent_path()) : std::string{};
}
} // namespace VideoCommon::ShaderProfile
