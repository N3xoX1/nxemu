#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifndef NXEMU_ENABLE_PERF_CAPTURE_INSTRUMENTATION
#define NXEMU_ENABLE_PERF_CAPTURE_INSTRUMENTATION 1
#endif

struct PerformanceCaptureConfig
{
    int32_t renderer_backend{};
    int32_t shader_backend{};
    int32_t gpu_accuracy{};
    int32_t dma_accuracy{};
    int32_t vsync_mode{};
    int32_t vulkan_device{};
    int32_t cpu_backend{};
    int32_t cpu_accuracy{};
    int32_t docked_mode{};
    int32_t memory_layout{};
    int32_t speed_limit{};
    int32_t astc_decode_mode{};
    int32_t nvdec_emulation{};
    int32_t resolution_setup{};
    int32_t scaling_filter{};
    int32_t anti_aliasing{};
    int32_t anisotropic_filtering{};
    int32_t astc_recompression{};
    int32_t vram_usage_mode{};
    int32_t fsr_sharpness{};
    float resolution_factor{1.0f};
    uint64_t cpu_options_mask{};
    bool nce_enabled{};
    bool sync_memory_operations{};
    bool use_speed_limit{};
    bool use_multi_core{};
    bool async_gpu{};
    bool async_presentation{};
    bool async_shader_building{};
    bool disk_pipeline_cache{};
    bool vulkan_pipeline_cache{};
    bool force_maximum_clocks{};
    bool reactive_flushing{};
    bool fast_gpu_time{};
    bool sync_to_video_framerate{};
    bool barrier_feedback_loops{};
};

// Extend this list for a PR; names are exported without adding hot-path formatting.
#define NXEMU_PERF_CAPTURE_EXPERIMENTAL_COUNTERS(X)
#define NXEMU_PERF_CAPTURE_STABLE_COUNTERS(X) \
    X(smo_requests) X(smo_stub_inline) X(smo_stub_queued) X(smo_real_fences) \
    X(fence_should_flush_true) X(fence_should_flush_false) X(fence_flush_requests) \
    X(fence_queue_depth_max) X(scheduler_flushes) X(scheduler_finishes) \
    X(scheduler_submissions) X(fence_queue_scheduler_flushes) X(scheduler_forced_flushes) \
    X(dma_safe_reads) X(dma_safe_read_requested_bytes) \
    X(flush_region_requests) X(flush_region_requested_bytes) \
    X(buffer_download_requests) X(buffer_download_requested_bytes) \
    X(texture_download_requests) X(texture_download_requested_bytes) X(query_flush_requests) \
    X(graphics_pipeline_requests) X(compute_pipeline_requests) X(pipeline_prepare_failures) \
    X(pipeline_native_completions) X(pipeline_native_exceptions) X(pipeline_async_skipped_draws) \
    X(present_queue_depth_max) X(swapchain_recreations)
#define NXEMU_PERF_CAPTURE_COUNTERS(X) \
    NXEMU_PERF_CAPTURE_STABLE_COUNTERS(X) \
    NXEMU_PERF_CAPTURE_EXPERIMENTAL_COUNTERS(X)

#define NXEMU_PERF_CAPTURE_TIMINGS(X) \
    X(fence_wait_call) X(scheduler_wait_call) X(scheduler_worker_wait_call) \
    X(flush_region_call) X(pipeline_prepare) X(pipeline_queue) X(pipeline_native_build) \
    X(pipeline_consumer_wait) X(present_free_queue_wait) X(present_fence_wait_call) \
    X(gpu_command_wait) X(host_sync_wait_call)

enum class PerformanceCounter : uint32_t {
#define PERF_ENUM(name) name,
    NXEMU_PERF_CAPTURE_COUNTERS(PERF_ENUM)
    Count
};
enum class PerformanceTiming : uint32_t {
    NXEMU_PERF_CAPTURE_TIMINGS(PERF_ENUM)
    Count
};
#undef PERF_ENUM

enum class PerformanceFrameStream : uint32_t {
    Composite,
    GameFrame,
    Count,
};

enum class PerformanceInvalidation : uint32_t {
    Paused = 1, ConfigurationChanged = 2, Hidden = 4, Shutdown = 8
};

struct PerformanceDuration {
    std::atomic<uint64_t> count{}, total_ns{}, max_ns{}, over_1ms{}, over_5ms{};
};

inline void PerformanceCaptureAtomicMax(std::atomic<uint64_t>& target, uint64_t value) {
    auto old = target.load(std::memory_order_relaxed);
    while (old < value && !target.compare_exchange_weak(old, value, std::memory_order_relaxed)) {}
}

// Owned by the OS. Video keeps a non-owning pointer for the OS lifetime.
struct PerformanceCaptureSharedState {
    using Clock = std::chrono::steady_clock;
    static constexpr size_t MaxFrames = 216000;
    static constexpr uint64_t Closed = uint64_t{1} << 63;

    struct Frames {
        std::mutex mutex;
        std::vector<uint64_t> timestamps_ns;
        uint64_t events{}, dropped{}, first_ns{}, last_ns{};
    };
    struct Data {
        std::array<std::atomic<uint64_t>, static_cast<size_t>(PerformanceCounter::Count)> counters{};
        std::array<PerformanceDuration, static_cast<size_t>(PerformanceTiming::Count)> timings{};
        std::array<Frames, static_cast<size_t>(PerformanceFrameStream::Count)> frames;
        std::atomic<uint64_t> in_flight{}, memory_samples{}, memory_first{}, memory_last{}, memory_peak{};
        std::atomic<uint64_t> process_memory_samples{}, process_working_set_first{},
                              process_working_set_last{}, process_working_set_peak{},
                              process_private_first{}, process_private_last{},
                              process_private_peak{};
        std::atomic<uint32_t> invalidations{};
        Clock::time_point start{};
    };

    // A writer covers ONLY instrumentation updates, never emulation work, driver calls or waits.
    class Writer {
    public:
        Writer() = default;
        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;
        Writer(Writer&& other) noexcept : state(std::exchange(other.state, nullptr)) {}
        ~Writer() {
            if (state && state->gate.fetch_sub(1, std::memory_order_release) == Closed + 1) {
                state->gate.notify_all();
            }
        }
        explicit operator bool() const { return state != nullptr; }
        Data* operator->() const { return state->data.get(); }
        void Add(PerformanceCounter metric, uint64_t value = 1) const {
            state->data->counters[static_cast<size_t>(metric)].fetch_add(value, std::memory_order_relaxed);
        }
        void Max(PerformanceCounter metric, uint64_t value) const {
            PerformanceCaptureAtomicMax(state->data->counters[static_cast<size_t>(metric)], value);
        }
        void Duration(PerformanceTiming metric, uint64_t ns) const {
            auto& d = state->data->timings[static_cast<size_t>(metric)];
            d.count.fetch_add(1, std::memory_order_relaxed);
            d.total_ns.fetch_add(ns, std::memory_order_relaxed);
            PerformanceCaptureAtomicMax(d.max_ns, ns);
            if (ns > 1'000'000) d.over_1ms.fetch_add(1, std::memory_order_relaxed);
            if (ns > 5'000'000) d.over_5ms.fetch_add(1, std::memory_order_relaxed);
        }
    private:
        friend struct PerformanceCaptureSharedState;
        explicit Writer(PerformanceCaptureSharedState* s) : state(s) {}
        PerformanceCaptureSharedState* state{};
    };

    uint64_t Epoch() const noexcept {
#if NXEMU_ENABLE_PERF_CAPTURE_INSTRUMENTATION
        return active_epoch.load(std::memory_order_acquire);
#else
        return 0;
#endif
    }
    Writer Write(uint64_t epoch) {
        if (!epoch) return {};
        auto value = gate.load(std::memory_order_relaxed);
        while (!(value & Closed)) {
            if (gate.compare_exchange_weak(value, value + 1, std::memory_order_acquire)) {
                Writer writer(this);
                if (active_epoch.load(std::memory_order_acquire) == epoch) return writer;
                return {};
            }
        }
        return {};
    }
    // Lifecycle calls are serialized by the recorder. No new epoch until Close has drained writers.
    void Begin() {
        auto next = std::make_unique<Data>();
        for (auto& f : next->frames) f.timestamps_ns.reserve(MaxFrames);
        next->start = Clock::now();
        data = std::move(next);
        gate.store(0, std::memory_order_release);
        active_epoch.store(++generation, std::memory_order_release);
    }
    Clock::time_point Close() {
        active_epoch.store(0, std::memory_order_release);
        auto value = gate.fetch_or(Closed, std::memory_order_acq_rel) | Closed;
        while (value != Closed) {
            gate.wait(value, std::memory_order_acquire);
            value = gate.load(std::memory_order_acquire);
        }
        return Clock::now();
    }
    void Frame([[maybe_unused]] PerformanceFrameStream stream) {
#if NXEMU_ENABLE_PERF_CAPTURE_INSTRUMENTATION
        if (auto w = Write(Epoch())) {
            auto& f = w->frames[static_cast<size_t>(stream)];
            std::scoped_lock lock(f.mutex);
            const auto ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - w->start).count());
            if (f.events++ == 0) f.first_ns = ns;
            f.last_ns = ns;
            if (f.timestamps_ns.size() < MaxFrames) f.timestamps_ns.push_back(ns);
            else ++f.dropped;
        }
#endif
    }
    void Invalidate(PerformanceInvalidation reason) {
        if (auto w = Write(Epoch())) w->invalidations.fetch_or(static_cast<uint32_t>(reason), std::memory_order_relaxed);
    }
    void Memory(uint64_t bytes) {
        if (auto w = Write(Epoch())) {
            if (w->memory_samples.fetch_add(1, std::memory_order_relaxed) == 0)
                w->memory_first.store(bytes, std::memory_order_relaxed);
            w->memory_last.store(bytes, std::memory_order_relaxed);
            PerformanceCaptureAtomicMax(w->memory_peak, bytes);
        }
    }
    void ProcessMemory(uint64_t working_set_bytes, uint64_t private_bytes) {
        if (auto w = Write(Epoch())) {
            if (w->process_memory_samples.fetch_add(1, std::memory_order_relaxed) == 0) {
                w->process_working_set_first.store(working_set_bytes, std::memory_order_relaxed);
                w->process_private_first.store(private_bytes, std::memory_order_relaxed);
            }
            w->process_working_set_last.store(working_set_bytes, std::memory_order_relaxed);
            w->process_private_last.store(private_bytes, std::memory_order_relaxed);
            PerformanceCaptureAtomicMax(w->process_working_set_peak, working_set_bytes);
            PerformanceCaptureAtomicMax(w->process_private_peak, private_bytes);
        }
    }
    // Renderer sets these before gameplay; exporter copies under this mutex.
    void SetDevice(const char* model, const char* driver) {
        std::scoped_lock lock(device_mutex);
        gpu_model = model ? model : "";
        gpu_driver = driver ? driver : "";
    }
    std::mutex device_mutex;
    std::string gpu_model, gpu_driver;
    std::unique_ptr<Data> data; // Read only after Close, while the recorder lifecycle lock is held.
private:
    std::atomic<uint64_t> active_epoch{}, gate{Closed};
    uint64_t generation{};
};

inline PerformanceCaptureSharedState::Writer PerformanceCaptureWrite(PerformanceCaptureSharedState& state) {
    return state.Write(state.Epoch());
}

struct PerformanceCaptureStamp {
#if NXEMU_ENABLE_PERF_CAPTURE_INSTRUMENTATION
    uint64_t epoch{};
    PerformanceCaptureSharedState::Clock::time_point time{};
    explicit PerformanceCaptureStamp(PerformanceCaptureSharedState& state) : epoch(state.Epoch()) {
        if (auto w = state.Write(epoch)) {
            w->in_flight.fetch_add(1, std::memory_order_relaxed);
            time = PerformanceCaptureSharedState::Clock::now();
        } else {
            epoch = 0;
        }
    }
    void End(PerformanceCaptureSharedState& state) const {
        if (auto w = state.Write(epoch)) {
            w.Duration(PerformanceTiming::pipeline_queue,
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    PerformanceCaptureSharedState::Clock::now() - time).count()));
            w->in_flight.fetch_sub(1, std::memory_order_relaxed);
        }
    }
#else
    PerformanceCaptureStamp() = default;
#endif
};

// Begin/end must belong to the same epoch. Crossing operations remain in in_flight at closure;
// their durations are deliberately not attributed to either adjacent capture.
class PerformanceCaptureTimer {
public:
    PerformanceCaptureTimer(PerformanceCaptureSharedState& state_, PerformanceTiming metric_, bool native_build_ = false)
        : state(state_), metric(metric_), native_build(native_build_), epoch(state.Epoch()) {
        if (auto w = state.Write(epoch)) {
            if (native_build) exceptions = std::uncaught_exceptions();
            w->in_flight.fetch_add(1, std::memory_order_relaxed);
            start = PerformanceCaptureSharedState::Clock::now();
        } else epoch = 0;
    }
    PerformanceCaptureTimer(const PerformanceCaptureTimer&) = delete;
    ~PerformanceCaptureTimer() {
        if (auto w = state.Write(epoch)) {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                PerformanceCaptureSharedState::Clock::now() - start).count();
            w.Duration(metric, static_cast<uint64_t>(ns));
            if (native_build) w.Add(std::uncaught_exceptions() > exceptions ? PerformanceCounter::pipeline_native_exceptions : PerformanceCounter::pipeline_native_completions);
            w->in_flight.fetch_sub(1, std::memory_order_relaxed);
        }
    }
private:
    PerformanceCaptureSharedState& state;
    PerformanceTiming metric;
    bool native_build{};
    int exceptions{};
    uint64_t epoch{};
    PerformanceCaptureSharedState::Clock::time_point start{};
};

#define PERF_JOIN_I(a, b) a##b
#define PERF_JOIN(a, b) PERF_JOIN_I(a, b)
#if NXEMU_ENABLE_PERF_CAPTURE_INSTRUMENTATION
#define PERF_CAPTURE_STAMP(state) PerformanceCaptureStamp((state))
#define PERF_CAPTURE_STAMP_END(stamp, state) (stamp).End((state))
#define PERF_CAPTURE_EPOCH(state) (state).Epoch()
#define PERF_CAPTURE_WRITE(state) PerformanceCaptureWrite((state))
#define PERF_CAPTURE_WRITE_EPOCH(state, epoch) (state).Write((epoch))
#define PERF_CAPTURE_MEMORY(state, bytes) (state).Memory(bytes)
#define PERF_CAPTURE_PROCESS_MEMORY(state, working_set_bytes, private_bytes) \
    (state).ProcessMemory((working_set_bytes), (private_bytes))
#define PERF_CAPTURE_FRAME(state, stream) (state).Frame(stream)
#define PERF_CAPTURE_INVALIDATE(state, reason) (state).Invalidate(reason)
#define PERF_CAPTURE_BUILD(state) PerformanceCaptureTimer PERF_JOIN(perf_build_, __LINE__)(state, PerformanceTiming::pipeline_native_build, true)
#define PERF_CAPTURE_SCOPE(state, metric) PerformanceCaptureTimer PERF_JOIN(perf_timer_, __LINE__)(state, PerformanceTiming::metric)
#define PERF_CAPTURE_ADD(state, metric, value) do { if (auto w = PerformanceCaptureWrite((state))) w.Add(PerformanceCounter::metric, static_cast<uint64_t>(value)); } while (false)
#define PERF_CAPTURE_MAX(state, metric, value) do { if (auto w = PerformanceCaptureWrite((state))) w.Max(PerformanceCounter::metric, static_cast<uint64_t>(value)); } while (false)
#else
#define PERF_CAPTURE_STAMP(state) PerformanceCaptureStamp{}
#define PERF_CAPTURE_STAMP_END(stamp, state) do {} while (false)
#define PERF_CAPTURE_EPOCH(state) uint64_t{0}
#define PERF_CAPTURE_WRITE(state) PerformanceCaptureSharedState::Writer{}
#define PERF_CAPTURE_WRITE_EPOCH(state, epoch) PerformanceCaptureSharedState::Writer{}
#define PERF_CAPTURE_MEMORY(state, bytes) do {} while (false)
#define PERF_CAPTURE_PROCESS_MEMORY(state, working_set_bytes, private_bytes) do {} while (false)
#define PERF_CAPTURE_FRAME(state, stream) do {} while (false)
#define PERF_CAPTURE_INVALIDATE(state, reason) do {} while (false)
#define PERF_CAPTURE_BUILD(state) do {} while (false)
#define PERF_CAPTURE_SCOPE(state, metric) do {} while (false)
#define PERF_CAPTURE_ADD(state, metric, value) do {} while (false)
#define PERF_CAPTURE_MAX(state, metric, value) do {} while (false)
#endif
