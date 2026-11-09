// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_common/thread_worker.h"
#include "yuzu_video_core/engines/draw_manager.h"
#include "yuzu_video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "yuzu_video_core/renderer_vulkan/guest_recipe_input_cache.h"
#include "yuzu_video_core/shader_profile_journal.h"
#include "yuzu_video_core/shader_profile_runtime.h"
#include <algorithm>
#include <bit>
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
            trace_inputs=true;
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
        // NXEMU_RECIPE_SAMPLE=1 observes every frame; NXEMU_RECIPE_QUIET sets the quiet frames before sampling.
        if (const char* option=std::getenv("NXEMU_RECIPE_SAMPLE"); option && *option)
            sample_period=std::max(1,std::atoi(option));
        if (const char* option=std::getenv("NXEMU_RECIPE_QUIET"); option && *option)
            quiet_limit=static_cast<u32>(std::max(0,std::atoi(option)));
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
    // vertex_inputs: the mask the key's vertex attribute types went through
    // (FixedPipelineState::MaskUnusedState), all bits when no attribute was masked.
    void Observe(Engine & engine, const GraphicsPipelineCacheKey & key, const DynamicFeatures & features,
                 u64* prefix_memo = nullptr, u64 vertex_inputs = ~0ULL) noexcept
    {
        // Once no recipe has been new for a while, only one random frame in sample_period
        // is observed. Draws through a transition seen for the first time are always
        // observed, with the rest of their frame. Skipped frames leave the register
        // groups stale, so register writes skip their hash update too.
        if (skip_frame && prefix_memo && *prefix_memo && channel == &engine) {
            engine.dirty.shader_profile_stale = 0xffU;
            return;
        }
        ObserveState(engine.dirty, engine.regs, engine.draw_manager->GetDrawState().topology,
                     engine.engine_state, &engine, key, features, prefix_memo, vertex_inputs);
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
                      const DynamicFeatures& features, u64* prefix_memo = nullptr,
                      u64 vertex_inputs = ~0ULL) noexcept
    {
        // Keep the frequent unchanged draw path small enough to inline. The
        // complete measured observer remains available when diagnostics are on.
        if (!timing) {
            if (failed) return;
            if (channel == source && dirty.shader_profile_changes == 0 &&
                dirty.shader_profile_registers[offsetof(Engine::Regs, alpha_test_enabled) / 4] != 0 &&
                topology == last_topology && hint == last_hint &&
                (prefix_memo && *prefix_memo ? *prefix_memo == last_prefix_id
                                             : last_prefix_valid && std::memcmp(last_prefix.data(), &key, sizeof(last_prefix)) == 0)) return;
            // Most changed draws switch between already recorded recipes. Their exact
            // inputs (register group hashes, program prefix id, topology, hint and
            // features) are fingerprinted and checked against recently seen ones,
            // without leaving this inline path.
            if (channel == source && prefix_memo && *prefix_memo && !trace_inputs && !resync_hashes &&
                dirty.shader_profile_stale == 0 &&
                dirty.shader_profile_registers[offsetof(Engine::Regs, alpha_test_enabled) / 4] != 0) {
                const u64 fingerprint = Fingerprint(dirty.shader_profile_hashes, topology, hint,
                                                    FeatureBits(features), *prefix_memo);
                const size_t slot = SeenSlot(fingerprint);
                if (seen[slot] == fingerprint || seen[slot ^ 1] == fingerprint) {
                    if (*prefix_memo != last_prefix_id) {
                        last_prefix_id = *prefix_memo;
                        last_prefix_valid = false;
                    }
                    dirty.shader_profile_changes = 0;
                    last_topology = topology;
                    last_hint = hint;
                    // The previous state is unknown now; a later prefix-only miss captures again.
                    current_state = nullptr;
                    return;
                }
            }
        }
        ObserveChangedState(dirty,regs,topology,hint,source,key,features,prefix_memo,vertex_inputs);
    }
private:
    static u32 FeatureBits(const DynamicFeatures& features) noexcept {
        return (features.has_extended_dynamic_state ? 1U : 0U) |
            (features.has_extended_dynamic_state_2 ? 2U : 0U) |
            (features.has_extended_dynamic_state_2_extra ? 4U : 0U) |
            (features.has_extended_dynamic_state_3_blend ? 8U : 0U) |
            (features.has_extended_dynamic_state_3_enables ? 16U : 0U) |
            (features.has_dynamic_vertex_input ? 32U : 0U) |
            (features.has_vertex_stride_workaround ? 64U : 0U);
    }
    // Never zero, so an empty slot never matches. A 64-bit collision can only skip
    // recording one recipe, never change a recorded one.
    static u64 Fingerprint(const std::array<u64, Engine::DirtyState::ShaderProfileGroups>& hashes,
                           Engine::Regs::PrimitiveTopology topology, Engine::EngineHint hint,
                           u32 feature_bits, u64 prefix_id) noexcept {
        u64 x = prefix_id * 0x9e3779b97f4a7c15ULL ^
                (static_cast<u64>(topology) | static_cast<u64>(hint) << 16 |
                 static_cast<u64>(feature_bits) << 32) * 0xc2b2ae3d27d4eb4fULL;
        for (const u64 hash : hashes) x = std::rotl(x, 25) ^ hash;
        x *= 0xd6e8feb86659fd93ULL;
        return (x ^ (x >> 32)) | 1;
    }
    static constexpr size_t SeenSlots = 16384;
    static size_t SeenSlot(u64 fingerprint) noexcept { return (fingerprint >> 20) & (SeenSlots - 1); }
    void RememberSeen(const Engine::DirtyState& dirty, Engine::Regs::PrimitiveTopology topology,
                      Engine::EngineHint hint, u32 feature_bits) noexcept {
        if (trace_inputs) return;
        const u64 fingerprint = Fingerprint(dirty.shader_profile_hashes, topology, hint, feature_bits, last_prefix_id);
        // Two ways per set: the newest replaces the older one.
        const size_t slot = SeenSlot(fingerprint);
        if (seen[slot] == fingerprint || seen[slot ^ 1] == fingerprint) return;
        seen[slot ^ 1] = seen[slot];
        seen[slot] = fingerprint;
    }
    void ObserveChangedState(Engine::DirtyState& dirty, const Engine::Regs& regs,
                      Engine::Regs::PrimitiveTopology topology, Engine::EngineHint hint,
                      const void* source, const GraphicsPipelineCacheKey& key,
                      const DynamicFeatures& features, u64* prefix_memo, u64 vertex_inputs) noexcept
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
            // A first-seen transition is always observed, but between known pipelines it
            // rarely holds a new recipe, so only a recorded recipe restarts full observation.
            // It usually comes with new content, whose state variants show up in the same
            // frame, so the rest of this frame is observed.
            if (!known_prefix) {
                ++new_transitions;
                skip_frame = false;
            }
            const bool prefix_changed = known_prefix ? known_prefix != last_prefix_id
                                                     : !last_prefix_valid || std::memcmp(last_prefix.data(), &key, sizeof(last_prefix)) != 0;
            if (!changed && !prefix_changed) return;
            const u32 feature_bits = FeatureBits(features);
            bool hit = false;
            {
                if (untracked) GuestPipelineState::Track(dirty);
                const auto* previous_state=current_state;
                const Entry* previous_entry=previous_state ? previous_state->entry : nullptr;
                const auto cached = [&] {
                    StageSample measured(timing, input_lookup_wall_ns);
                    // Input traces keep the exact word hashing so their keys stay comparable.
                    if (trace_inputs)
                        return input_cache.Find(regs, dirty.shader_profile_changes, full_capture,
                                                topology, hint, feature_bits, &key, prefix_changed, known_prefix);
                    // Writes that bypass the engine's register path are caught by the
                    // per-frame resync; a stale hash only costs a capture or one recording.
                    u64 refresh = dirty.shader_profile_stale;
                    if (full_capture || resync_hashes) refresh = GuestPipelineState::All;
                    if (refresh & GuestPipelineState::All) input_cache.RefreshHashes(regs, dirty, refresh);
                    dirty.shader_profile_stale = 0;
                    resync_hashes = false;
                    return input_cache.FindHashed(dirty.shader_profile_hashes, topology, hint, feature_bits,
                                                  &key, full_capture || prefix_changed, known_prefix);
                }();
                hit = cached.hit;
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
            if (prefix_changed || full_capture) {
                // Pipelines keep a memo id for their prefix words, so the 416-byte copy is
                // only needed for draws without one; those compare words, never a stale copy.
                if (known_prefix && !full_capture) {
                    last_prefix_valid = false;
                } else {
                    std::memcpy(last_prefix.data(), &key, sizeof(last_prefix));
                    last_prefix_valid = true;
                }
            }
            // A hit is a known recorded recipe for these exact inputs; its projection
            // was checked when it was recorded.
            if (hit && current_state->entry) {
                if (timing) ++cached_recipe_hits;
                RememberSeen(dirty, topology, hint, feature_bits);
                return;
            }
            if ([&] {
                    FixedPipelineState expected{current_state->projected};
                    expected.MaskUnusedState(vertex_inputs);
                    return expected != key.state;
                }())
                throw std::runtime_error("Automatic guest recipe projection mismatch");
            if (current_state->entry) {
                if (timing) ++cached_recipe_hits;
                RememberSeen(dirty, topology, hint, feature_bits);
                return;
            }
            static_assert(GuestPipelineState::WordCount == 278);
            std::memcpy(candidate_entry.data()+279, &key, sizeof(last_prefix));
            const auto duplicate = [&] {
                StageSample measured(timing, dedup_wall_ns);
                return entries.find(candidate_entry);
            }();
            if (duplicate!=entries.end()) { current_state->entry=&*duplicate; if (timing) ++duplicate_entries; RememberSeen(dirty, topology, hint, feature_bits); return; }
            current_state->entry = Record(candidate_entry);
            Discovery();
            if (!failed) RememberSeen(dirty, topology, hint, feature_bits);
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
    void Discovery() noexcept {
        if (quiet_frames >= quiet_limit) ++wakes;
        quiet_frames = 0;
        skip_frame = false;
    }
public:
    // Called on the GPU thread once per presented frame.
    void EndFrame() noexcept {
        if (quiet_frames < quiet_limit) ++quiet_frames;
        ++frames;
        // The observed frame is drawn at random, so a variant that recurs in step with a
        // fixed period is still seen.
        sample_state ^= sample_state << 13;
        sample_state ^= sample_state >> 7;
        sample_state ^= sample_state << 17;
        skip_frame = sample_period > 1 && quiet_frames >= quiet_limit && sample_state % sample_period != 0;
        if (skip_frame) ++skipped_frames;
        Flush();
    }
    // Called on the GPU thread at frame boundaries and before shutdown drain.
    // Existing independently checked journal frames retain their exact layout.
    void Flush() noexcept {
        if (++flushes % 16 == 0) resync_hashes = true;
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
        LOG_INFO(Render_Vulkan, "Automatic guest recipes: total_unique={} loaded={} new={} captures={} failed={} measured_observations={} observer_wall_ns={} frames={} skipped_frames={} wakes={} new_transitions={}", entries.size(), loaded_recipes, entries.size()-loaded_recipes, captures, failed, calls, elapsed_ns, frames, skipped_frames, wakes, new_transitions);
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
    bool resync_hashes{true};
    bool last_prefix_valid{};
    bool trace_inputs{};
    std::array<u64, SeenSlots> seen{};
    u32 flushes{};
    u32 sample_period{8};
    u32 quiet_limit{240};
    u32 quiet_frames{};
    bool skip_frame{};
    u64 frames{}, skipped_frames{}, wakes{}, new_transitions{};
    u64 sample_state{0x9e3779b97f4a7c15ULL};
    std::filesystem::path report_path;
    std::shared_ptr<VideoCommon::ShaderProfile::Journal::WriteMetrics> write_metrics;
};
} // namespace Vulkan
