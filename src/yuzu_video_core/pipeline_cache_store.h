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
void Append(const std::filesystem::path& path, u32 shader_abi, const Record& record,
            std::string* record_id = nullptr, std::string_view context = {});
// Associate an already verified record without retaining or rehashing its code.
// Returns false if the record disappeared (for example after cache truncation).
bool AssociateContext(const std::filesystem::path& path, u32 shader_abi,
                      const std::string& record_id, std::string_view context);
std::set<std::string> ReadContext(const std::filesystem::path& path, std::string_view context);
// While loading, never while playing: regroups the frames appended since the
// last call into compressed blocks once they reach min_loose_bytes. Returns true
// when the file was rewritten.
bool Compact(const std::filesystem::path& path, u32 shader_abi, size_t min_loose_bytes = 1024 * 1024);
// Where the associations of one version are kept; empty when it has none yet.
std::filesystem::path ContextIndexPath(const std::filesystem::path& path, std::string_view context);
} // namespace VideoCommon::CacheStore
