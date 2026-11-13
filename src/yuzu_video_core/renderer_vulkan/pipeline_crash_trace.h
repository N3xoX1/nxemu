// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "yuzu_common/common_types.h"
#include "yuzu_common/fs/file.h"
#include "yuzu_common/fs/fs_util.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/logging/log.h"

namespace Vulkan::PipelineCrashTrace {

using Stages = std::vector<std::pair<std::string, std::vector<u32>>>;

inline std::filesystem::path Root() {
    static const std::filesystem::path root{[] {
        const auto log_dir{Common::FS::GetYuzuPath(Common::FS::YuzuPath::LogDir)};
        auto path{log_dir / "pipeline_trace"};
        auto old_path{log_dir / "pipeline_trace.old"};
        std::error_code ec;
        std::filesystem::remove_all(old_path, ec);
        if (std::filesystem::exists(path, ec)) {
            std::filesystem::rename(path, old_path, ec);
        }
        std::filesystem::create_directories(path, ec);
        LOG_INFO(Render_Vulkan, "Pipeline crash trace enabled: {}",
                     Common::FS::PathToUTF8String(path));
        return path;
    }()};
    return root;
}

inline std::filesystem::path Directory(u64 hash) {
    return Root() / fmt::format("{:016x}", hash);
}

inline void WriteFile(const std::filesystem::path& path, std::span<const u8> data) {
    Common::FS::IOFile file(path, Common::FS::FileAccessMode::Write,
                            Common::FS::FileType::BinaryFile);
    if (file.IsOpen()) {
        static_cast<void>(file.WriteSpan(data));
        static_cast<void>(file.Flush());
    }
}

template <typename Key>
inline void Record(u64 hash, const Key& key, const Stages& stages) {
    const auto dir{Directory(hash)};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    WriteFile(dir / "key.bin",
              std::span{reinterpret_cast<const u8*>(&key), key.Size()});
    for (const auto& [name, code] : stages) {
        WriteFile(dir / (name + ".spv"),
                  std::span{reinterpret_cast<const u8*>(code.data()), code.size() * sizeof(u32)});
    }
}

inline void BeginNative(u64 hash) {
    static constexpr std::string_view marker{"building\n"};
    WriteFile(Directory(hash) / "building.txt",
              std::span{reinterpret_cast<const u8*>(marker.data()), marker.size()});
    LOG_INFO(Render_Vulkan, "Native graphics pipeline build begin 0x{:016x}", hash);
}

inline void EndNative(u64 hash, bool success) {
    if (success) {
        std::error_code ec;
        std::filesystem::remove_all(Directory(hash), ec);
    }
}

} // namespace Vulkan::PipelineCrashTrace
