// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace VideoCore {
class ShaderNotify {
public:
    [[nodiscard]] int ShadersBuilding() noexcept;

    void MarkShaderComplete() noexcept {
        ++num_complete;
    }

    void MarkShaderBuilding() noexcept {
        ++num_building;
    }

    // The loading screen asks to start the game now: the shader warmup goes on in the background.
    void RequestBackgroundWarmup() noexcept {
        background_requested.store(true, std::memory_order_relaxed);
    }
    [[nodiscard]] bool BackgroundWarmupRequested() const noexcept {
        return background_requested.load(std::memory_order_relaxed);
    }
    // A new load starts with no request and no background warmup.
    void ResetBackgroundWarmup() noexcept {
        background_requested.store(false, std::memory_order_relaxed);
        background_total.store(0, std::memory_order_relaxed);
        background_done.store(0, std::memory_order_relaxed);
    }
    void StartBackgroundWarmup(std::uint32_t total) noexcept {
        background_done.store(0, std::memory_order_relaxed);
        background_total.store(total, std::memory_order_release);
    }
    void MarkBackgroundWarmupDone() noexcept {
        background_done.fetch_add(1, std::memory_order_relaxed);
    }
    // Pipelines the background warmup has finished, of all it was given; total is zero when there is none.
    [[nodiscard]] std::uint32_t BackgroundWarmupDone() const noexcept {
        return background_done.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint32_t BackgroundWarmupTotal() const noexcept {
        return background_total.load(std::memory_order_acquire);
    }

private:
    std::atomic_int num_building{};
    std::atomic_int num_complete{};
    std::atomic_bool background_requested{};
    std::atomic<std::uint32_t> background_total{};
    std::atomic<std::uint32_t> background_done{};
    int report_base{};

    bool completed{};
    int num_when_completed{};
    std::chrono::steady_clock::time_point complete_time;
};
} // namespace VideoCore
