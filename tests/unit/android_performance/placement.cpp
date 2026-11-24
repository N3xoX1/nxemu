// SPDX-License-Identifier: GPL-2.0-or-later

#include <cassert>
#include <chrono>
#include <vector>

#include "yuzu_common/android/thread_placement.h"

using namespace std::chrono_literals;
using Common::Android::AdaptiveThreadPlacement;
using Common::Android::ClassifyCpuTopology;
using Common::Android::CpuCoreInfo;
using Common::Android::ThreadPlacementMasks;
using Common::Android::ThreadPlacementRole;

namespace {

int apply_calls = 0;
int32_t last_tid = -1;
std::vector<int> last_cpus;

bool FakeApplyAffinity(int32_t tid, const std::vector<int>& cpus) {
    ++apply_calls;
    last_tid = tid;
    last_cpus = cpus;
    return true;
}

} // namespace

int main() {
    // 1. Snapdragon 8 Gen 2 / Gen 3 style: 1x Prime (X3) + 4x Big (A715) + 3x Little (A510).
    // GpuDraw gets Prime+Big; GpuDecode and VulkanWorker get Big only (no Prime contention).
    {
        const auto masks = ClassifyCpuTopology({
            {0, 380}, {1, 380}, {2, 380},
            {3, 760}, {4, 760}, {5, 760}, {6, 760},
            {7, 1024},
        });
        assert(masks.IsEnabled());
        assert(masks.all_cpus == (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
        assert(masks.draw_cpus == (std::vector<int>{3, 4, 5, 6, 7}));
        assert(masks.helper_cpus == (std::vector<int>{3, 4, 5, 6}));
    }

    // 2. 4-tier SoC (e.g. 1x X4 + 3x A720 high + 2x A720 mid + 2x A520 little).
    {
        const auto masks = ClassifyCpuTopology({
            {0, 320}, {1, 320},
            {2, 680}, {3, 680},
            {4, 820}, {5, 820}, {6, 820},
            {7, 1024},
        });
        assert(masks.IsEnabled());
        assert(masks.draw_cpus == (std::vector<int>{2, 3, 4, 5, 6, 7}));
        assert(masks.helper_cpus == (std::vector<int>{2, 3, 4, 5, 6}));
    }

    // 3. Snapdragon 8 Elite style: 2x Prime (4.32 GHz) + 6x Perf (3.53 GHz).
    {
        const auto masks = ClassifyCpuTopology({
            {0, 3532800}, {1, 3532800}, {2, 3532800},
            {3, 3532800}, {4, 3532800}, {5, 3532800},
            {6, 4320000}, {7, 4320000},
        });
        assert(masks.IsEnabled());
        assert(masks.draw_cpus == (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
        assert(masks.helper_cpus == (std::vector<int>{0, 1, 2, 3, 4, 5}));
    }

    // 4. Classic 4x Big + 4x Little SoC.
    {
        const auto masks = ClassifyCpuTopology({
            {0, 450}, {1, 450}, {2, 450}, {3, 450},
            {4, 1024}, {5, 1024}, {6, 1024}, {7, 1024},
        });
        assert(masks.IsEnabled());
        assert(masks.draw_cpus == (std::vector<int>{4, 5, 6, 7}));
        assert(masks.helper_cpus == (std::vector<int>{4, 5, 6, 7}));
    }

    // 5. Homogeneous or too few cores -> placement stays disabled.
    {
        assert(!ClassifyCpuTopology({{0, 1024}, {1, 1024}, {2, 1024}, {3, 1024}}).IsEnabled());
        assert(!ClassifyCpuTopology({{0, 400}, {1, 800}, {2, 1024}}).IsEnabled());
    }

    // 6. Adaptive promotion / hysteresis / demotion / hidden reset for VulkanWorker and GpuDraw.
    {
        const auto masks = ClassifyCpuTopology({
            {0, 380}, {1, 380}, {2, 380},
            {3, 760}, {4, 760}, {5, 760}, {6, 760},
            {7, 1024},
        });

        apply_calls = 0;
        AdaptiveThreadPlacement worker{ThreadPlacementRole::VulkanWorker, masks,
                                       &FakeApplyAffinity};
        // Light load never promotes and performs zero syscalls.
        for (int i = 0; i < 10; ++i) {
            worker.Update(0, 4ms, 16ms);
        }
        assert(!worker.IsPromoted() && apply_calls == 0);

        // Two heavy frames followed by a deadband frame do not promote yet.
        worker.Update(0, 12ms, 16ms);
        worker.Update(0, 12ms, 16ms);
        assert(!worker.IsPromoted() && apply_calls == 0);
        worker.Update(0, 8ms, 16ms); // 50% deadband resets consecutive busy count.
        worker.Update(0, 12ms, 16ms);
        assert(!worker.IsPromoted() && apply_calls == 0);

        // Zero duration (skipped outlier) does not alter counters or promote.
        worker.Update(0, 0ns, 16ms);
        assert(!worker.IsPromoted() && apply_calls == 0);

        // 3 consecutive heavy frames promote VulkanWorker to Big cores (excluding Prime X3).
        worker.Update(0, 12ms, 16ms);
        worker.Update(0, 12ms, 16ms);
        assert(worker.IsPromoted() && apply_calls == 1);
        assert(last_tid == 0 && last_cpus == (std::vector<int>{3, 4, 5, 6}));

        // Further heavy frames do not issue redundant sched_setaffinity syscalls.
        worker.Update(0, 14ms, 16ms);
        assert(apply_calls == 1);

        // 29 light frames keep it promoted; the 30th demotes back to all CPUs.
        for (int i = 0; i < 29; ++i) {
            worker.Update(0, 4ms, 16ms);
        }
        assert(worker.IsPromoted() && apply_calls == 1);
        worker.Update(0, 4ms, 16ms);
        assert(!worker.IsPromoted() && apply_calls == 2);
        assert(last_cpus == masks.all_cpus);

        // Promote GpuDraw (Prime+Big) and verify hidden window (0ns budget) immediately resets.
        AdaptiveThreadPlacement draw{ThreadPlacementRole::GpuDraw, masks, &FakeApplyAffinity};
        draw.Update(0, 12ms, 16ms);
        draw.Update(0, 12ms, 16ms);
        draw.Update(0, 12ms, 16ms);
        assert(draw.IsPromoted() && apply_calls == 3);
        assert(last_cpus == (std::vector<int>{3, 4, 5, 6, 7}));
        draw.Update(0, 0ns, 0ns);
        assert(!draw.IsPromoted() && apply_calls == 4);
        assert(last_cpus == masks.all_cpus);

        // Peer TID targeting for GpuDecode (Big cores, excluding Prime X3).
        AdaptiveThreadPlacement decode{ThreadPlacementRole::GpuDecode, masks, &FakeApplyAffinity};
        decode.Update(84, 12ms, 16ms);
        decode.Update(84, 12ms, 16ms);
        decode.Update(84, 12ms, 16ms);
        assert(decode.IsPromoted() && apply_calls == 5);
        assert(last_tid == 84 && last_cpus == (std::vector<int>{3, 4, 5, 6}));
        decode.Reset(84);
        assert(!decode.IsPromoted() && apply_calls == 6);
        assert(last_tid == 84 && last_cpus == masks.all_cpus);
    }

    return 0;
}
