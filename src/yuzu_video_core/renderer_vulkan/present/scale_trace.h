// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_video_core/vulkan_common/vulkan.h"
#include <chrono>
#include <cstdint>
#include <vector>
#include <cstdlib>
#include <filesystem>
#include <fstream>
namespace Vulkan::ScaleTrace
{
inline std::ofstream output;
inline unsigned remaining{}, draw{};
inline std::uint64_t capture_fragment{};
inline std::filesystem::path trace_prefix;
struct CaptureInput {
    unsigned slot;
    VkImage image;
    VkImageView view;
};
inline std::vector<CaptureInput> capture_inputs;
inline bool capture_pending{};
inline const char * Directory()
{
    static const char * path = std::getenv("NXEMU_SCALE_TRACE");
    return path;
}
inline bool Active()
{
    return remaining != 0;
}
inline void FrameEnd()
{
    if (!Directory()) return;
    if (remaining && --remaining == 0) output.close();
    static auto next = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now < next) return;
    next = now + std::chrono::milliseconds(250);
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(std::filesystem::path(Directory()) / "trace.request", error);
    static auto previous = std::filesystem::file_time_type::min();
    if (error || stamp == previous) return;
    previous = stamp;
    if (output.is_open()) output.close();
    const auto id = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    trace_prefix = std::filesystem::path(Directory()) / ("trace-" + std::to_string(id));
    output.open(trace_prefix.string() + ".txt");
    capture_fragment = 0;
    std::ifstream selector(std::filesystem::path(Directory()) / "capture-draw.request");
    selector >> std::hex >> capture_fragment;
    remaining = 2;
    draw = 0;
}
} // namespace Vulkan::ScaleTrace
