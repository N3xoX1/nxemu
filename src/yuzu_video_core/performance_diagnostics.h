// Temporary opt-in timing for the validated rendering implementation.
#pragma once

#include <array>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <utility>

#include "yuzu_common/logging/log.h"

namespace VideoCommon::PerformanceDiagnostics {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("NXEMU_PERFORMANCE_DIAGNOSTICS");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

enum class Category : size_t {
    CommandList, Draw, GraphicsPipeline, GraphicsConfigure, Compute, ComputePipeline,
    ComputeConfigure, SparseCopy, GuestFlush, Finish, WaitWorker, DriverExecute,
    DriverSubmit, Composite, Present, AcquireImage, QueuePresent,
    GraphicsViews, GraphicsBuffers, GraphicsBindings, GraphicsTargets, DynamicStates,
    ReadbackPlan, ReadbackRetire, ReadbackCopy, ReadbackDeviceCopy,
    ReadbackSegments, ReadbackRuns, ReadbackSmall, ReadbackLarge,
    ReadbackStores, ReadbackSkipped, ReadbackGpu, ReadbackGpuSkipped,
    ReadbackGpuRejectedSize, ReadbackGpuRejectedBuffer, ReadbackGpuRejectedBudget, Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Category::Count)> Names{
    "commands", "draw", "graphics_pipeline", "graphics_configure", "compute",
    "compute_pipeline", "compute_configure", "sparse_copy", "guest_flush", "finish",
    "wait_worker", "driver_execute", "driver_submit", "composite", "present",
    "acquire_image", "queue_present",
    "graphics_views", "graphics_buffers", "graphics_bindings", "graphics_targets",
    "dynamic_states", "readback_plan", "readback_retire", "readback_copy",
    "readback_device_copy", "readback_segments", "readback_runs", "readback_small", "readback_large",
    "readback_stores", "readback_skipped", "readback_gpu", "readback_gpu_skipped",
    "readback_gpu_rejected_size", "readback_gpu_rejected_buffer", "readback_gpu_rejected_budget",
};

using Clock = std::chrono::steady_clock;
struct Counter {
    Clock::duration elapsed{};
    size_t calls{};
    size_t bytes{};
};
struct ThreadCounters {
    Clock::time_point begin = Clock::now();
    std::array<Counter, Names.size()> counters{};
    size_t depth{};
};
inline thread_local ThreadCounters thread_counters;

inline void RecordBytes(Category category, size_t bytes) {
    if (Enabled()) {
        thread_counters.counters[static_cast<size_t>(category)].bytes += bytes;
    }
}

inline void RecordEvent(Category category, size_t bytes = 0) {
    if (Enabled()) {
        auto& counter = thread_counters.counters[static_cast<size_t>(category)];
        ++counter.calls;
        counter.bytes += bytes;
    }
}

// Times are inclusive, overlap between nested categories, and must not be added.
// Counters are local to each thread: no lock or atomic on the per-draw path.
class Scope {
public:
    explicit Scope(Category category_) : category(category_), enabled(Enabled()) {
        if (enabled) {
            ++thread_counters.depth;
            begin = Clock::now();
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() {
        if (!enabled) {
            return;
        }
        const auto end = Clock::now();
        auto& state = thread_counters;
        auto& counter = state.counters[static_cast<size_t>(category)];
        counter.elapsed += end - begin;
        ++counter.calls;
        if (--state.depth != 0 || end - state.begin < std::chrono::seconds(2)) {
            return;
        }
        std::string totals;
        for (size_t i = 0; i < Names.size(); ++i) {
            const auto& entry = state.counters[i];
            if (entry.calls) {
                totals += fmt::format(" {}={:.3f}ms/{}", Names[i],
                    std::chrono::duration<double, std::milli>(entry.elapsed).count(), entry.calls);
                if (entry.bytes) {
                    totals += fmt::format(" {}_bytes={}", Names[i], entry.bytes);
                }
            }
        }
        LOG_INFO(Render_Vulkan, "Performance timings: thread={} interval_ms={:.3f} inclusive{}",
            std::hash<std::thread::id>{}(std::this_thread::get_id()),
            std::chrono::duration<double, std::milli>(end - state.begin).count(), totals);
        state.counters = {};
        state.begin = Clock::now();
    }

private:
    Category category;
    bool enabled;
    Clock::time_point begin{};
};

template <typename Func>
decltype(auto) Measure(Category category, Func&& func) {
    Scope scope{category};
    return std::forward<Func>(func)();
}

} // namespace VideoCommon::PerformanceDiagnostics
