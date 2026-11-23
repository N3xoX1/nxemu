// SPDX-FileCopyrightText: Copyright 2026 Valiantsin Dzerakh, Copyright 2026 MacNativeGames contributors
// SPDX-License-Identifier: MIT
//
// Adapted from totk-pc-recomp runtime/frame_rate.c frame_step_present (MIT), itself adapted from
// MacNativeGames games/totk/runtime/game_patches.c runtime_frame_step_update (MIT).

#include <algorithm>
#include <bit>
#include <cstring>

#include "core/frame_step.h"
#include "core/memory.h"
#include "yuzu_common/logging/log.h"

namespace Core {

namespace {

constexpr f64 StepMaxS = 1.0 / 15.0;
constexpr f64 PauseS = 0.25;
constexpr f64 StepFirstS = 1.0 / 60.0;
// Tears of the Kingdom's physics runs out of memory with shorter steps
constexpr f64 StepMinS = 1.0 / 240.0;
constexpr f64 ReportS = 10.0;
constexpr u32 GpuCompletedOffset = 16;
constexpr u32 GpuSubmittedOffset = 20;

} // namespace

void FrameStep::Reset()
{
    std::scoped_lock lock{m_mutex};
    m_addresses.clear();
    m_pages.clear();
    // Not the GPU counts: a list submitted before the reset can still complete after it
    m_active.store(false, std::memory_order_relaxed);
    m_step = static_cast<f32>(StepFirstS);
    m_video_playing = false;
    m_has_last = false;
    m_count = m_next = 0;
    m_report_frames = 0;
}

void FrameStep::AddAddress(Memory::Memory & memory, u64 address)
{
    std::scoped_lock lock{m_mutex};
    if (std::find(m_addresses.begin(), m_addresses.end(), address) != m_addresses.end())
    {
        return;
    }
    // Now rather than at the next present: the game reads the step before its first present
    if (!Write(memory, address, m_step, m_video_playing))
    {
        LOG_ERROR(Core, "Frame step: {:#x} is not mapped; nothing is written there", address);
        return;
    }
    m_addresses.push_back(address);
    if (u8 * const page = memory.GetPointer(address))
    {
        m_pages.push_back(page);
        const u32 completed = m_gpu_completed.load(std::memory_order_relaxed);
        const u32 submitted = m_gpu_submitted.load(std::memory_order_relaxed);
        std::memcpy(page + GpuCompletedOffset, &completed, sizeof(completed));
        std::memcpy(page + GpuSubmittedOffset, &submitted, sizeof(submitted));
    }
    m_active.store(true, std::memory_order_relaxed);
    LOG_INFO(Core, "Frame step at {:#x}", address);
}

bool FrameStep::Active() const
{
    return m_active.load(std::memory_order_relaxed);
}

void FrameStep::OnGpuListSubmitted()
{
    StoreGpuCount(GpuSubmittedOffset, m_gpu_submitted.fetch_add(1, std::memory_order_relaxed) + 1);
}

void FrameStep::OnGpuListCompleted()
{
    StoreGpuCount(GpuCompletedOffset, m_gpu_completed.fetch_add(1, std::memory_order_relaxed) + 1);
}

// The GPU thread calls this, so it writes the pages' host memory rather than going through Memory
void FrameStep::StoreGpuCount(u32 offset, u32 count)
{
    std::scoped_lock lock{m_mutex};
    for (u8 * const page : m_pages)
    {
        std::memcpy(page + offset, &count, sizeof(count));
    }
}

void FrameStep::OnPresent(Memory::Memory & memory, bool video_playing)
{
    if (!Active())
    {
        return;
    }
    std::scoped_lock lock{m_mutex};
    if (video_playing != m_video_playing)
    {
        m_video_playing = video_playing;
        LOG_INFO(Core, video_playing ? "Frame step: a video plays" : "Frame step: the video ended");
        Store(memory, m_step);
    }
    const auto now = std::chrono::steady_clock::now();
    if (m_has_last)
    {
        f64 dt = std::chrono::duration<f64>(now - m_last).count();
        if (dt > PauseS || dt < 0.001)
        {
            m_count = m_next = 0;
        }
        else
        {
            m_samples[m_next++ % m_samples.size()] = std::min(dt, StepMaxS);
            m_count = std::min<u32>(m_count + 1, static_cast<u32>(m_samples.size()));
            f64 sum = 0;
            for (u32 i = 0; i < m_count; i++)
            {
                sum += m_samples[i];
            }
            Store(memory, static_cast<f32>(std::max(sum / m_count, StepMinS)));
        }
    }
    m_last = now;
    m_has_last = true;

    if (m_report_frames == 0)
    {
        m_report_start = now;
        m_report_stepped = 0;
    }
    m_report_frames++;
    m_report_stepped += m_step;
    const f64 wall = std::chrono::duration<f64>(now - m_report_start).count();
    if (wall >= ReportS)
    {
        LOG_INFO(Core, "Frame step: {} frames in {:.2f} s ({:.1f} fps), {:.2f} s stepped (speed {:.3f}), step now {:.2f} ms",
                 m_report_frames - 1, wall, (m_report_frames - 1) / wall, m_report_stepped, m_report_stepped / wall, m_step * 1000.0f);
        m_report_frames = 0;
    }
}

// Two 64-bit stores: the game reads the values on its own threads, and each stays finite and in range whichever store
// it sees.
bool FrameStep::Write(Memory::Memory & memory, u64 address, f32 seconds, bool video_playing)
{
    if (!memory.IsValidVirtualAddressRange(address, 16))
    {
        return false;
    }
    const f32 ratio = seconds * 30.0f, interval = seconds * 60.0f;
    memory.Write64(address, static_cast<u64>(std::bit_cast<u32>(ratio)) | static_cast<u64>(std::bit_cast<u32>(seconds)) << 32);
    memory.Write64(address + 8, static_cast<u64>(std::bit_cast<u32>(interval)) | static_cast<u64>(video_playing ? 1 : 0) << 32);
    return true;
}

void FrameStep::Store(Memory::Memory & memory, f32 seconds)
{
    for (const u64 address : m_addresses)
    {
        Write(memory, address, seconds, m_video_playing);
    }
    m_step = seconds;
}

} // namespace Core
