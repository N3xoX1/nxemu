// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <string>

namespace Vulkan {

// Diagnostic only. Full register capture is deliberately not the production recorder.
// This probe never substitutes a key, dispatches compilation, or reads shader instructions.
class GuestPipelineRecipeProbe {
public:
    static std::unique_ptr<GuestPipelineRecipeProbe> FromEnvironment() {
        const char* path = std::getenv("NXEMU_NXSHADER_RECORD");
        if (!path || !*path) path = std::getenv("NXEMU_SHADER_RECIPE_PROBE");
        return path && *path ? std::make_unique<GuestPipelineRecipeProbe>(path) : nullptr;
    }

    explicit GuestPipelineRecipeProbe(std::string path_) : path(std::move(path_)) {}

    void Observe(const Maxwell& regs, Maxwell::PrimitiveTopology topology,
                 Tegra::Engines::Maxwell3D::EngineHint hint,
                 const GraphicsPipelineCacheKey& key, const DynamicFeatures& features) noexcept {
        if (failed) return;
        const auto start = std::chrono::steady_clock::now();
        try {
            feature_bits = (features.has_extended_dynamic_state ? 1U : 0U) |
                (features.has_extended_dynamic_state_2 ? 2U : 0U) |
                (features.has_extended_dynamic_state_2_extra ? 4U : 0U) |
                (features.has_extended_dynamic_state_3_blend ? 8U : 0U) |
                (features.has_extended_dynamic_state_3_enables ? 16U : 0U) |
                (features.has_dynamic_vertex_input ? 32U : 0U) |
                (features.has_vertex_stride_workaround ? 64U : 0U);
            const auto guest = GuestPipelineState::Capture(regs, topology, hint);
            const auto projected = guest.Project(features);
            ++observations;
            if (projected != key.state) {
                ++mismatches;
                if (first_mismatch_byte < 0) {
                    const auto* a = reinterpret_cast<const unsigned char*>(&projected);
                    const auto* b = reinterpret_cast<const unsigned char*>(&key.state);
                    for (size_t i = 0; i < key.state.Size(); ++i) {
                        if (a[i] != b[i]) { first_mismatch_byte = static_cast<int>(i); break; }
                    }
                }
            }
            auto words = guest.Words();
            state_words = words.size();
            for (const u64 hash : key.unique_hashes) {
                words.push_back(static_cast<u32>(hash));
                words.push_back(static_cast<u32>(hash >> 32));
            }
            for (const auto& stage : key.cbuf_sizes) {
                words.insert(words.end(), stage.begin(), stage.end());
            }
            words.insert(words.end(), key.texture_pool.begin(), key.texture_pool.end());
            // Compare complete values, never use a hash alone to merge recipes.
            auto found = recipes.find(words);
            if (found != recipes.end()) ++found->second;
            else if (recipes.size() < MaxRecipes) recipes.emplace(std::move(words), 1);
            else ++dropped;
            if (local_keys.size() < MaxRecipes) {
                local_keys.emplace(reinterpret_cast<const char*>(&key), key.Size());
            }
        } catch (...) { failed = true; }
        elapsed_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
    }

    void Flush() const noexcept {
        try {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) { LOG_ERROR(Render_Vulkan, "Cannot write guest recipe probe report"); return; }
            out << "{\"schema\":\"nxshader-guest-state-probe-1\",\"diagnostic_only\":true,"
                   "\"shader_instructions_included\":false,\"observations\":" << observations
                << ",\"projection_mismatches\":" << mismatches
                << ",\"first_mismatch_byte\":" << first_mismatch_byte
                << ",\"dropped_observations\":" << dropped
                << ",\"failed\":" << (failed ? "true" : "false")
                << ",\"probe_cpu_ns\":" << elapsed_ns
                << ",\"local_keys\":" << local_keys.size()
                << ",\"guest_state_word_count\":" << state_words
                << ",\"feature_bits\":" << feature_bits
                << ",\"recipes\":[";
            bool first = true;
            for (const auto& [words, count] : recipes) {
                if (!first) out << ',';
                first = false;
                out << "{\"observations\":" << count << ",\"words\":[";
                for (size_t i = 0; i < words.size(); ++i) {
                    if (i) out << ',';
                    out << words[i];
                }
                out << "]}";
            }
            out << "]}\n";
            out.flush();
            if (!out) LOG_ERROR(Render_Vulkan, "Incomplete guest recipe probe report");
            LOG_INFO(Render_Vulkan, "Guest recipe probe: observations={} recipes={} mismatches={} dropped={} failed={}",
                     observations, recipes.size(), mismatches, dropped, failed);
        } catch (...) { LOG_ERROR(Render_Vulkan, "Guest recipe probe report failed"); }
    }

private:
    static constexpr size_t MaxRecipes = 16384;
    std::string path;
    std::map<std::vector<u32>, u64> recipes;
    std::set<std::string> local_keys;
    u64 observations{}, mismatches{}, dropped{}, elapsed_ns{};
    size_t state_words{};
    u32 feature_bits{};
    int first_mismatch_byte{-1};
    bool failed{};
};
} // namespace Vulkan
