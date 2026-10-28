// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <locale>
#include <sstream>
#include <type_traits>

namespace VideoCommon::ShaderProfile::Timing {
enum class Phase {
    ProfileIO, ProfileDecompression, JsonParse, SchemaAndLocators,
    ResourceTotal, ResourceIO, ResourceDecompression, ResourceVerification,
    EnvironmentAssembly, RecipeAssembly, ReaderTotal,
    GuestProjection, EnvironmentDeserialization, CacheAppend, ExistingCacheLoad,
    WarmupWait, StartupWall, CompilerWall, GuestTranslation, SpirvEmission,
    ShaderModuleCreation, NativeGraphics, NativeCompute, Count
};
enum class Counter {
    ProfileBytes, JsonBytes, ResourceReadBytes, ResourceDecodedBytes,
    ResourceGroups, Programs, Environments, Recipes, HleAliases,
    PipelineRequests, PipelineRequestsCompleted, StopRequested, Count
};
struct Metrics {
    static constexpr std::array names{
        "profile_io", "profile_decompression", "json_parse", "schema_and_locators",
        "resource_total", "resource_io", "resource_decompression", "resource_verification",
        "environment_assembly", "recipe_assembly", "reader_total",
        "guest_projection", "environment_deserialization", "cache_append", "existing_cache_load",
        "warmup_wait", "startup_wall", "compiler_wall", "guest_translation", "spirv_emission",
        "shader_module_creation", "native_graphics", "native_compute"};
    static constexpr std::array counters{
        "profile_bytes", "json_bytes", "resource_read_bytes", "resource_decoded_bytes",
        "resource_groups", "programs", "environments", "recipes", "hle_aliases",
        "pipeline_requests", "pipeline_requests_completed", "stop_requested"};
    static_assert(names.size() == static_cast<size_t>(Phase::Count));
    static_assert(counters.size() == static_cast<size_t>(Counter::Count));
    std::array<std::atomic<std::uint64_t>, names.size()> ns{}, calls{}, completed{};
    std::array<std::atomic<std::uint64_t>, counters.size()> counts{};
    void Add(Counter key, std::uint64_t value) {
        counts[static_cast<size_t>(key)].fetch_add(value, std::memory_order_relaxed);
    }
    std::string Json() const {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << "{\"schema\":1,\"phases\":{";
        for (size_t i=0; i<names.size(); ++i) {
            if (i) out << ',';
            out << '"' << names[i] << "\":{\"ms\":" << static_cast<double>(ns[i].load())/1e6
                << ",\"calls\":" << calls[i].load()
                << ",\"completed\":" << completed[i].load() << '}';
        }
        out << "},\"counts\":{";
        for (size_t i=0; i<counters.size(); ++i) {
            if (i) out << ',';
            out << '"' << counters[i] << "\":" << counts[i].load();
        }
        out << "}}";
        return out.str();
    }
};
class Timer {
public:
    Timer(Metrics* metrics_, Phase phase_) : metrics(metrics_), phase(phase_),
        exceptions(std::uncaught_exceptions()) {
        if (metrics) start=std::chrono::steady_clock::now();
    }
    Timer(const Timer&) = delete;
    ~Timer() { Stop(); }
    void Stop() noexcept {
        if (!metrics) return;
        const auto index=static_cast<size_t>(phase);
        metrics->ns[index].fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-start).count(), std::memory_order_relaxed);
        metrics->calls[index].fetch_add(1, std::memory_order_relaxed);
        if (std::uncaught_exceptions()==exceptions)
            metrics->completed[index].fetch_add(1, std::memory_order_relaxed);
        metrics=nullptr;
    }
private:
    Metrics* metrics;
    Phase phase;
    int exceptions;
    std::chrono::steady_clock::time_point start{};
};
template<class F> auto Measure(Metrics* metrics, Phase phase, F&& function) {
    Timer timer(metrics, phase);
    return function();
}
// Set only inside startup compiler work. Runtime draws never install a collector.
inline thread_local Metrics* current=nullptr;
class Context {
public:
    explicit Context(Metrics* metrics) : previous(current) { current=metrics; }
    ~Context() { current=previous; }
    Context(const Context&) = delete;
private:
    Metrics* previous;
};
} // namespace VideoCommon::ShaderProfile::Timing
