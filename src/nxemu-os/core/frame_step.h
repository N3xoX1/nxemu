// SPDX-FileCopyrightText: Copyright 2026 Valiantsin Dzerakh, Copyright 2026 MacNativeGames contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

#include "yuzu_common/common_types.h"

namespace Core::Memory {
class Memory;
}

namespace Core {

// The measured frame step, for patch sets that make a game step by real time instead of whole vsyncs. The loader
// names the patch page of each module whose applied sets read it (nxemu-loader core/file_sys/patch_sets.h); at every
// present this writes three floats at each: +0 the step in 30 Hz frames, +4 the step in seconds, +8 the step in 60 Hz
// frames. The step is the average of the last five display intervals, at most 1/15 s (a slower frame slows the
// game down, as on the console) and at least 1/240 s.
class FrameStep
{
public:
    // A new process: the address belongs to the modules the loader maps next
    void Reset();
    // Adds a module's patch page and stores the current step there, which the game reads before its first present
    void AddAddress(Memory::Memory & memory, u64 address);
    [[nodiscard]] bool Active() const;
    void OnPresent(Memory::Memory & memory);
    // The fewest vsyncs a frame takes: 2 while a video plays and a patch set reads the step, else 1. A game may show
    // one frame of a 30 fps movie per game frame, as Tears of the Kingdom does with its pre-rendered cutscenes, so above
    // 30 fps the movie plays too fast. Held at 30 fps it plays at its own speed, and the measured step keeps the rest
    // of the game at its speed.
    [[nodiscard]] s32 MinSwapInterval(bool video_playing);

private:
    static bool Write(Memory::Memory & memory, u64 address, f32 seconds);
    void Store(Memory::Memory & memory, f32 seconds);

    std::atomic<bool> m_active{false};
    std::atomic<bool> m_video_hold{false};
    std::mutex m_mutex;
    std::vector<u64> m_addresses;
    bool m_has_last{};
    std::chrono::steady_clock::time_point m_last{};
    std::array<f64, 5> m_samples{};
    u32 m_count{};
    u32 m_next{};
    f32 m_step{1.0f / 60.0f};
    // The report every 10 s: frames shown and game seconds stepped against wall seconds
    std::chrono::steady_clock::time_point m_report_start{};
    u32 m_report_frames{};
    f64 m_report_stepped{};
};

} // namespace Core
