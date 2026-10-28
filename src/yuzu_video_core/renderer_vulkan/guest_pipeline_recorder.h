// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_common/thread_worker.h"
#include "yuzu_video_core/engines/draw_manager.h"
#include "yuzu_video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "yuzu_video_core/renderer_vulkan/guest_recipe_input_cache.h"
#include "yuzu_video_core/shader_profile_journal.h"
#include "yuzu_video_core/shader_profile_runtime.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

namespace Vulkan
{
// Register writes only mark changed input groups. Exact input memoization avoids
// canonical capture for recently known recipes. Exact dedup uses a non-cryptographic
// hash; resource SHA-256 and file I/O run on the cache worker.
class GuestPipelineRecorder
{
    using Engine = Tegra::Engines::Maxwell3D;
    using Entry = VideoCommon::ShaderProfile::Journal::Entry;
    struct StageSample {
        u64* total{};
        std::chrono::steady_clock::time_point start{};
        StageSample(bool enabled, u64& sum) : total(enabled ? &sum : nullptr) {
            if (total) start = std::chrono::steady_clock::now();
        }
        ~StageSample() {
            if (total) *total += std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now()-start).count();
        }
    };

public:
    GuestPipelineRecorder(std::filesystem::path path_, Common::ThreadWorker & writer_) :
        path(std::move(path_)), writer(writer_)
    {
        auto saved = VideoCommon::ShaderProfile::Journal::Read(path);
        entries.reserve(VideoCommon::ShaderProfile::Journal::MaxEntries);
        entries.insert(saved.entries.begin(), saved.entries.end());
        loaded_recipes = entries.size();
        pending_writes.reserve(BatchEntries);
        if (const char* trace=std::getenv("NXEMU_RECIPE_INPUT_TRACE"); trace && *trace) {
            input_trace_path=std::filesystem::u8path(trace);
            input_trace.reserve(524288);
            input_cache.EnableDiagnosticKeys();
        }
        if (saved.incomplete_tail) std::filesystem::resize_file(path, saved.valid_bytes);
        if (!std::filesystem::exists(path) || std::filesystem::file_size(path) == 0)
        {
            std::ofstream out(path, std::ios::binary);
            out << VideoCommon::ShaderProfile::Journal::Magic;
            if (!out) throw std::runtime_error("Cannot initialize guest recipe journal");
        }
        if (const char* option=std::getenv("NXEMU_RECIPE_RECORD_TIMING"))
            timing=*option && std::string_view(option)!="0";
        if (const char * output = std::getenv("NXEMU_RECIPE_RECORD_REPORT"); output && *output)
        {
            report_path = std::filesystem::u8path(output);
            timing = true;
        }
        if (timing) write_metrics = std::make_shared<VideoCommon::ShaderProfile::Journal::WriteMetrics>();
    }
    static void MarkFailed(const std::filesystem::path & target) noexcept
    {
        try
        {
            auto marker = target;
            marker += ".failed";
            std::ofstream out(marker, std::ios::binary | std::ios::trunc);
            out << "Incomplete recording; see renderer log. Clear the local cache to start again.\n";
        }
        catch (...)
        {
        }
    }
    // prefix_memo: storage that the caller keeps for these exact key prefix words,
    // zero until the recorder fills it; or null when there is none.
    void Observe(Engine & engine, const GraphicsPipelineCacheKey & key, const DynamicFeatures & features,
                 u64* prefix_memo = nullptr) noexcept
    {
        ObserveState(engine.dirty, engine.regs, engine.draw_manager->GetDrawState().topology,
                     engine.engine_state, &engine, key, features, prefix_memo);
    }
    // Called once per compute pipeline and session; the journal keeps first use.
    void ObserveCompute(std::span<const u32, VideoCommon::ShaderProfile::Journal::ComputeWords> key) noexcept
    {
        if (failed) return;
        try
        {
            Entry entry{};
            entry[0] = VideoCommon::ShaderProfile::Journal::ComputeFlag;
            std::copy(key.begin(), key.end(), entry.begin() + 1);
            if (entries.contains(entry)) return;
            Record(entry);
        }
        catch (const std::exception & e)
        {
            failed = true;
            writer.QueueWork([target = path] { MarkFailed(target); });
            LOG_ERROR(Render_Vulkan, "Automatic guest recipe recorder stopped: {}", e.what());
        }
    }
    // Explicit engine inputs allow deterministic replay tests without constructing
    // an emulated GPU. The live path above supplies exactly these same values.
    void ObserveState(Engine::DirtyState& dirty, const Engine::Regs& regs,
                      Engine::Regs::PrimitiveTopology topology, Engine::EngineHint hint,
                      const void* source, const GraphicsPipelineCacheKey& key,
                      const DynamicFeatures& features, u64* prefix_memo = nullptr) noexcept
    {
        // Keep the frequent unchanged draw path small enough to inline. The
        // complete measured observer remains available when diagnostics are on.
        if (!timing) {
            if (failed) return;
            if (channel == source && dirty.shader_profile_changes == 0 &&
                dirty.shader_profile_registers[offsetof(Engine::Regs, alpha_test_enabled) / 4] != 0 &&
                topology == last_topology && hint == last_hint &&
                (prefix_memo && *prefix_memo ? *prefix_memo == last_prefix_id
                                             : std::memcmp(last_prefix.data(), &key, sizeof(last_prefix)) == 0)) return;
        }
        ObserveChangedState(dirty,regs,topology,hint,source,key,features,prefix_memo);
    }
private:
    void ObserveChangedState(Engine::DirtyState& dirty, const Engine::Regs& regs,
                      Engine::Regs::PrimitiveTopology topology, Engine::EngineHint hint,
                      const void* source, const GraphicsPipelineCacheKey& key,
                      const DynamicFeatures& features, u64* prefix_memo) noexcept
    {
        static_assert(offsetof(GraphicsPipelineCacheKey, state) == 104 * sizeof(u32));
        struct Sample
        {
            GuestPipelineRecorder & owner;
            std::chrono::steady_clock::time_point start{};
            explicit Sample(GuestPipelineRecorder & recorder) :
                owner(recorder)
            {
                if (owner.timing) start = std::chrono::steady_clock::now();
            }
            ~Sample()
            {
                if (owner.timing)
                {
                    ++owner.calls;
                    owner.elapsed_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
                }
            }
        } sample(*this);
        if (failed) return;
        try
        {
            const bool untracked = dirty.shader_profile_registers[offsetof(Engine::Regs, alpha_test_enabled) / 4] == 0;
            const bool full_capture = untracked || channel != source;
            const bool changed = full_capture || dirty.shader_profile_changes != 0 ||
                                 topology != last_topology || hint != last_hint;
            // Two ids may stand for the same words after an eviction; that only
            // costs a lookup, never a missed recipe.
            const u64 known_prefix = prefix_memo ? *prefix_memo : 0;
            const bool prefix_changed = known_prefix ? known_prefix != last_prefix_id
                                                     : std::memcmp(last_prefix.data(), &key, sizeof(last_prefix)) != 0;
            if (!changed && !prefix_changed) return;
            const u32 feature_bits = (features.has_extended_dynamic_state ? 1U : 0U) |
                (features.has_extended_dynamic_state_2 ? 2U : 0U) |
                (features.has_extended_dynamic_state_2_extra ? 4U : 0U) |
                (features.has_extended_dynamic_state_3_blend ? 8U : 0U) |
                (features.has_extended_dynamic_state_3_enables ? 16U : 0U) |
                (features.has_dynamic_vertex_input ? 32U : 0U) |
                (features.has_vertex_stride_workaround ? 64U : 0U);
            {
                if (untracked) GuestPipelineState::Track(dirty);
                const auto* previous_state=current_state;
                const Entry* previous_entry=previous_state ? previous_state->entry : nullptr;
                const auto cached = [&] {
                    StageSample measured(timing, input_lookup_wall_ns);
                    return input_cache.Find(regs, dirty.shader_profile_changes, full_capture,
                                            topology, hint, feature_bits, &key, prefix_changed, known_prefix);
                }();
                current_state = cached.state;
                last_prefix_id = input_cache.PrefixId();
                if (prefix_memo) *prefix_memo = last_prefix_id;
                if (!input_trace_path.empty() && input_trace.size()<524288)
                    input_trace.push_back(input_cache.DiagnosticKey());
                if (timing) { ++capture_candidates; cached_state_hits += cached.hit; }
                if (!cached.hit) {
                    // Prefix changes select another program combination without
                    // changing canonical register inputs. Find leaves projected
                    // data intact until this caller replaces it, even on eviction.
                    if (!changed && prefix_changed && previous_entry && (*previous_entry)[0]==feature_bits) {
                        current_state->projected=previous_state->projected;
                        candidate_entry=*previous_entry;
                        ++prefix_capture_reuses;
                    } else {
                        const auto captured = [&] {
                            StageSample measured(timing, capture_wall_ns);
                            return GuestPipelineState::Capture(regs, topology, hint);
                        }();
                        ++captures;
                        StageSample measured(timing, canonical_wall_ns);
                        captured.WriteWords(std::span<u32, GuestPipelineState::WordCount>(candidate_entry.data()+1, GuestPipelineState::WordCount));
                        candidate_entry[0] = feature_bits;
                        current_state->projected = captured.Project(features);
                        if (timing) ++canonical_updates;
                    }
                }
                channel = source;
                dirty.shader_profile_changes = 0;
                last_topology = topology;
                last_hint = hint;
            }
            if (prefix_changed || full_capture) std::memcpy(last_prefix.data(), &key, sizeof(last_prefix));
            if (current_state->projected != key.state)
                throw std::runtime_error("Automatic guest recipe projection mismatch");
            if (current_state->entry) {
                if (timing) ++cached_recipe_hits;
                return;
            }
            static_assert(GuestPipelineState::WordCount == 278);
            std::memcpy(candidate_entry.data()+279, &key, sizeof(last_prefix));
            const auto duplicate = [&] {
                StageSample measured(timing, dedup_wall_ns);
                return entries.find(candidate_entry);
            }();
            if (duplicate!=entries.end()) { current_state->entry=&*duplicate; if (timing) ++duplicate_entries; return; }
            current_state->entry = Record(candidate_entry);
        }
        catch (const std::exception & e)
        {
            failed = true;
            writer.QueueWork([target = path] { MarkFailed(target); });
            LOG_ERROR(Render_Vulkan, "Automatic guest recipe recorder stopped: {}", e.what());
        }
    }
    // Keeps a new entry and queues its frame. Throws when the journal is full.
    const Entry* Record(const Entry& entry)
    {
        if (entries.size() >= VideoCommon::ShaderProfile::Journal::MaxEntries)
            throw std::runtime_error("Automatic guest recipe journal is full");
        const Entry* kept;
        {
            StageSample measured(timing, dedup_wall_ns);
            kept = &*entries.insert(entry).first;
        }
        StageSample measured(timing, enqueue_wall_ns);
        if (writer_failed->load(std::memory_order_relaxed)) {
            failed = true;
            return kept;
        }
        pending_writes.push_back(entry);
        if (pending_writes.size() == BatchEntries) Flush();
        return kept;
    }
public:
    // Called on the GPU thread at frame boundaries and before shutdown drain.
    // Existing independently checked journal frames retain their exact layout.
    void Flush() noexcept {
        if (pending_writes.empty()) return;
        try {
            auto batch = std::move(pending_writes);
            pending_writes = {};
            pending_writes.reserve(BatchEntries);
            writer.QueueWork([target = path, batch = std::move(batch), metrics = write_metrics, write_failed = writer_failed] {
                try
                {
                    const auto before = metrics ? VideoCommon::ShaderProfile::Runtime::ThreadCpu() : VideoCommon::ShaderProfile::Runtime::CpuSample{};
                    VideoCommon::ShaderProfile::Journal::AppendBatch(target, batch, metrics.get());
                    if (metrics)
                    {
                        const auto after = VideoCommon::ShaderProfile::Runtime::ThreadCpu();
                        if (before.time_available && after.time_available && after.ns >= before.ns)
                        {
                            metrics->cpu_ns += after.ns - before.ns;
                            ++metrics->cpu_samples;
                        }
                        if (before.cycles_available && after.cycles_available && after.cycles >= before.cycles)
                        {
                            metrics->cpu_cycles += after.cycles - before.cycles;
                            ++metrics->cycle_samples;
                        }
                    }
                }
                catch (const std::exception & e)
                {
                    write_failed->store(true, std::memory_order_relaxed);
                    MarkFailed(target);
                    LOG_ERROR(Render_Vulkan, "Guest recipe journal: {}", e.what());
                }
            });
        } catch (const std::exception& error) {
            failed = true;
            MarkFailed(path);
            LOG_ERROR(Render_Vulkan, "Guest recipe batch stopped: {}", error.what());
        }
    }
    ~GuestPipelineRecorder()
    {
        Flush();
        writer.WaitForRequests();
        failed = failed || writer_failed->load(std::memory_order_relaxed);
        if (!input_trace_path.empty()) try {
            std::ofstream trace(input_trace_path,std::ios::binary|std::ios::trunc);
            trace.write("NXITR001",8);
            trace.write(reinterpret_cast<const char*>(input_trace.data()),input_trace.size()*sizeof(input_trace.front()));
            trace.flush();
            if(!trace)LOG_WARNING(Render_Vulkan,"Cannot write bounded input-cache diagnostic trace");
        } catch(const std::exception&) { LOG_WARNING(Render_Vulkan,"Cannot save input-cache diagnostic trace"); }
        LOG_INFO(Render_Vulkan, "Automatic guest recipes: total_unique={} loaded={} new={} captures={} failed={} measured_observations={} observer_wall_ns={}", entries.size(), loaded_recipes, entries.size()-loaded_recipes, captures, failed, calls, elapsed_ns);
        // All writer work is drained before this recorder is destroyed. Observer
        // time is elapsed steady-clock time, not per-thread CPU accounting.
        if (!report_path.empty() && write_metrics) try
            {
                const auto & m = *write_metrics;
                std::ofstream out(report_path, std::ios::binary | std::ios::trunc);
                out << "{\"schema\":3,\"observer_timing_enabled\":true,\"observer_scope\":\"Observe only; excludes register revision tracking\",\"unique_recipes\":" << entries.size()
                    << ",\"loaded_recipes\":" << loaded_recipes << ",\"new_recipes\":" << entries.size()-loaded_recipes << ",\"captures\":" << captures
                    << ",\"prefix_capture_reuses\":" << prefix_capture_reuses
                    << ",\"capture_candidates\":" << capture_candidates << ",\"cached_state_hits\":" << cached_state_hits << ",\"cached_recipe_hits\":" << cached_recipe_hits << ",\"input_lookup_wall_ns\":" << input_lookup_wall_ns << ",\"canonical_updates\":" << canonical_updates << ",\"duplicate_entries\":" << duplicate_entries
                    << ",\"capture_wall_ns\":" << capture_wall_ns << ",\"canonical_wall_ns\":" << canonical_wall_ns
                    << ",\"dedup_wall_ns\":" << dedup_wall_ns << ",\"enqueue_wall_ns\":" << enqueue_wall_ns
                    << ",\"failed\":" << failed << ",\"observed_draws\":" << calls << ",\"observer_wall_ns\":" << elapsed_ns
                    << ",\"observer_mean_ns_per_observation\":" << (calls?elapsed_ns/calls:0)
                    << ",\"capture_mean_ns_per_capture\":" << (captures?capture_wall_ns/captures:0)
                    << ",\"memo_storage_bytes\":" << input_cache.StorageBytes()
                    << ",\"worker_batches\":" << m.batches << ",\"worker_frames\":" << m.frames << ",\"worker_frame_bytes\":" << m.bytes
                    << ",\"worker_encoding_ns\":" << m.encoding_ns << ",\"worker_io_ns\":" << m.io_ns
                    << ",\"worker_cpu_ns\":" << m.cpu_ns << ",\"worker_cpu_samples\":" << m.cpu_samples
                    << ",\"worker_cpu_cycles\":" << m.cpu_cycles << ",\"worker_cycle_samples\":" << m.cycle_samples << "}";
                out.flush();
                if (!out) LOG_WARNING(Render_Vulkan, "Cannot write guest recording cost report");
            }
            catch (const std::exception & e)
            {
                LOG_WARNING(Render_Vulkan, "Guest recording cost report: {}", e.what());
            }
    }

private:
    static constexpr size_t BatchEntries = 32;
    std::filesystem::path input_trace_path;
    std::vector<std::array<u64,10>> input_trace;
    std::vector<Entry> pending_writes;
    std::shared_ptr<std::atomic_bool> writer_failed{std::make_shared<std::atomic_bool>(false)};
    std::filesystem::path path;
    Common::ThreadWorker & writer;
    std::unordered_set<Entry, VideoCommon::ShaderProfile::Journal::EntryHash> entries;
    GuestRecipeInputCache<> input_cache;
    GuestRecipeInputCache<>::State* current_state{};
    Entry candidate_entry{};
    std::array<u32, 104> last_prefix{};
    u64 last_prefix_id{};
    const void* channel{};
    u64 captures{}, calls{}, elapsed_ns{}, loaded_recipes{};
    u64 prefix_capture_reuses{};
    u64 canonical_updates{}, duplicate_entries{}, capture_candidates{}, cached_state_hits{}, cached_recipe_hits{};
    u64 input_lookup_wall_ns{};
    u64 capture_wall_ns{}, canonical_wall_ns{}, dedup_wall_ns{}, enqueue_wall_ns{};
    Engine::Regs::PrimitiveTopology last_topology{};
    Engine::EngineHint last_hint{};
    bool failed{};
    bool timing{};
    std::filesystem::path report_path;
    std::shared_ptr<VideoCommon::ShaderProfile::Journal::WriteMetrics> write_metrics;
};
} // namespace Vulkan
