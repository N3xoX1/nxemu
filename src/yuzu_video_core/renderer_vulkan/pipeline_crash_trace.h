// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "yuzu_common/common_types.h"
#include "yuzu_common/fs/file.h"
#include "yuzu_common/fs/fs_util.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/logging/log.h"

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
#endif

// Diagnostic trace for driver crashes inside native pipeline builds.
//
// log/pipeline_trace/<pipeline hash>/  key.bin, one .spv per stage, and status.txt
//                                      ("translating", "building", "built <ms>", "failed <ms>")
// log/pipeline_trace/thread_<id>.txt   what that worker thread is doing right now
//
// Nothing is deleted on success. After a crash, the logged "Unhandled exception ... on thread N"
// names thread_N.txt, which names the pipeline (or untraced work) that thread was running.
namespace Vulkan::PipelineCrashTrace {

using Stages = std::vector<std::pair<std::string, std::vector<u32>>>;

// Set by CreateComputePipeline so the ComputePipeline constructor can capture the key hash
// without changing its signature. The constructor runs on the same thread.
inline thread_local u64 pending_compute_hash{};

inline u64 ThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
#endif
}

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

inline void WriteText(const std::filesystem::path& path, std::string_view text) {
    WriteFile(path, std::span{reinterpret_cast<const u8*>(text.data()), text.size()});
}

inline void SetThreadState(std::string_view state, u64 hash) {
    WriteText(Root() / fmt::format("thread_{}.txt", ThreadId()),
              fmt::format("{} 0x{:016x}\n", state, hash));
}

inline void SetStatus(u64 hash, std::string_view status) {
    WriteText(Directory(hash) / "status.txt", fmt::format("{} thread {}\n", status, ThreadId()));
}

// Called when guest shader translation starts, before any SPIR-V or driver work.
inline void BeginTranslate(std::string_view kind, u64 hash) {
    std::error_code ec;
    std::filesystem::create_directories(Directory(hash), ec);
    SetStatus(hash, "translating");
    SetThreadState(fmt::format("translating {}", kind), hash);
}

inline void Record(u64 hash, std::span<const u8> key, const Stages& stages) {
    const auto dir{Directory(hash)};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    WriteFile(dir / "key.bin", key);
    for (const auto& [name, code] : stages) {
        WriteFile(dir / (name + ".spv"),
                  std::span{reinterpret_cast<const u8*>(code.data()), code.size() * sizeof(u32)});
    }
    SetStatus(hash, "queued");
    SetThreadState("queued native build", hash);
}

class NativeBuild {
public:
    NativeBuild(std::string_view kind_, u64 hash_)
        : kind{kind_}, hash{hash_}, start{std::chrono::steady_clock::now()} {
        SetStatus(hash, "building");
        SetThreadState(fmt::format("building {}", kind), hash);
        LOG_INFO(Render_Vulkan, "Native {} pipeline build begin 0x{:016x} thread {}", kind, hash,
                 ThreadId());
    }

    void Finish(bool success) {
        const auto ms{std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count()};
        SetStatus(hash, fmt::format("{} {}ms", success ? "built" : "failed", ms));
        SetThreadState(fmt::format("idle after {}", kind), hash);
        LOG_INFO(Render_Vulkan, "Native {} pipeline build end 0x{:016x} thread {} {} {}ms", kind,
                 hash, ThreadId(), success ? "built" : "failed", ms);
    }

private:
    std::string kind;
    u64 hash;
    std::chrono::steady_clock::time_point start;
};

} // namespace Vulkan::PipelineCrashTrace
