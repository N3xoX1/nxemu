// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

namespace Common::Android {

enum class ThreadPlacementRole {
    GpuDraw,
    GpuDecode,
    VulkanWorker,
};

struct CpuCoreInfo {
    int cpu_id{};
    uint64_t score{};
};

struct ThreadPlacementMasks {
    std::vector<int> all_cpus;
    std::vector<int> draw_cpus;
    std::vector<int> helper_cpus;

    bool IsEnabled() const {
        return !all_cpus.empty() && (!draw_cpus.empty() || !helper_cpus.empty());
    }
};

// Pure topology classifier: groups online/allowed cores by capacity or max frequency
// so GpuDraw can use Prime+Big while GpuDecode/VulkanWorker favor Big without
// contending on a single Prime core (e.g. Cortex-X3/X4) or stalling on LITTLE cores.
ThreadPlacementMasks ClassifyCpuTopology(const std::vector<CpuCoreInfo>& cores);

// Adaptive affinity controller with hysteresis. Keeps threads unrestricted while
// light so EAS can save power, promotes to performance cores only when the thread
// becomes a real frame bottleneck, and demotes back once load drops or when hidden.
class AdaptiveThreadPlacement {
public:
    using ApplyAffinityFn = bool (*)(int32_t tid, const std::vector<int>& cpus);

    explicit AdaptiveThreadPlacement(ThreadPlacementRole role_);
    AdaptiveThreadPlacement(ThreadPlacementRole role_, ThreadPlacementMasks masks_,
                            ApplyAffinityFn apply_fn_);
    ~AdaptiveThreadPlacement() = default;

    AdaptiveThreadPlacement(const AdaptiveThreadPlacement&) = delete;
    AdaptiveThreadPlacement& operator=(const AdaptiveThreadPlacement&) = delete;

    // tid == 0 targets the calling thread; a positive tid targets a peer thread
    // (such as RunDecodeThread managed from RunDrawThread).
    void Update(int32_t tid, std::chrono::nanoseconds actual, std::chrono::nanoseconds budget);

    // Immediately restore unrestricted affinity if currently promoted.
    void Reset(int32_t tid);

    bool IsPromoted() const {
        return promoted;
    }

private:
    const std::vector<int>& PreferredCpus() const;
    void SetPromoted(int32_t tid, bool enable);

    ThreadPlacementRole role;
    ThreadPlacementMasks masks;
    ApplyAffinityFn apply_affinity;
    uint32_t busy_frames{};
    uint32_t light_frames{};
    bool promoted{};
};

} // namespace Common::Android
