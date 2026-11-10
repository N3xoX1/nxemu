// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <span>
#include <string>
#include <vector>
#include <set>
#include "yuzu_common/common_types.h"
#include "yuzu_video_core/shared_shader_bytes.h"

namespace VideoCommon::CacheStore {
// The shader ABI of the Vulkan renderer's local store: the renderer writes it and profile export reads it.
// Global-memory helper changes require a new one.
inline constexpr u32 VulkanShaderAbi = 28;
// Local cache only: code, environments and pipeline combinations have separate identities.
// All lengths are explicit little endian; shader ABI is independent of the storage revision.
struct Record {
    std::vector<SharedShaderBytes> environments;
    std::string key;
    std::string id; // Verified frame identity when read from disk.
};
struct ReadLimits {
    size_t index_bytes{size_t{1024} * 1024 * 1024};
    size_t record_bytes{512 * 1024 * 1024};
};
std::vector<Record> Read(const std::filesystem::path& path, u32 shader_abi,
                          const ReadLimits& limits = {}, std::set<std::string>* record_ids = nullptr);
// generated: a record of a synthetic recipe, associated with context through its generated index only.
void Append(const std::filesystem::path& path, u32 shader_abi, const Record& record,
            std::string* record_id = nullptr, std::string_view context = {}, bool generated = false);
// For the length of an import on Windows: the store and the version indexes stay open between records, as a virus
// scanner checks a file each time it closes. They deny writing to everyone else, so they need no check between
// records either, and an append that cannot open them throws. Elsewhere this changes nothing.
struct AppendSession {
    AppendSession();
    ~AppendSession();
    AppendSession(const AppendSession&) = delete;
    AppendSession& operator=(const AppendSession&) = delete;
};
// Associate an already verified record without retaining or rehashing its code.
// Returns false if the record disappeared (for example after cache truncation).
bool AssociateContext(const std::filesystem::path& path, u32 shader_abi,
                      const std::string& record_id, std::string_view context, bool generated = false);
// The records of a version: recorded ones, or with generated those of synthetic recipes.
std::set<std::string> ReadContext(const std::filesystem::path& path, std::string_view context, bool generated = false);
// While loading, never while playing: regroups the frames appended since the
// last call into compressed blocks once they reach min_loose_bytes. Returns true
// when the file was rewritten.
bool Compact(const std::filesystem::path& path, u32 shader_abi, size_t min_loose_bytes = 1024 * 1024);
// Where the associations of one version are kept; empty when it has none yet.
std::filesystem::path ContextIndexPath(const std::filesystem::path& path, std::string_view context);
} // namespace VideoCommon::CacheStore
