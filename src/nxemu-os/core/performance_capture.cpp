// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/performance_capture.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <numeric>
#include <sstream>
#include <stdexcept>
#if NXEMU_ENABLE_PERF_CAPTURE_INSTRUMENTATION && defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif
#include <fmt/format.h>
#include <nxemu-os/version.h>
#include <nxemu-module-spec/video.h>
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/logging/log.h"

namespace {

constexpr std::size_t FrameStreamCount = static_cast<std::size_t>(PerformanceFrameStream::Count);
constexpr std::array<const char*, FrameStreamCount> FrameStreamNames{
    "composite",
    "game_frame",
};

struct FrameSummary {
    std::size_t count{};
    double mean_ms{};
    double median_ms{};
    double p95_ms{};
    double p99_ms{};
    double p999_ms{};
    double one_percent_low_fps{};
    double point_one_percent_low_fps{};
    double worst_ms{};
    std::size_t over_25_ms{};
    std::size_t over_33_ms{};
    std::size_t over_50_ms{};
    std::size_t over_100_ms{};
    std::size_t over_250_ms{};
    std::size_t over_1_5x{};
    std::size_t over_2x{};
    std::size_t over_3x{};
    double stutter_total_ms{};
    double stutter_excess_ms{};
};

double Percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double pos = p * static_cast<double>(sorted.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(pos));
    const auto hi = static_cast<std::size_t>(std::ceil(pos));
    if (lo == hi) {
        return sorted[lo];
    }
    const double fraction = pos - static_cast<double>(lo);
    return sorted[lo] + (sorted[hi] - sorted[lo]) * fraction;
}

double LowFps(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    const std::size_t sample_count =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(sorted.size() * fraction)));
    const double total = std::accumulate(sorted.end() - sample_count, sorted.end(), 0.0);
    const double average_ms = total / static_cast<double>(sample_count);
    return average_ms > 0.0 ? 1000.0 / average_ms : 0.0;
}

FrameSummary SummarizeFrames(const std::vector<double>& raw_values) {
    if (raw_values.empty()) {
        return {};
    }

    std::vector<double> sorted = raw_values;
    std::sort(sorted.begin(), sorted.end());

    FrameSummary out{};
    out.count = sorted.size();
    out.mean_ms = std::accumulate(sorted.begin(), sorted.end(), 0.0) /
                  static_cast<double>(sorted.size());
    out.median_ms = Percentile(sorted, 0.5);
    out.p95_ms = Percentile(sorted, 0.95);
    out.p99_ms = Percentile(sorted, 0.99);
    out.p999_ms = Percentile(sorted, 0.999);
    out.one_percent_low_fps = LowFps(sorted, 0.01);
    out.point_one_percent_low_fps = LowFps(sorted, 0.001);
    out.worst_ms = sorted.back();

    const double t1_5 = out.median_ms * 1.5;
    const double t2 = out.median_ms * 2.0;
    const double t3 = out.median_ms * 3.0;
    for (double ms : raw_values) {
        out.over_25_ms += ms > 25.0;
        out.over_33_ms += ms > 33.333333;
        out.over_50_ms += ms > 50.0;
        out.over_100_ms += ms > 100.0;
        out.over_250_ms += ms > 250.0;
        if (ms > t1_5) {
            ++out.over_1_5x;
            out.stutter_total_ms += ms;
            out.stutter_excess_ms += ms - out.median_ms;
        }
        out.over_2x += ms > t2;
        out.over_3x += ms > t3;
    }
    return out;
}

template <typename Atomic>
uint64_t LoadCounter(const Atomic& value) {
    return value.load(std::memory_order_relaxed);
}

#if NXEMU_ENABLE_PERF_CAPTURE_INSTRUMENTATION
bool QueryProcessCpuTime100ns(uint64_t& value) {
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return false;
    }
    const auto to_uint64 = [](const FILETIME& time) {
        ULARGE_INTEGER ticks{};
        ticks.LowPart = time.dwLowDateTime;
        ticks.HighPart = time.dwHighDateTime;
        return ticks.QuadPart;
    };
    value = to_uint64(kernel) + to_uint64(user);
    return true;
#else
    (void)value;
    return false;
#endif
}
#endif

void PutFrameSummary(JsonValue& target, const FrameSummary& s) {
    target["one_percent_sample_count"] = JsonValue(static_cast<uint64_t>(std::ceil(s.count * 0.01)));
    target["point_one_percent_sample_count"] = JsonValue(static_cast<uint64_t>(std::ceil(s.count * 0.001)));
    target["count"] = JsonValue(static_cast<int64_t>(s.count));
    target["mean_ms"] = JsonValue(s.mean_ms);
    target["median_ms"] = JsonValue(s.median_ms);
    target["p95_ms"] = JsonValue(s.p95_ms);
    target["p99_ms"] = JsonValue(s.p99_ms);
    target["p999_ms"] = JsonValue(s.p999_ms);
    target["one_percent_low_fps"] = JsonValue(s.one_percent_low_fps);
    target["point_one_percent_low_fps"] = JsonValue(s.point_one_percent_low_fps);
    target["worst_ms"] = JsonValue(s.worst_ms);
    target["over_25_ms"] = JsonValue(static_cast<int64_t>(s.over_25_ms));
    target["over_33_ms"] = JsonValue(static_cast<int64_t>(s.over_33_ms));
    target["over_50_ms"] = JsonValue(static_cast<int64_t>(s.over_50_ms));
    target["over_100_ms"] = JsonValue(static_cast<int64_t>(s.over_100_ms));
    target["over_250_ms"] = JsonValue(static_cast<int64_t>(s.over_250_ms));
    target["over_1_5x"] = JsonValue(static_cast<int64_t>(s.over_1_5x));
    target["over_2x"] = JsonValue(static_cast<int64_t>(s.over_2x));
    target["over_3x"] = JsonValue(static_cast<int64_t>(s.over_3x));
    target["stutter_total_ms"] = JsonValue(s.stutter_total_ms);
    target["stutter_excess_ms"] = JsonValue(s.stutter_excess_ms);
}

void PutConfig(JsonValue& target, const PerformanceCaptureConfig& c) {
    target["renderer_backend"] = JsonValue(static_cast<int64_t>(c.renderer_backend));
    target["shader_backend"] = JsonValue(static_cast<int64_t>(c.shader_backend));
    target["gpu_accuracy"] = JsonValue(static_cast<int64_t>(c.gpu_accuracy));
    target["dma_accuracy"] = JsonValue(static_cast<int64_t>(c.dma_accuracy));
    target["vsync_mode"] = JsonValue(static_cast<int64_t>(c.vsync_mode));
    target["vulkan_device"] = JsonValue(static_cast<int64_t>(c.vulkan_device));
    target["cpu_backend"] = JsonValue(static_cast<int64_t>(c.cpu_backend));
    target["cpu_accuracy"] = JsonValue(static_cast<int64_t>(c.cpu_accuracy));
    target["docked_mode"] = JsonValue(static_cast<int64_t>(c.docked_mode));
    target["memory_layout"] = JsonValue(static_cast<int64_t>(c.memory_layout));
    target["speed_limit"] = JsonValue(static_cast<int64_t>(c.speed_limit));
    target["astc_decode_mode"] = JsonValue(static_cast<int64_t>(c.astc_decode_mode));
    target["nvdec_emulation"] = JsonValue(static_cast<int64_t>(c.nvdec_emulation));
    target["resolution_setup"] = JsonValue(static_cast<int64_t>(c.resolution_setup));
    target["scaling_filter"] = JsonValue(static_cast<int64_t>(c.scaling_filter));
    target["anti_aliasing"] = JsonValue(static_cast<int64_t>(c.anti_aliasing));
    target["anisotropic_filtering"] = JsonValue(static_cast<int64_t>(c.anisotropic_filtering));
    target["astc_recompression"] = JsonValue(static_cast<int64_t>(c.astc_recompression));
    target["vram_usage_mode"] = JsonValue(static_cast<int64_t>(c.vram_usage_mode));
    target["fsr_sharpness"] = JsonValue(static_cast<int64_t>(c.fsr_sharpness));
    target["resolution_factor"] = JsonValue(static_cast<double>(c.resolution_factor));
    target["cpu_options_mask"] = JsonValue(static_cast<uint64_t>(c.cpu_options_mask));
    target["nce_enabled"] = JsonValue(c.nce_enabled);
    target["sync_memory_operations"] = JsonValue(c.sync_memory_operations);
    target["use_speed_limit"] = JsonValue(c.use_speed_limit);
    target["use_multi_core"] = JsonValue(c.use_multi_core);
    target["async_gpu"] = JsonValue(c.async_gpu);
    target["async_presentation"] = JsonValue(c.async_presentation);
    target["async_shader_building"] = JsonValue(c.async_shader_building);
    target["disk_pipeline_cache"] = JsonValue(c.disk_pipeline_cache);
    target["vulkan_pipeline_cache"] = JsonValue(c.vulkan_pipeline_cache);
    target["force_maximum_clocks"] = JsonValue(c.force_maximum_clocks);
    target["reactive_flushing"] = JsonValue(c.reactive_flushing);
    target["fast_gpu_time"] = JsonValue(c.fast_gpu_time);
    target["sync_to_video_framerate"] = JsonValue(c.sync_to_video_framerate);
    target["barrier_feedback_loops"] = JsonValue(c.barrier_feedback_loops);
}

} // namespace

namespace Core {

bool QueryPerformanceCaptureProcessMemory(PerformanceCaptureProcessMemorySample& sample) {
#if NXEMU_ENABLE_PERF_CAPTURE_INSTRUMENTATION && defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = static_cast<DWORD>(sizeof(counters));
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                 static_cast<DWORD>(sizeof(counters)))) {
        return false;
    }
    sample.working_set_bytes = static_cast<uint64_t>(counters.WorkingSetSize);
    sample.private_bytes = static_cast<uint64_t>(counters.PrivateUsage);
    return true;
#else
    (void)sample;
    return false;
#endif
}

bool PerformanceCapture::Start(uint64_t title_, const PerformanceCaptureConfig& config_,
                               const std::function<std::chrono::microseconds()>& system_time) {
#if !NXEMU_ENABLE_PERF_CAPTURE_INSTRUMENTATION
    return false;
#else
    std::scoped_lock lock(lifecycle);
    if (IsActive() || title_ == 0) return false;
    try {
        title = title_;
        config = config_;
        const auto now = std::time(nullptr);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &now);
#else
        gmtime_r(&now, &utc);
#endif
        std::ostringstream stamp;
        stamp.imbue(std::locale::classic());
        stamp << std::put_time(&utc, "%Y-%m-%d_%H-%M-%S");
        started_utc = stamp.str();
        shared.Begin();
        start_system = system_time();
        process_cpu_available = QueryProcessCpuTime100ns(process_cpu_start_100ns);
        process_cpu_stop_100ns = process_cpu_start_100ns;
        PerformanceCaptureProcessMemorySample memory_sample{};
        if (QueryPerformanceCaptureProcessMemory(memory_sample)) {
            shared.ProcessMemory(memory_sample.working_set_bytes, memory_sample.private_bytes);
        }
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR(Core, "Performance capture start failed: {}", e.what());
        return false;
    }
#endif
}

bool PerformanceCapture::Stop(const std::function<std::chrono::microseconds()>& system_time, std::string& output_path) {
#if !NXEMU_ENABLE_PERF_CAPTURE_INSTRUMENTATION
    return false;
#else
    std::scoped_lock lock(lifecycle);
    if (shared.Epoch()) {
        PerformanceCaptureProcessMemorySample memory_sample{};
        if (QueryPerformanceCaptureProcessMemory(memory_sample)) {
            shared.ProcessMemory(memory_sample.working_set_bytes, memory_sample.private_bytes);
        }
        stopped = shared.Close();
        stop_system = system_time();
        if (process_cpu_available && !QueryProcessCpuTime100ns(process_cpu_stop_100ns)) {
            process_cpu_available = false;
        }
        pending.store(true, std::memory_order_release);
    }
    if (!pending.load(std::memory_order_acquire)) return false;
    // After Close, no writer can touch this data. Failed exports retain it for F9 retry.
    const auto& data = *shared.data;
    std::filesystem::path temporary;
    try {
        const auto duration_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            stopped - data.start).count());
        const double seconds = static_cast<double>(duration_ns) / 1e9;
        JsonValue root(JsonValueType::Object);
        root["format_version"] = JsonValue(int64_t{7});
        root["title_id"] = JsonValue(fmt::format("{:016X}", title));
        root["started_utc"] = JsonValue(started_utc);
        root["duration_seconds"] = JsonValue(seconds);
        root["game"]["version_raw"] = JsonValue(static_cast<uint64_t>(game_version));
        root["game"]["version"] = game_display_version.empty() ? JsonValue(std::to_string(game_version)) : JsonValue(game_display_version);
        root["build"]["revision"] = JsonValue(GIT_REVISION);
        root["build"]["version"] = JsonValue(GIT_VERSION);
#ifdef _MSC_VER
        root["build"]["compiler"] = JsonValue("MSVC " + std::to_string(_MSC_VER));
#else
        root["build"]["compiler"] = JsonValue(__VERSION__);
#endif
#ifdef NDEBUG
        root["build"]["configuration"] = JsonValue("Release");
#else
        root["build"]["configuration"] = JsonValue("Debug");
#endif
        root["build"]["instrumentation"] = JsonValue(true);
        PutConfig(root["config"], config);
        root["emulation_speed_source"] = JsonValue(config.use_multi_core ? "unavailable_host_clock" : "guest_cpu_ticks");
        root["emulation_speed"] = config.use_multi_core || seconds <= 0 ? JsonValue() :
            JsonValue(std::chrono::duration<double>(stop_system - start_system).count() / seconds);
        {
            std::scoped_lock device_lock(shared.device_mutex);
            root["device"]["model"] = shared.gpu_model.empty() ? JsonValue() : JsonValue(shared.gpu_model);
            root["device"]["driver"] = shared.gpu_driver.empty() ? JsonValue() : JsonValue(shared.gpu_driver);
        }
        const auto invalidations = data.invalidations.load(std::memory_order_relaxed);
        root["invalidations"]["paused"] = JsonValue((invalidations & static_cast<uint32_t>(PerformanceInvalidation::Paused)) != 0);
        root["invalidations"]["configuration_changed"] = JsonValue((invalidations & static_cast<uint32_t>(PerformanceInvalidation::ConfigurationChanged)) != 0);
        root["invalidations"]["hidden"] = JsonValue((invalidations & static_cast<uint32_t>(PerformanceInvalidation::Hidden)) != 0);
        root["invalidations"]["shutdown"] = JsonValue((invalidations & static_cast<uint32_t>(PerformanceInvalidation::Shutdown)) != 0);
        const auto unfinished = LoadCounter(data.in_flight);
        root["boundary_operations_in_flight"] = JsonValue(unfinished);
        root["timing_metrics_complete"] = JsonValue(unfinished == 0);
        root["counter_window_policy"] = JsonValue("events committed before closure; timings require start and end in the same capture; inclusive durations overlap");
        root["availability"]["vulkan_scheduler"] = JsonValue(config.renderer_backend == static_cast<int32_t>(RendererBackend::Vulkan));
        root["availability"]["vulkan_presentation"] = JsonValue(config.renderer_backend == static_cast<int32_t>(RendererBackend::Vulkan));
        root["availability"]["pipeline_native_build"] = JsonValue(config.renderer_backend == static_cast<int32_t>(RendererBackend::OpenGL) || config.renderer_backend == static_cast<int32_t>(RendererBackend::Vulkan));
        root["display_timing_available"] = JsonValue(false);
        root["physical_gpu_time_available"] = JsonValue(false);
        root["frame_policy"] = JsonValue("first event establishes origin; all subsequent complete intervals retained; no edge trimming");
        for (std::size_t stream = 0; stream < FrameStreamCount; ++stream) {
            const auto& f = data.frames[stream];
            std::vector<double> intervals;
            if (!f.timestamps_ns.empty()) intervals.reserve(f.timestamps_ns.size() - 1);
            for (size_t i = 1; i < f.timestamps_ns.size(); ++i)
                intervals.push_back(static_cast<double>(f.timestamps_ns[i] - f.timestamps_ns[i-1]) / 1e6);
            const auto summary = SummarizeFrames(intervals);
            auto& target = root["streams"][FrameStreamNames[stream]];
            PutFrameSummary(target["intervals"], summary);
            target["frame_events"] = JsonValue(f.events);
            target["capture_rate_fps"] = JsonValue(seconds > 0 ? f.events / seconds : 0.0);
            target["dropped_samples"] = JsonValue(f.dropped);
            target["truncated"] = JsonValue(f.dropped != 0);
            target["sampled_until_ns"] = f.timestamps_ns.empty() ? JsonValue() : JsonValue(f.timestamps_ns.back());
            target["first_frame_ns"] = f.events ? JsonValue(f.first_ns) : JsonValue();
            target["last_frame_ns"] = f.events ? JsonValue(f.last_ns) : JsonValue();
            const auto silent_ns = f.events ? duration_ns - f.last_ns : duration_ns;
            target["time_since_last_frame_ms"] = JsonValue(static_cast<double>(silent_ns) / 1e6);
            target["end_interval_censored"] = JsonValue(true);
            const double stall_threshold_ms = std::max(250.0, summary.median_ms * 3.0);
            const bool stalled = static_cast<double>(silent_ns) / 1e6 > stall_threshold_ms;
            target["stalled_at_stop"] = JsonValue(stalled);
            target["insufficient_samples"] = JsonValue(summary.count < 100);
        }
#define EXPORT_COUNTER(name) root["counters"][#name] = JsonValue(LoadCounter(data.counters[static_cast<size_t>(PerformanceCounter::name)]));
        NXEMU_PERF_CAPTURE_STABLE_COUNTERS(EXPORT_COUNTER)
#undef EXPORT_COUNTER
        root["experimental"] = JsonValue(JsonValueType::Object);
#define EXPORT_EXPERIMENTAL_COUNTER(name) root["experimental"][#name] = JsonValue(LoadCounter(data.counters[static_cast<size_t>(PerformanceCounter::name)]));
        NXEMU_PERF_CAPTURE_EXPERIMENTAL_COUNTERS(EXPORT_EXPERIMENTAL_COUNTER)
#undef EXPORT_EXPERIMENTAL_COUNTER
#define EXPORT_TIME(name) { const auto& t = data.timings[static_cast<size_t>(PerformanceTiming::name)]; \
        auto& v = root["timings"][#name]; v["count"] = JsonValue(LoadCounter(t.count)); \
        v["total_ns"] = JsonValue(LoadCounter(t.total_ns)); v["max_ns"] = JsonValue(LoadCounter(t.max_ns)); \
        v["over_1ms"] = JsonValue(LoadCounter(t.over_1ms)); v["over_5ms"] = JsonValue(LoadCounter(t.over_5ms)); }
        NXEMU_PERF_CAPTURE_TIMINGS(EXPORT_TIME)
#undef EXPORT_TIME
        const auto samples = LoadCounter(data.memory_samples);
        root["memory"]["available"] = JsonValue(samples != 0);
        root["memory"]["samples"] = JsonValue(samples);
        root["memory"]["first_bytes"] = samples ? JsonValue(LoadCounter(data.memory_first)) : JsonValue();
        root["memory"]["last_bytes"] = samples ? JsonValue(LoadCounter(data.memory_last)) : JsonValue();
        root["memory"]["sampled_peak_bytes"] = samples ? JsonValue(LoadCounter(data.memory_peak)) : JsonValue();

        const auto process_memory_samples = LoadCounter(data.process_memory_samples);
        root["process_memory"]["available"] = JsonValue(process_memory_samples != 0);
        root["process_memory"]["samples"] = JsonValue(process_memory_samples);
        auto& working_set = root["process_memory"]["working_set"];
        working_set["first_bytes"] = process_memory_samples
                                         ? JsonValue(LoadCounter(data.process_working_set_first))
                                         : JsonValue();
        working_set["last_bytes"] = process_memory_samples
                                        ? JsonValue(LoadCounter(data.process_working_set_last))
                                        : JsonValue();
        working_set["sampled_peak_bytes"] = process_memory_samples
                                                ? JsonValue(LoadCounter(data.process_working_set_peak))
                                                : JsonValue();
        auto& private_memory = root["process_memory"]["private_commit"];
        private_memory["first_bytes"] = process_memory_samples
                                            ? JsonValue(LoadCounter(data.process_private_first))
                                            : JsonValue();
        private_memory["last_bytes"] = process_memory_samples
                                           ? JsonValue(LoadCounter(data.process_private_last))
                                           : JsonValue();
        private_memory["sampled_peak_bytes"] = process_memory_samples
                                                   ? JsonValue(LoadCounter(data.process_private_peak))
                                                   : JsonValue();

        const bool cpu_available =
            process_cpu_available && process_cpu_stop_100ns >= process_cpu_start_100ns;
        const uint64_t cpu_delta_100ns =
            cpu_available ? process_cpu_stop_100ns - process_cpu_start_100ns : 0;
        const double process_cpu_seconds = static_cast<double>(cpu_delta_100ns) / 10'000'000.0;
        root["process_cpu"]["available"] = JsonValue(cpu_available);
        root["process_cpu"]["process_time_seconds"] = cpu_available ? JsonValue(process_cpu_seconds) : JsonValue();
        root["process_cpu"]["average_core_equivalents"] =
            cpu_available && seconds > 0.0 ? JsonValue(process_cpu_seconds / seconds) : JsonValue();

        const auto base = Common::FS::GetYuzuPath(Common::FS::YuzuPath::LogDir) / "performance" / fmt::format("{:016X}", title);
        std::filesystem::create_directories(base);
        std::filesystem::path destination;
        for (uint64_t suffix = 1; ; ++suffix) {
            const auto name = suffix == 1 ? started_utc : started_utc + "_" + std::to_string(suffix);
            destination = base / name;
            if (std::filesystem::exists(destination)) continue;
            const auto candidate = base / (name + ".pending");
            if (!std::filesystem::create_directory(candidate)) continue;
            temporary = candidate;
            if (!std::filesystem::exists(destination)) break;
            std::filesystem::remove(temporary); // Only our still-empty reservation.
            temporary.clear();
        }
        {
            std::ofstream file(temporary / "frametimes.csv", std::ios::binary);
            file.imbue(std::locale::classic());
            file.exceptions(std::ios::failbit | std::ios::badbit);
            file << "stream,frame_id,timestamp_ns,interval_ms\n" << std::fixed << std::setprecision(6);
            // Merge streams by timestamp, not by unrelated per-stream frame ordinals.
            std::array<std::size_t, FrameStreamCount> index{};
            while (true) {
                std::size_t stream = FrameStreamCount;
                uint64_t timestamp = 0;
                for (std::size_t candidate = 0; candidate < FrameStreamCount; ++candidate) {
                    const auto& frames = data.frames[candidate].timestamps_ns;
                    if (index[candidate] >= frames.size()) {
                        continue;
                    }
                    const auto candidate_timestamp = frames[index[candidate]];
                    if (stream == FrameStreamCount || candidate_timestamp < timestamp) {
                        stream = candidate;
                        timestamp = candidate_timestamp;
                    }
                }
                if (stream == FrameStreamCount) {
                    break;
                }

                const auto& frames = data.frames[stream].timestamps_ns;
                const auto i = index[stream]++;
                file << FrameStreamNames[stream] << ',' << i << ',' << frames[i] << ',';
                if (i) file << static_cast<double>(frames[i] - frames[i - 1]) / 1e6;
                file << '\n';
            }
            file.flush();
            file.close();
        }
        {
            std::ofstream file(temporary / "summary.json", std::ios::binary);
            file.exceptions(std::ios::failbit | std::ios::badbit);
            const auto json = JsonStyledWriter().write(root);
            file.write(json.data(), static_cast<std::streamsize>(json.size()));
            file.flush();
            file.close();
        }
        std::filesystem::rename(temporary, destination);
        temporary.clear();
        output_path = destination.string();
        pending.store(false, std::memory_order_release);
        return true;
    } catch (const std::exception& e) {
        // Remove only our own partial files; never touch a published capture.
        if (!temporary.empty()) {
            std::error_code ignored;
            std::filesystem::remove(temporary / "summary.json", ignored);
            std::filesystem::remove(temporary / "frametimes.csv", ignored);
            std::filesystem::remove(temporary, ignored);
        }
        LOG_ERROR(Core, "Performance export failed (F9 retries the retained capture): {}", e.what());
        return false;
    }
#endif
}

} // namespace Core
