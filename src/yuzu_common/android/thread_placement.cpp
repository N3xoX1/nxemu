// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/android/thread_placement.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

#if defined(__ANDROID__) && defined(__linux__) && defined(NXEMU_ENABLE_ANDROID_ADPF)
#include <sched.h>
#include <unistd.h>
#endif

namespace Common::Android {

namespace {

constexpr int64_t PromoteThresholdPercent = 60;
constexpr int64_t DemoteThresholdPercent = 40;
constexpr uint32_t PromoteConsecutiveFrames = 3;
constexpr uint32_t DemoteConsecutiveFrames = 30;

#if defined(__ANDROID__) && defined(__linux__) && defined(NXEMU_ENABLE_ANDROID_ADPF)
uint64_t ReadSysfsUint64(const std::string& path) {
    std::ifstream file{path};
    uint64_t value = 0;
    if (file >> value) {
        return value;
    }
    return 0;
}

ThreadPlacementMasks DetectHostTopology() {
    cpu_set_t initial_mask;
    CPU_ZERO(&initial_mask);
    if (sched_getaffinity(0, sizeof(initial_mask), &initial_mask) != 0) {
        return {};
    }

    const long configured = sysconf(_SC_NPROCESSORS_CONF);
    const int max_cpus =
        configured > 0 ? std::min<int>(static_cast<int>(configured), CPU_SETSIZE) : CPU_SETSIZE;

    std::vector<int> allowed_cpus;
    for (int cpu = 0; cpu < max_cpus; ++cpu) {
        if (CPU_ISSET(cpu, &initial_mask)) {
            allowed_cpus.push_back(cpu);
        }
    }
    if (allowed_cpus.size() < 4) {
        return {};
    }

    // Prefer EAS cpu_capacity (reflects microarchitecture + frequency). Fall back to
    // cpuinfo_max_freq when cpu_capacity is unavailable.
    std::vector<CpuCoreInfo> cores;
    cores.reserve(allowed_cpus.size());
    bool all_have_capacity = true;
    for (const int cpu : allowed_cpus) {
        const uint64_t cap =
            ReadSysfsUint64("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpu_capacity");
        if (cap == 0) {
            all_have_capacity = false;
            break;
        }
        cores.push_back({cpu, cap});
    }

    if (!all_have_capacity) {
        cores.clear();
        for (const int cpu : allowed_cpus) {
            const uint64_t freq = ReadSysfsUint64("/sys/devices/system/cpu/cpu" +
                                                  std::to_string(cpu) +
                                                  "/cpufreq/cpuinfo_max_freq");
            if (freq == 0) {
                return {};
            }
            cores.push_back({cpu, freq});
        }
    }

    return ClassifyCpuTopology(cores);
}

const ThreadPlacementMasks& CachedHostTopology() {
    static const ThreadPlacementMasks masks = DetectHostTopology();
    return masks;
}

bool ApplyPosixAffinity(int32_t tid, const std::vector<int>& cpus) {
    if (cpus.empty()) {
        return false;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    for (const int cpu : cpus) {
        if (cpu >= 0 && cpu < CPU_SETSIZE) {
            CPU_SET(cpu, &set);
        }
    }
    return sched_setaffinity(static_cast<pid_t>(tid), sizeof(set), &set) == 0;
}
#else
const ThreadPlacementMasks& CachedHostTopology() {
    static const ThreadPlacementMasks empty{};
    return empty;
}

bool ApplyPosixAffinity(int32_t, const std::vector<int>&) {
    return false;
}
#endif

} // namespace

ThreadPlacementMasks ClassifyCpuTopology(const std::vector<CpuCoreInfo>& cores) {
    ThreadPlacementMasks result;
    if (cores.size() < 4) {
        return result;
    }

    std::vector<CpuCoreInfo> sorted = cores;
    std::ranges::sort(sorted, [](const CpuCoreInfo& a, const CpuCoreInfo& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.cpu_id < b.cpu_id;
    });

    if (sorted.front().score == 0 || sorted.back().score == 0 ||
        sorted.front().score == sorted.back().score) {
        return result;
    }

    struct Tier {
        uint64_t anchor_score{};
        std::vector<int> cpus;
    };
    std::vector<Tier> tiers;
    for (const auto& core : sorted) {
        // Merge cores within 5% score into the same performance tier.
        if (!tiers.empty() && core.score * 100 >= tiers.back().anchor_score * 95) {
            tiers.back().cpus.push_back(core.cpu_id);
        } else {
            tiers.push_back({core.score, {core.cpu_id}});
        }
    }

    if (tiers.size() < 2) {
        return result;
    }

    result.all_cpus.reserve(sorted.size());
    for (const auto& core : sorted) {
        result.all_cpus.push_back(core.cpu_id);
    }
    std::ranges::sort(result.all_cpus);

    if (tiers.size() >= 3) {
        // 3+ tier SoC (e.g. 1x Prime + Nx Big + Mx Little):
        // - GpuDraw gets Prime + Big (all non-Little cores).
        // - GpuDecode / VulkanWorker get Big (all middle tiers), avoiding contention
        //   on the single Prime core while staying off slow Little cores.
        std::vector<int> big_cpus;
        for (size_t i = 1; i + 1 < tiers.size(); ++i) {
            big_cpus.insert(big_cpus.end(), tiers[i].cpus.begin(), tiers[i].cpus.end());
        }
        result.draw_cpus = tiers[0].cpus;
        result.draw_cpus.insert(result.draw_cpus.end(), big_cpus.begin(), big_cpus.end());
        result.helper_cpus = big_cpus.size() >= 2 ? std::move(big_cpus) : result.draw_cpus;
    } else {
        // 2-tier SoC:
        const auto& top = tiers[0];
        const auto& bottom = tiers[1];
        if (top.cpus.size() <= 2 && bottom.cpus.size() >= 4 &&
            bottom.anchor_score * 100 >= top.anchor_score * 70) {
            // Prime + Performance layout (e.g. 2x Prime + 6x Perf):
            // Keep GpuDraw able to use Prime+Perf, steer helpers onto Perf to avoid
            // Prime contention.
            result.draw_cpus = result.all_cpus;
            result.helper_cpus = bottom.cpus;
        } else if (top.cpus.size() >= 2) {
            // Classic Big + Little layout (e.g. 4x Big + 4x Little or 2x Big + 6x Little):
            // Favor the Big cluster for all bottlenecked GPU pipeline threads.
            result.draw_cpus = top.cpus;
            result.helper_cpus = top.cpus;
        } else {
            // 1 fast core + many slow cores: never pin everything onto 1 core.
            return {};
        }
    }

    std::ranges::sort(result.draw_cpus);
    std::ranges::sort(result.helper_cpus);
    return result;
}

AdaptiveThreadPlacement::AdaptiveThreadPlacement(ThreadPlacementRole role_)
    : AdaptiveThreadPlacement(role_, CachedHostTopology(), &ApplyPosixAffinity) {}

AdaptiveThreadPlacement::AdaptiveThreadPlacement(ThreadPlacementRole role_,
                                                 ThreadPlacementMasks masks_,
                                                 ApplyAffinityFn apply_fn_)
    : role{role_}, masks{std::move(masks_)}, apply_affinity{apply_fn_} {}

const std::vector<int>& AdaptiveThreadPlacement::PreferredCpus() const {
    return role == ThreadPlacementRole::GpuDraw ? masks.draw_cpus : masks.helper_cpus;
}

void AdaptiveThreadPlacement::SetPromoted(int32_t tid, bool enable) {
    if (promoted == enable || !apply_affinity || !masks.IsEnabled()) {
        return;
    }
    const auto& target_cpus = enable ? PreferredCpus() : masks.all_cpus;
    if (target_cpus.empty() || (enable && target_cpus == masks.all_cpus)) {
        return;
    }
    if (apply_affinity(tid, target_cpus)) {
        promoted = enable;
    }
}

void AdaptiveThreadPlacement::Reset(int32_t tid) {
    busy_frames = 0;
    light_frames = 0;
    SetPromoted(tid, false);
}

void AdaptiveThreadPlacement::Update(int32_t tid, std::chrono::nanoseconds actual,
                                     std::chrono::nanoseconds budget) {
    if (!masks.IsEnabled()) {
        return;
    }
    if (budget <= std::chrono::nanoseconds{}) {
        Reset(tid);
        return;
    }
    if (actual <= std::chrono::nanoseconds{}) {
        return;
    }

    const int64_t actual_ns = actual.count();
    const int64_t budget_ns = budget.count();
    if (actual_ns * 100 >= budget_ns * PromoteThresholdPercent) {
        light_frames = 0;
        if (!promoted && ++busy_frames >= PromoteConsecutiveFrames) {
            SetPromoted(tid, true);
            busy_frames = 0;
        }
    } else if (actual_ns * 100 <= budget_ns * DemoteThresholdPercent) {
        busy_frames = 0;
        if (promoted && ++light_frames >= DemoteConsecutiveFrames) {
            SetPromoted(tid, false);
            light_frames = 0;
        }
    } else {
        busy_frames = 0;
        light_frames = 0;
    }
}

} // namespace Common::Android
