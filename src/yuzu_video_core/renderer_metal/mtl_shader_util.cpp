// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <fmt/format.h>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <array>
#include <atomic>
#include <system_error>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <string_view>
#include <vector>

#include <pthread.h>

#include <spirv_msl.hpp>

#include "yuzu_common/cityhash.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/settings.h"
#include "yuzu_common/thread.h"
#include "video_settings.h"
#include "yuzu_shader_recompiler/backend/spirv/emit_spirv.h"
#include "yuzu_video_core/metal_common/metal_device.h"
#include "yuzu_video_core/renderer_metal/mtl_shader_util.h"

namespace Metal {
namespace {
spv::ExecutionModel ToExecutionModel(Shader::Stage stage) {
    switch (stage) {
    case Shader::Stage::VertexA:
    case Shader::Stage::VertexB:
        return spv::ExecutionModelVertex;
    case Shader::Stage::TessellationControl:
        return spv::ExecutionModelTessellationControl;
    case Shader::Stage::TessellationEval:
        return spv::ExecutionModelTessellationEvaluation;
    case Shader::Stage::Geometry:
        return spv::ExecutionModelGeometry;
    case Shader::Stage::Fragment:
        return spv::ExecutionModelFragment;
    case Shader::Stage::Compute:
        return spv::ExecutionModelGLCompute;
    }
    return spv::ExecutionModelVertex;
}

MTL::LanguageVersion ToLanguageVersion(u32 msl_version) {
    // Same major*10000 + minor*100 encoding SPIRV-Cross uses.
    const u32 major = msl_version / 10000;
    const u32 minor = (msl_version % 10000) / 100;

    if (major > 3 || (major == 3 && minor >= 1)) {
        return MTL::LanguageVersion3_1;
    }
    if (major >= 3) {
        return MTL::LanguageVersion3_0;
    }
    if (major == 2 && minor >= 4) {
        return MTL::LanguageVersion2_4;
    }
    return MTL::LanguageVersion2_4;
}

void ReportUnsupportedFeatures(const Device& device, Shader::Stage stage,
                               const Shader::Info& info) {
    if (stage == Shader::Stage::Geometry) {
        LOG_WARNING(Shader_MSL, "Geometry shaders have no Metal equivalent");
    }
    if (info.stores_tess_level_outer || info.stores_tess_level_inner) {
        LOG_WARNING(Shader_MSL, "Tessellation uses a different model in Metal");
    }
    const bool supports_typeless_reads =
        device.GetDevice() != nullptr &&
        device.GetDevice()->readWriteTextureSupport() >= MTL::ReadWriteTextureTier2;
    if ((!supports_typeless_reads && info.uses_typeless_image_reads) ||
        info.uses_typeless_image_writes) {
        LOG_WARNING(Shader_MSL, "Typeless image access is not expressible in MSL");
    }
}

// Decorate the position builtin `Invariant`, SPIRV-Cross turns that into MSL's
// `[[position, invariant]]`. Without it Apple's compiler can schedule position math
// per pipeline, so an EQUAL depth test between two passes kills every fragment.
void ForcePositionInvariance(spirv_cross::CompilerMSL& compiler) {
    for (const spirv_cross::VariableID id : compiler.get_active_interface_variables()) {
        // Standalone `out gl_Position`, which is how the recompiler emits it.
        if (compiler.has_decoration(id, spv::DecorationBuiltIn) &&
            compiler.get_decoration(id, spv::DecorationBuiltIn) == spv::BuiltInPosition) {
            compiler.set_decoration(id, spv::DecorationInvariant);
            continue;
        }

        const spirv_cross::SPIRType& type = compiler.get_type_from_variable(id);
        if (type.basetype != spirv_cross::SPIRType::Struct) {
            continue;
        }
        for (u32 member = 0; member < type.member_types.size(); ++member) {
            if (compiler.has_member_decoration(type.self, member, spv::DecorationBuiltIn) &&
                compiler.get_member_decoration(type.self, member, spv::DecorationBuiltIn) ==
                    spv::BuiltInPosition) {
                compiler.set_member_decoration(type.self, member, spv::DecorationInvariant);
            }
        }
    }
}

struct ArgumentBufferSlotCounts {
    u32 set0{};
    u32 set1{};
};

ArgumentBufferSlotCounts CountArgumentBufferSlots(const spirv_cross::CompilerMSL& compiler) {
    const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
    std::array<u32, 2> highest{0, 0};
    std::array<bool, 2> any{false, false};
    const auto note = [&](const spirv_cross::Resource& resource, bool has_sampler) {
        const u32 set = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
        if (set >= 2) {
            return;
        }
        any[set] = true;
        const spirv_cross::SPIRType& type = compiler.get_type(resource.type_id);
        u32 span = 1;
        for (size_t i = 0; i < type.array.size(); ++i) {
            if (i < type.array_size_literal.size() && type.array_size_literal[i] &&
                type.array[i] > 0) {
                span *= type.array[i];
            }
        }
        const u32 extra = span > 0 ? span - 1 : 0;
        highest[set] =
            (std::max)(highest[set], compiler.get_automatic_msl_resource_binding(resource.id) + extra);
        if (has_sampler) {
            highest[set] = (std::max)(
                highest[set],
                compiler.get_automatic_msl_resource_binding_secondary(resource.id) + extra);
        }
    };
    for (const auto& resource : resources.uniform_buffers) {
        note(resource, false);
    }
    for (const auto& resource : resources.storage_buffers) {
        note(resource, false);
    }
    for (const auto& resource : resources.sampled_images) {
        note(resource, true);
    }
    for (const auto& resource : resources.separate_images) {
        note(resource, false);
    }
    for (const auto& resource : resources.separate_samplers) {
        note(resource, false);
    }
    for (const auto& resource : resources.storage_images) {
        note(resource, false);
    }
    return {any[0] ? highest[0] + 1 : 0, any[1] ? highest[1] + 1 : 0};
}

struct MslCacheKey {
    u64 spirv_hash;
    u64 spirv_words;
    u32 msl_version;
    u32 flags;

    bool operator==(const MslCacheKey&) const noexcept = default;
};

struct MslCacheKeyHash {
    size_t operator()(const MslCacheKey& key) const noexcept {
        return static_cast<size_t>(key.spirv_hash ^ (key.spirv_words * 0x9E3779B97F4A7C15ull) ^
                                   (static_cast<u64>(key.msl_version) << 32) ^
                                   (static_cast<u64>(key.flags) * 0xBF58476D1CE4E5B9ull));
    }
};

constexpr size_t MAX_MSL_CACHE_ENTRIES = 8192;

struct MslCacheEntry {
    MslShader shader;
    std::list<MslCacheKey>::iterator lru_it;
};

struct PendingMsl {
    std::mutex mutex;
    std::condition_variable cv;
    bool done{false};
    MslShader shader;
    std::exception_ptr exception;
};

std::mutex msl_cache_mutex;
std::list<MslCacheKey> msl_cache_lru;
std::unordered_map<MslCacheKey, MslCacheEntry, MslCacheKeyHash> msl_cache;
std::unordered_map<MslCacheKey, std::shared_ptr<PendingMsl>, MslCacheKeyHash>
    pending_msl_translations;

} // Anonymous namespace

MslShader TranslateSpirvToMsl(const Device& device, std::span<const u32> spirv,
                              Shader::Stage stage, const Shader::Info& info,
                              [[maybe_unused]] const BindingMap& bindings,
                              bool rasterizes_points, bool d24_frag_depth_equal) {
    if (spirv.empty()) {
        throw Exception("Cannot translate an empty SPIR-V module");
    }

    const bool apply_d24_depth_equal =
        d24_frag_depth_equal && stage == Shader::Stage::Fragment && info.stores_frag_depth;
    const u32 flags = (static_cast<u32>(stage) & 0xffu) | (rasterizes_points ? (1u << 8) : 0u) |
                      (device.IsMobile() ? (1u << 9) : 0u) |
                      (device.SupportsArgumentBuffersTier2() ? (1u << 10) : 0u) |
                      (apply_d24_depth_equal ? (1u << 11) : 0u);
    const MslCacheKey cache_key{
        Common::CityHash64(reinterpret_cast<const char*>(spirv.data()),
                           spirv.size() * sizeof(u32)),
        spirv.size(),
        device.GetMslVersion(),
        flags,
    };

    std::shared_ptr<PendingMsl> wait_target;
    std::shared_ptr<PendingMsl> own_pending;
    {
        std::scoped_lock lock{msl_cache_mutex};
        if (const auto it = msl_cache.find(cache_key); it != msl_cache.end()) {
            msl_cache_lru.splice(msl_cache_lru.begin(), msl_cache_lru, it->second.lru_it);
            return it->second.shader;
        }
        if (const auto pit = pending_msl_translations.find(cache_key);
            pit != pending_msl_translations.end()) {
            wait_target = pit->second;
        } else {
            own_pending = std::make_shared<PendingMsl>();
            pending_msl_translations.emplace(cache_key, own_pending);
        }
    }

    if (wait_target) {
        // Another worker is translating this exact SPIR-V module; wait only on its
        // per-key condition variable (no cross-key thundering herd).
        std::unique_lock lock{wait_target->mutex};
        wait_target->cv.wait(lock, [&wait_target] { return wait_target->done; });
        if (wait_target->exception) {
            std::rethrow_exception(wait_target->exception);
        }
        return wait_target->shader;
    }

    struct PendingMslGuard {
        const MslCacheKey& key;
        std::shared_ptr<PendingMsl> pending;
        bool active = true;
        ~PendingMslGuard() {
            if (active) {
                {
                    std::scoped_lock lock{msl_cache_mutex};
                    pending_msl_translations.erase(key);
                }
                {
                    std::scoped_lock plock{pending->mutex};
                    pending->exception = std::current_exception();
                    pending->done = true;
                }
                pending->cv.notify_all();
            }
        }
    } pending_guard{cache_key, own_pending};

    MslShader result{};
    result.stage = stage;
    ReportUnsupportedFeatures(device, stage, info);

    try {
        spirv_cross::CompilerMSL compiler{spirv.data(), spirv.size()};

        spirv_cross::CompilerMSL::Options options = compiler.get_msl_options();

        options.platform = device.IsMobile() ? spirv_cross::CompilerMSL::Options::iOS
                                             : spirv_cross::CompilerMSL::Options::macOS;
        options.msl_version = device.GetMslVersion();

        options.argument_buffers = true;
        options.argument_buffers_tier =
            device.SupportsArgumentBuffersTier2()
                ? spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2
                : spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier1;

        // We bind every resource the recompiler recorded, so SPIRV-Cross has to lay out
        // every one too. Otherwise the dense [[id(N)]] counter shifts and we sample the
        // wrong resource.
        options.force_active_argument_buffer_resources = true;

        // Don't combine enable_decoration_binding with argument buffers, the argument
        // buffer analysis throws "nullptr" out of Variant::get.
        options.enable_decoration_binding = !options.argument_buffers;

        options.pad_fragment_output_components = true;

        // Metal driver workarounds in SPIRV-Cross:
        // - sample_dref_lod_array_as_grad: explicit level() on depth2d_array::sample_compare()
        //   is biased on Apple GPUs (shadow cascade acne).
        // - check_discarded_frag_stores: stores from already-discarded fragments can still land.
        // - force_fragment_with_side_effects_execution: prevents Metal from prematurely discarding
        //   fragments that perform storage writes before discarding.
        // - replace_recursive_inputs: avoids a Metal 3.1+ compiler infinite recursion on recursive
        //   entry-point input structs (zero overhead on non-recursive structs).
        options.sample_dref_lod_array_as_grad = true;
        options.check_discarded_frag_stores = true;
        options.force_fragment_with_side_effects_execution = true;
        options.replace_recursive_inputs = true;

        // MSL's texture1d has no mipmaps and no LOD select. ToTextureType in
        // mtl_texture_cache.cpp maps shader 1D to 2D as well, the two MUST agree.
        options.texture_1D_as_2D = true;

        options.force_native_arrays = true;

        options.texture_buffer_native = true;

        // Apple GPUs compute cube-map gradients differently, without this we get
        // seams along cube face boundaries.
        options.agx_manual_cube_grad_fixup = true;

        options.ios_use_simdgroup_functions = true;

        // A guest vertex shader may write gl_PointSize on a triangle draw, which
        // Vulkan ignores but Metal rejects the whole pipeline state for.
        options.enable_point_size_builtin = rasterizes_points;

        // Strip NoContraction on fragment shaders that do not write depth (matching
        // MoltenVK's has_slow_fragment_no_contraction). Keeping NoContraction on non-fragment
        // stages and depth-writing fragment shaders preserves bit-exact depth math across
        // Depth Pre-Pass and Base Pass (fixing DepthCompare::Equal artifacts in UE4/5 titles
        // such as Sonic Racing: CrossWorlds and Final Fantasy Resonance) without general
        // fragment shader overhead.
        if (stage == Shader::Stage::Fragment && !info.stores_frag_depth) {
            for (u32 id = 0; id < compiler.get_current_id_bound(); ++id) {
                compiler.unset_decoration(spirv_cross::ID(id), spv::DecorationNoContraction);
            }
        }

        options.invariant_float_math = false;

        // Input attachments, the pixel local reads of a color attachment, read the tile
        // through [[color(n)]] instead of a texture.
        options.use_framebuffer_fetch_subpasses = true;

        compiler.set_msl_options(options);

        const auto exec_model = ToExecutionModel(stage);
        if (exec_model == spv::ExecutionModelFragment || exec_model == spv::ExecutionModelVertex ||
            exec_model == spv::ExecutionModelGLCompute) {
            // Pin DescriptorSet 0 to ARGUMENT_BUFFER_INDEX (0).
            spirv_cross::MSLResourceBinding set0_binding{};
            set0_binding.stage = exec_model;
            set0_binding.desc_set = 0;
            set0_binding.binding = spirv_cross::kArgumentBufferBinding;
            set0_binding.msl_buffer = BindingMap::ARGUMENT_BUFFER_INDEX;
            compiler.add_msl_resource_binding(set0_binding);

            // Pin DescriptorSet 1 (direct bindless texture/sampler tables) when used.
            const bool uses_table_set =
                info.runtime_sampler_count != 0 ||
                std::ranges::any_of(info.texture_descriptors,
                                    [](const auto& d) { return d.is_direct || d.in_table_set; });
            if (uses_table_set) {
                spirv_cross::MSLResourceBinding set1_binding{};
                set1_binding.stage = exec_model;
                set1_binding.desc_set = 1;
                set1_binding.binding = spirv_cross::kArgumentBufferBinding;
                set1_binding.msl_buffer =
                    exec_model == spv::ExecutionModelFragment
                        ? BindingMap::FRAGMENT_TABLE_ARGUMENT_BUFFER_INDEX
                        : (exec_model == spv::ExecutionModelGLCompute
                               ? BindingMap::COMPUTE_TABLE_ARGUMENT_BUFFER_INDEX
                               : BindingMap::VERTEX_TABLE_ARGUMENT_BUFFER_INDEX);
                compiler.add_msl_resource_binding(set1_binding);
            }

            // The push constant block holding RescalingLayout (and RenderAreaLayout on
            // graphics stages).
            spirv_cross::MSLResourceBinding push_constants{};
            push_constants.stage = exec_model;
            push_constants.desc_set = spirv_cross::kPushConstDescSet;
            push_constants.binding = spirv_cross::kPushConstBinding;
            push_constants.msl_buffer = exec_model == spv::ExecutionModelFragment
                                            ? BindingMap::FRAGMENT_PUSH_CONSTANT_INDEX
                                            : (exec_model == spv::ExecutionModelGLCompute
                                                   ? BindingMap::COMPUTE_PUSH_CONSTANT_INDEX
                                                   : BindingMap::VERTEX_PUSH_CONSTANT_INDEX);
            compiler.add_msl_resource_binding(push_constants);
        }

        if (ToExecutionModel(stage) != spv::ExecutionModelFragment &&
            ToExecutionModel(stage) != spv::ExecutionModelGLCompute) {
            ForcePositionInvariance(compiler);
        }

        result.source = compiler.compile();

        result.entry_point = compiler.get_cleansed_entry_point_name(
            compiler.get_entry_points_and_stages().front().name, ToExecutionModel(stage));

        // On D24 -> D32F promoted depth buffers, read-only Equal depth tests with custom
        // gl_FragDepth can still fail on ~3-5% of pixels that straddle a D24 rounding
        // boundary due to ~1-2 ULP interpolation differences between depth-only and MRT
        // passes on Apple Silicon. Emulate a ±1.5 D24 ULP Equal check against the read-only
        // depth attachment bound at [[texture(0)]] so straddling pixels pass while occluded
        // or discarded geometry is still strictly rejected.
        if (apply_d24_depth_equal) {
            const std::string sig_needle = " " + result.entry_point + "(";
            const size_t fn_pos = result.source.find(sig_needle);
            if (fn_pos != std::string::npos) {
                const size_t open_paren = fn_pos + sig_needle.size() - 1;
                size_t close_paren = std::string::npos;
                int paren_depth = 0;
                for (size_t i = open_paren; i < result.source.size(); ++i) {
                    if (result.source[i] == '(') {
                        ++paren_depth;
                    } else if (result.source[i] == ')') {
                        if (--paren_depth == 0) {
                            close_paren = i;
                            break;
                        }
                    }
                }
                if (close_paren != std::string::npos) {
                    const std::string_view params(result.source.data() + open_paren + 1,
                                                  close_paren - open_paren - 1);
                    std::string extra_params;
                    if (!params.empty()) {
                        extra_params += ", ";
                    }
                    if (params.find("[[position]]") == std::string_view::npos) {
                        extra_params += "float4 gl_FragCoord [[position]], ";
                    }
                    extra_params += "depth2d<float> spv_d24_depth_tex [[texture(0)]]";
                    result.source.insert(close_paren, extra_params);

                    // 16 D24 ULPs (~9.54e-7): covers the ~1-2 ULP interpolation delta
                    // plus foreground Reverse-Z amplification in custom PDO gl_FragDepth
                    // without letting occluded/discarded sprites bleed through.
                    static constexpr std::string_view ret_needle = "return out;";
                    static constexpr std::string_view check_snippet =
                        "if (abs(out.gl_FragDepth - "
                        "spv_d24_depth_tex.read(uint2(gl_FragCoord.xy), 0)) > "
                        "(16.0f / 16777215.0f)) { discard_fragment(); }\n    return out;";
                    size_t pos = close_paren + extra_params.size();
                    while ((pos = result.source.find(ret_needle, pos)) != std::string::npos) {
                        result.source.replace(pos, ret_needle.size(), check_snippet);
                        pos += check_snippet.size();
                    }
                }
            }
        }

        if (options.argument_buffers) {
            const ArgumentBufferSlotCounts counts = CountArgumentBufferSlots(compiler);
            result.argument_buffer_slots = counts.set0;
            result.table_argument_buffer_slots = counts.set1;
        }
    } catch (const spirv_cross::CompilerError& error) {
        // A bare "nullptr" is Variant::get asked for an ID that doesn't exist.
        throw Exception(std::string{"SPIRV-Cross failed: "} + error.what());
    } catch (const std::exception& error) {
        throw Exception(std::string{"SPIR-V to MSL translation failed: "} + error.what());
    }

    if (result.source.empty()) {
        throw Exception("SPIRV-Cross produced no output");
    }

    {
        std::scoped_lock lock{msl_cache_mutex};
        pending_msl_translations.erase(cache_key);
        pending_guard.active = false;
        if (!msl_cache.contains(cache_key)) {
            while (msl_cache.size() >= MAX_MSL_CACHE_ENTRIES && !msl_cache_lru.empty()) {
                msl_cache.erase(msl_cache_lru.back());
                msl_cache_lru.pop_back();
            }
            msl_cache_lru.push_front(cache_key);
            msl_cache.emplace(cache_key, MslCacheEntry{result, msl_cache_lru.begin()});
        }
    }
    {
        std::scoped_lock plock{own_pending->mutex};
        own_pending->shader = result;
        own_pending->done = true;
    }
    own_pending->cv.notify_all();

    return result;
}

namespace {
/// One member of the generated `struct main0_in`.
struct StageInMember {
    std::string type;
    std::string name;
    u32 location{};    ///< The N in [[attribute(N)]]
};

std::vector<StageInMember> ParseStageInMembers(const std::string& source) {
    std::vector<StageInMember> members;
    const size_t begin = source.find("struct main0_in");
    if (begin == std::string::npos) {
        return members;
    }
    const size_t end = source.find("};", begin);
    if (end == std::string::npos) {
        return members;
    }

    size_t cursor = begin;
    while (cursor < end) {
        const size_t line_end = std::min(source.find('\n', cursor), end);
        const std::string_view line{source.data() + cursor, line_end - cursor};
        cursor = line_end + 1;

        const size_t attribute = line.find("[[attribute(");
        if (attribute == std::string_view::npos) {
            continue;
        }
        const size_t number = attribute + std::string_view{"[[attribute("}.size();
        const size_t number_end = line.find(')', number);
        if (number_end == std::string_view::npos) {
            continue;
        }

        std::string_view head = line.substr(0, attribute);
        while (!head.empty() && (head.front() == ' ' || head.front() == '\t')) {
            head.remove_prefix(1);
        }
        while (!head.empty() && head.back() == ' ') {
            head.remove_suffix(1);
        }
        const size_t space = head.find(' ');
        if (space == std::string_view::npos) {
            continue;
        }

        StageInMember member{};
        member.type = std::string{head.substr(0, space)};
        member.name = std::string{head.substr(space + 1)};
        member.location = static_cast<u32>(
            std::strtoul(std::string{line.substr(number, number_end - number)}.c_str(), nullptr, 10));
        if (!member.name.empty()) {
            members.push_back(std::move(member));
        }
    }
    return members;
}

struct FormatLayout {
    const char* scalar{nullptr};
    u32 components{0};
    bool normalized{false};
    bool is_signed{false};
    /// For formats that are one packed word: an MSL expression taking `w` as that word.
    const char* packed_expression{nullptr};
};

FormatLayout LayoutOf(MTL::VertexFormat format) {
    using F = MTL::VertexFormat;
    switch (format) {
    case F::VertexFormatUChar:              return {"uchar", 1, false, false, nullptr};
    case F::VertexFormatUChar2:             return {"uchar", 2, false, false, nullptr};
    case F::VertexFormatUChar3:             return {"uchar", 3, false, false, nullptr};
    case F::VertexFormatUChar4:             return {"uchar", 4, false, false, nullptr};
    case F::VertexFormatUCharNormalized:    return {"uchar", 1, true, false, nullptr};
    case F::VertexFormatUChar2Normalized:   return {"uchar", 2, true, false, nullptr};
    case F::VertexFormatUChar3Normalized:   return {"uchar", 3, true, false, nullptr};
    case F::VertexFormatUChar4Normalized:   return {"uchar", 4, true, false, nullptr};
    case F::VertexFormatChar:               return {"char", 1, false, true, nullptr};
    case F::VertexFormatChar2:              return {"char", 2, false, true, nullptr};
    case F::VertexFormatChar3:              return {"char", 3, false, true, nullptr};
    case F::VertexFormatChar4:              return {"char", 4, false, true, nullptr};
    case F::VertexFormatCharNormalized:     return {"char", 1, true, true, nullptr};
    case F::VertexFormatChar2Normalized:    return {"char", 2, true, true, nullptr};
    case F::VertexFormatChar3Normalized:    return {"char", 3, true, true, nullptr};
    case F::VertexFormatChar4Normalized:    return {"char", 4, true, true, nullptr};
    case F::VertexFormatUShort:             return {"ushort", 1, false, false, nullptr};
    case F::VertexFormatUShort2:            return {"ushort", 2, false, false, nullptr};
    case F::VertexFormatUShort3:            return {"ushort", 3, false, false, nullptr};
    case F::VertexFormatUShort4:            return {"ushort", 4, false, false, nullptr};
    case F::VertexFormatUShortNormalized:   return {"ushort", 1, true, false, nullptr};
    case F::VertexFormatUShort2Normalized:  return {"ushort", 2, true, false, nullptr};
    case F::VertexFormatUShort3Normalized:  return {"ushort", 3, true, false, nullptr};
    case F::VertexFormatUShort4Normalized:  return {"ushort", 4, true, false, nullptr};
    case F::VertexFormatShort:              return {"short", 1, false, true, nullptr};
    case F::VertexFormatShort2:             return {"short", 2, false, true, nullptr};
    case F::VertexFormatShort3:             return {"short", 3, false, true, nullptr};
    case F::VertexFormatShort4:             return {"short", 4, false, true, nullptr};
    case F::VertexFormatShortNormalized:    return {"short", 1, true, true, nullptr};
    case F::VertexFormatShort2Normalized:   return {"short", 2, true, true, nullptr};
    case F::VertexFormatShort3Normalized:   return {"short", 3, true, true, nullptr};
    case F::VertexFormatShort4Normalized:   return {"short", 4, true, true, nullptr};
    case F::VertexFormatHalf:               return {"half", 1, false, true, nullptr};
    case F::VertexFormatHalf2:              return {"half", 2, false, true, nullptr};
    case F::VertexFormatHalf3:              return {"half", 3, false, true, nullptr};
    case F::VertexFormatHalf4:              return {"half", 4, false, true, nullptr};
    case F::VertexFormatFloat:              return {"float", 1, false, true, nullptr};
    case F::VertexFormatFloat2:             return {"float", 2, false, true, nullptr};
    case F::VertexFormatFloat3:             return {"float", 3, false, true, nullptr};
    case F::VertexFormatFloat4:             return {"float", 4, false, true, nullptr};
    case F::VertexFormatInt:                return {"int", 1, false, true, nullptr};
    case F::VertexFormatInt2:               return {"int", 2, false, true, nullptr};
    case F::VertexFormatInt3:               return {"int", 3, false, true, nullptr};
    case F::VertexFormatInt4:               return {"int", 4, false, true, nullptr};
    case F::VertexFormatUInt:               return {"uint", 1, false, false, nullptr};
    case F::VertexFormatUInt2:              return {"uint", 2, false, false, nullptr};
    case F::VertexFormatUInt3:              return {"uint", 3, false, false, nullptr};
    case F::VertexFormatUInt4:              return {"uint", 4, false, false, nullptr};
    // Single 32-bit words. No MSL type for them so we unpack by hand, component
    // order matches Metal's own decode.
    case F::VertexFormatUInt1010102Normalized:
        return {nullptr, 4, true, false,
                "float4(float(w & 1023u) / 1023.0, float((w >> 10) & 1023u) / 1023.0, "
                "float((w >> 20) & 1023u) / 1023.0, float((w >> 30) & 3u) / 3.0)"};
    case F::VertexFormatInt1010102Normalized:
        return {nullptr, 4, true, true,
                "float4(max(float(int(w << 22) >> 22) / 511.0, -1.0), "
                "max(float(int(w << 12) >> 22) / 511.0, -1.0), "
                "max(float(int(w << 2) >> 22) / 511.0, -1.0), "
                "max(float(int(w) >> 30), -1.0))"};
    case F::VertexFormatFloatRG11B10:
        return {nullptr, 3, false, true,
                "float4(float(as_type<half>(ushort((w & 2047u) << 4))), "
                "float(as_type<half>(ushort(((w >> 11) & 2047u) << 4))), "
                "float(as_type<half>(ushort(((w >> 22) & 1023u) << 5))), 1.0)"};
    default:
        return {};
    }
}

u32 FetchedBytes(const FormatLayout& layout) {
    if (layout.packed_expression != nullptr) {
        return 4;
    }
    const std::string_view scalar{layout.scalar};
    const u32 width = (scalar == "uchar" || scalar == "char")     ? 1
                      : (scalar == "ushort" || scalar == "short" || scalar == "half") ? 2
                                                                                      : 4;
    return width * layout.components;
}

// MSL expression for one attribute's value: decode `format` at `base`, then widen to
// four components filling with (0, 0, 0, 1) the way MTLVertexDescriptor would.
std::string EmitAttributeFetch(MTL::VertexFormat format, const std::string& base,
                               const std::string& target_type, const std::string& guard) {
    const FormatLayout layout = LayoutOf(format);
    if (layout.components == 0) {
        return {};
    }

    const bool integral_target = target_type.rfind("float", 0) != 0;

    if (layout.packed_expression != nullptr) {
        // These only ever decode to floats.
        if (integral_target) {
            return {};
        }
        const std::string unpacked = fmt::format(
            "({{ const uint w = *(device const uint*)({}); {}; }})", base,
            layout.packed_expression);
        return fmt::format("(({} + 4u <= {}) ? ({}) : {}(0.0, 0.0, 0.0, 1.0))", guard, "spvLimit",
                           unpacked, target_type);
    }

    // packed_* so an attribute left unaligned by the stream's stride still loads. The
    // natural vector types want alignment the guest never promises.
    const std::string load =
        layout.components == 1
            ? fmt::format("(*(device const {}*)({}))", layout.scalar, base)
            : fmt::format("(*(device const packed_{}{}*)({}))", layout.scalar,
                          layout.components, base);
    const std::string comp_suffix =
        layout.components == 1 ? std::string{} : std::to_string(layout.components);

    std::string value;
    if (integral_target) {
        value = fmt::format("{}{}({})", target_type.substr(0, target_type.size() - 1), comp_suffix,
                            load);
    } else if (layout.normalized) {
        const double denominator = layout.is_signed ? ((1u << (
            std::string_view{layout.scalar} == "char" ? 7 : 15)) - 1)
                                                    : ((std::string_view{layout.scalar} == "uchar"
                                                            ? 255u
                                                            : 65535u));
        value = fmt::format("float{}({}) / {:.1f}", comp_suffix, load, denominator);
        if (layout.is_signed) {
            value = fmt::format("max({}, float{}(-1.0))", value, comp_suffix);
        }
    } else {
        value = fmt::format("float{}({})", comp_suffix, load);
    }

    const std::string one = integral_target ? "1" : "1.0";
    const std::string zero = integral_target ? "0" : "0.0";
    std::string widened;
    switch (layout.components) {
    case 1:
        widened = fmt::format("{}({}, {}, {}, {})", target_type, value, zero, zero, one);
        break;
    case 2:
        widened = fmt::format("{}({}, {}, {})", target_type, value, zero, one);
        break;
    case 3:
        widened = fmt::format("{}({}, {})", target_type, value, one);
        break;
    default:
        widened = fmt::format("{}({})", target_type, value);
        break;
    }

    // Metal's stage-in bounds checked this for us, raw addressing doesn't, and an
    // unchecked read faults the GPU instead of just drawing wrong.
    return fmt::format("(({} + {}u <= {}) ? ({}) : {}({}, {}, {}, {}))", guard,
                       FetchedBytes(layout), "spvLimit", widened, target_type, zero, zero, zero,
                       one);
}

// Replace the capture kernel's [[stage_in]] input with an explicit fetch. Metal 4
// dropped compute stage-in, and a kernel declaring it doesn't fail, every attribute
// just reads zero. Returns false and leaves source alone when the layout can't be
// expressed, the caller then declines the capture instead of reading wrong memory.
bool InjectManualVertexFetch(std::string& source, const FixedPipelineState& state) {
    const std::vector<StageInMember> members = ParseStageInMembers(source);
    if (members.empty()) {
        // Nothing to fetch, and no stage-in parameter to remove.
        return source.find("[[stage_in]]") == std::string::npos;
    }

    std::string parameters;
    std::string body;
    std::array<bool, MAX_VERTEX_BUFFERS> wanted{};

    for (const StageInMember& member : members) {
        if (member.location >= MAX_VERTEX_ATTRIBUTES) {
            return false;
        }
        const auto& attribute = state.attributes[member.location];
        if (attribute.format == MTL::VertexFormatInvalid) {
            // Declared by the shader, not supplied by the guest.
            body += fmt::format("    in.{} = {}(0, 0, 0, 1);\n", member.name, member.type);
            continue;
        }
        if (attribute.buffer_index >= MAX_VERTEX_BUFFERS) {
            return false;
        }
        const auto& binding = state.bindings[attribute.buffer_index];
        const u32 slot = BindingMap::VERTEX_BUFFER_BASE + attribute.buffer_index;
        if (!binding.enabled || slot > BindingMap::MAX_VERTEX_BUFFER_SLOT ||
            IsCaptureReservedBufferSlot(slot)) {
            return false;
        }

        const std::string index =
            binding.divisor == 0
                ? "spvVertexIndex"
                : (binding.divisor == 1
                       ? fmt::format("(spvInstanceIndex + spvStreamLimits[{}])",
                                     CAPTURE_DRAW_BASE_SLOT + 1)
                       : fmt::format("((spvInstanceIndex / {}u) + spvStreamLimits[{}])",
                                     binding.divisor, CAPTURE_DRAW_BASE_SLOT + 1));

        const std::string offset = fmt::format("{}u + {}u * {}", attribute.offset, binding.stride,
                                               index);
        const std::string base = fmt::format("spvStream{} + spvOffset", attribute.buffer_index);
        const std::string fetch =
            EmitAttributeFetch(attribute.format, base, member.type, "spvOffset");
        if (fetch.empty()) {
            return false;
        }
        wanted[attribute.buffer_index] = true;
        body += fmt::format("    {{\n        const uint spvOffset = {};\n"
                            "        const uint spvLimit = spvStreamLimits[{}];\n"
                            "        in.{} = {};\n    }}\n",
                            offset, attribute.buffer_index, member.name, fetch);
    }

    for (u32 buffer = 0; buffer < MAX_VERTEX_BUFFERS; ++buffer) {
        if (wanted[buffer]) {
            parameters += fmt::format("device const uchar* spvStream{} [[buffer({})]], ", buffer,
                                      BindingMap::VERTEX_BUFFER_BASE + buffer);
        }
    }
    parameters += fmt::format("device const uint* spvOrder [[buffer({})]], ",
                              CAPTURE_INDEX_BUFFER_INDEX);
    parameters += fmt::format("constant uint* spvStreamLimits [[buffer({})]], ",
                              CAPTURE_LIMITS_BUFFER_INDEX);

    const std::string stage_in = "main0_in in [[stage_in]], ";
    const size_t at = source.find(stage_in);
    if (at == std::string::npos) {
        return false;
    }
    source.replace(at, stage_in.size(), parameters);

    // [[grid_size]] is the STAGE-IN region size, which SPIRV-Cross bounds checks every
    // thread against. With no stage-in it reads zero and every thread returns before its
    // first store. [[threads_per_grid]] is what we pass to dispatchThreads.
    const std::string grid_size = "[[grid_size]]";
    const size_t grid_at = source.find(grid_size);
    if (grid_at == std::string::npos) {
        return false;
    }
    source.replace(grid_at, grid_size.size(), "[[threads_per_grid]]");

    const std::string guard = "    if (any(gl_GlobalInvocationID >= spvStageInputSize))\n"
                              "        return;\n";
    const size_t guard_at = source.find(guard);
    if (guard_at == std::string::npos) {
        return false;
    }
    const std::string prologue =
        "    const uint spvVertexIndex = spvOrder[gl_GlobalInvocationID.x];\n"
        "    const uint spvInstanceIndex = gl_GlobalInvocationID.y;\n"
        "    main0_in in;\n" +
        body;
    source.insert(guard_at + guard.size(), prologue);
    return true;
}

/// SPIRV-Cross takes the base vertex and instance of a captured vertex shader from
/// [[grid_origin]], which a Metal 4 compute encoder cannot set, so they always read zero.
/// Guest shaders index bone palettes with them, which skinned every mesh with the first
/// one's bones. They come from the limits buffer instead, see CAPTURE_DRAW_BASE_SLOT. The
/// vertex index is the expanded index the attributes are fetched with, not the thread id.
bool ProvideCaptureDrawParameters(std::string& source) {
    const std::string param = "uint3 spvDispatchBase [[grid_origin]]";
    const size_t param_at = source.find(param);
    if (param_at == std::string::npos) {
        return true;
    }
    size_t erase_at = param_at;
    size_t erase_len = param.size();
    if (source.compare(param_at + param.size(), 2, ", ") == 0) {
        erase_len += 2;
    } else if (param_at >= 2 && source.compare(param_at - 2, 2, ", ") == 0) {
        erase_at -= 2;
        erase_len += 2;
    }
    source.erase(erase_at, erase_len);

    const size_t body_at = source.find("\n{\n", erase_at);
    if (body_at == std::string::npos) {
        return false;
    }
    source.insert(body_at + 3,
                  fmt::format("    const uint3 spvDispatchBase = uint3(spvStreamLimits[{}], "
                              "spvStreamLimits[{}], 0u);\n",
                              CAPTURE_DRAW_BASE_SLOT, CAPTURE_DRAW_BASE_SLOT + 1));

    const std::string thread_index = "gl_GlobalInvocationID.x + spvDispatchBase.x;";
    for (size_t at = source.find(thread_index); at != std::string::npos;
         at = source.find(thread_index, at)) {
        source.replace(at, thread_index.size(), "spvOrder[gl_GlobalInvocationID.x];");
    }
    return true;
}

} // Anonymous namespace

MslShader TranslateVertexToCaptureKernel(const Device& device, std::span<const u32> spirv,
                                         const Shader::Info& info,
                                         [[maybe_unused]] const BindingMap& bindings,
                                         const FixedPipelineState& state) {
    if (spirv.empty()) {
        throw Exception("Cannot translate an empty SPIR-V module");
    }

    MslShader result{};
    // A compute shader, despite starting life as a vertex program.
    result.stage = Shader::Stage::Compute;

    try {
        spirv_cross::CompilerMSL compiler{spirv.data(), spirv.size()};
        spirv_cross::CompilerMSL::Options options = compiler.get_msl_options();

        options.platform = device.IsMobile() ? spirv_cross::CompilerMSL::Options::iOS
                                             : spirv_cross::CompilerMSL::Options::macOS;
        options.msl_version = device.GetMslVersion();

        // Emit the vertex program as a kernel writing its outputs to a buffer.
        options.vertex_for_tessellation = true;
        options.capture_output_to_buffer = true;
        // Explicit rather than relying on SPIRV-Cross's identical default.
        options.shader_output_buffer_index = CAPTURE_OUTPUT_BUFFER_INDEX;
        // A capture rasterizes nothing.
        options.disable_rasterization = true;

        options.argument_buffers = true;
        options.argument_buffers_tier =
            device.SupportsArgumentBuffersTier2()
                ? spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2
                : spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier1;

        options.force_active_argument_buffer_resources = true;
        options.enable_decoration_binding = !options.argument_buffers;
        options.sample_dref_lod_array_as_grad = true;
        options.replace_recursive_inputs = true;
        // Must agree with ToTextureType in mtl_texture_cache.cpp (1D textures are bound as 2D).
        options.texture_1D_as_2D = true;
        options.force_native_arrays = true;
        options.texture_buffer_native = true;
        options.agx_manual_cube_grad_fixup = true;
        options.ios_use_simdgroup_functions = true;

        compiler.set_msl_options(options);

        result.source = compiler.compile();

        if (!InjectManualVertexFetch(result.source, state)) {
            static std::atomic<u32> reported{0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
                LOG_WARNING(Shader_MSL,
                            "Vertex capture declined: the guest's attribute layout cannot be "
                            "fetched without stage-in");
            }
            // Empty source is how every caller spells "no capture kernel".
            return {};
        }
        if (!ProvideCaptureDrawParameters(result.source)) {
            return {};
        }

        // Vertex, not GLCompute, even though the MSL we get is a kernel:
        // capture_output_to_buffer only changes what SPIRV-Cross emits.
        result.entry_point = compiler.get_cleansed_entry_point_name(
            compiler.get_entry_points_and_stages().front().name, spv::ExecutionModelVertex);

        const ArgumentBufferSlotCounts counts = CountArgumentBufferSlots(compiler);
        result.argument_buffer_slots = counts.set0;
        result.table_argument_buffer_slots = counts.set1;
    } catch (const spirv_cross::CompilerError& error) {
        throw Exception(std::string{"SPIRV-Cross failed to build a capture kernel: "} +
                        error.what());
    } catch (const std::exception& error) {
        throw Exception(std::string{"Capture kernel translation failed: "} + error.what());
    }

    if (result.source.empty()) {
        throw Exception("SPIRV-Cross produced no capture kernel");
    }
    (void)info;
    return result;
}

namespace {
// Compiled MSL keyed by hash + length of its source (not the text itself, keeping
// every shader's source alive costs tens of MB). One guest shader serves many
// pipelines and each was calling newLibrary() on identical text, ~70s of a 150s run.
// The pipeline archive doesn't help, it's only consulted at newRenderPipelineState
// and the library has to exist by then.
struct LibraryKey {
    u64 device_id;
    u32 msl_version;
    u64 hash;
    u64 length;

    bool operator==(const LibraryKey&) const noexcept = default;
};

struct LibraryKeyHash {
    size_t operator()(const LibraryKey& key) const noexcept {
        return static_cast<size_t>(key.hash ^ (key.length * 0x9E3779B97F4A7C15ull) ^
                                   (key.device_id * 0xBF58476D1CE4E5B9ull) ^
                                   (static_cast<u64>(key.msl_version) << 32));
    }
};

constexpr size_t MAX_LIBRARY_CACHE_ENTRIES = 8192;

struct LibraryCacheEntry {
    Owned<MTL::Library> library;
    std::list<LibraryKey>::iterator lru_it;
};

struct PendingLibrary {
    std::mutex mutex;
    std::condition_variable cv;
    bool done{false};
    Owned<MTL::Library> library;
};

std::mutex library_cache_mutex;
std::list<LibraryKey> library_cache_lru;
std::unordered_map<LibraryKey, LibraryCacheEntry, LibraryKeyHash> library_cache;
std::unordered_map<LibraryKey, std::shared_ptr<PendingLibrary>, LibraryKeyHash> pending_libraries;

std::shared_mutex library_constants_mutex;
std::unordered_map<const MTL::Library*, bool> library_has_constants;

} // Anonymous namespace

void ClearLibraryCache(u64 device_registry_id) {
    {
        std::scoped_lock lock{msl_cache_mutex};
        msl_cache.clear();
        msl_cache_lru.clear();
    }
    if (device_registry_id == 0) {
        std::vector<Owned<MTL::Library>> evicted;
        {
            std::scoped_lock lock{library_cache_mutex};
            evicted.reserve(library_cache.size());
            for (auto& [k, entry] : library_cache) {
                evicted.push_back(std::move(entry.library));
            }
            library_cache.clear();
            library_cache_lru.clear();
        }
        {
            std::unique_lock lock{library_constants_mutex};
            library_has_constants.clear();
        }
        return;
    }
    std::vector<Owned<MTL::Library>> evicted;
    {
        std::scoped_lock lock{library_cache_mutex};
        std::erase_if(library_cache, [device_registry_id, &evicted](auto& entry) {
            if (entry.first.device_id == device_registry_id) {
                library_cache_lru.erase(entry.second.lru_it);
                evicted.push_back(std::move(entry.second.library));
                return true;
            }
            return false;
        });
    }
    if (!evicted.empty()) {
        std::unique_lock lock{library_constants_mutex};
        for (const auto& lib : evicted) {
            library_has_constants.erase(lib.get());
        }
    }
}

Owned<MTL::Library> BuildLibrary(const Device& device, const MslShader& shader) {
    const LibraryKey key{device.GetDevice() ? device.GetDevice()->registryID() : 0,
                         device.GetMslVersion(),
                         Common::CityHash64(shader.source.data(), shader.source.size()),
                         shader.source.size()};
    std::shared_ptr<PendingLibrary> wait_target;
    std::shared_ptr<PendingLibrary> own_pending;
    {
        std::scoped_lock lock{library_cache_mutex};
        if (const auto it = library_cache.find(key); it != library_cache.end()) {
            library_cache_lru.splice(library_cache_lru.begin(), library_cache_lru,
                                     it->second.lru_it);
            return it->second.library;
        }
        if (const auto pit = pending_libraries.find(key); pit != pending_libraries.end()) {
            wait_target = pit->second;
        } else {
            own_pending = std::make_shared<PendingLibrary>();
            pending_libraries.emplace(key, own_pending);
        }
    }

    if (wait_target) {
        // Another worker is already compiling this exact MSL source; wait only on its
        // per-key condition variable instead of waking on unrelated libraries.
        std::unique_lock lock{wait_target->mutex};
        wait_target->cv.wait(lock, [&wait_target] { return wait_target->done; });
        return wait_target->library;
    }

    struct PendingGuard {
        const LibraryKey& key;
        std::shared_ptr<PendingLibrary> pending;
        bool active = true;
        ~PendingGuard() {
            if (active) {
                {
                    std::scoped_lock lock{library_cache_mutex};
                    pending_libraries.erase(key);
                }
                {
                    std::scoped_lock plock{pending->mutex};
                    pending->done = true;
                }
                pending->cv.notify_all();
            }
        }
    } pending_guard{key, own_pending};

    if (const char* dir = std::getenv("NXEMU_METAL_DUMP_DIR"); dir != nullptr && *dir != '\0') {
        std::ofstream dump(std::filesystem::path{dir} / fmt::format("{:016x}.metal", key.hash),
                           std::ios::binary);
        dump.write(shader.source.data(), static_cast<std::streamsize>(shader.source.size()));
    }

    ScopedAutoreleasePool pool;

    MTL::CompileOptions* options = MTL::CompileOptions::alloc()->init();
    options->setLanguageVersion(ToLanguageVersion(device.GetMslVersion()));

    // Match MoltenVK's default MTLMathModeSafe (MVK_CONFIG_FAST_MATH_ON_DEMAND on SPIR-V
    // without FastMath flags). Relaxed fast math breaks IEEE-754 NaN/Inf/subnormal clamps in
    // HDR lighting and bloom shaders (e.g. Sonic x Shadow Generations) while preserveInvariance
    // keeps depth pre-passes and shading passes bit-identical.
    options->setPreserveInvariance(true);
    options->setFastMathEnabled(false);
    options->setMathMode(MTL::MathModeSafe);
    LOG_DEBUG(Render_Metal, "Shader math mode {}", static_cast<int>(options->mathMode()));

    NS::Error* error = nullptr;
    Owned<MTL::Library> library;
    NS::String* const source_ns = ToNSString(shader.source);
    if (MTL4::Compiler* const compiler = device.GetMTL4UtilityCompiler()) {
        MTL4::LibraryDescriptor* descriptor = MTL4::LibraryDescriptor::alloc()->init();
        descriptor->setSource(source_ns);
        descriptor->setOptions(options);
        library = Adopt(compiler->newLibrary(descriptor, &error));
        descriptor->release();
    } else {
        library = Adopt(device.GetDevice()->newLibrary(source_ns, options, &error));
    }

    options->release();

    if (!library) {
        LOG_ERROR(Shader_MSL, "Failed to compile MSL: {}", DescribeError(error));
        return {};
    }

    Owned<MTL::Library> result;
    std::vector<Owned<MTL::Library>> evicted;
    {
        std::scoped_lock lock{library_cache_mutex};
        pending_libraries.erase(key);
        pending_guard.active = false;
        if (const auto it = library_cache.find(key); it != library_cache.end()) {
            library_cache_lru.splice(library_cache_lru.begin(), library_cache_lru,
                                     it->second.lru_it);
            result = it->second.library;
        } else {
            while (library_cache.size() >= MAX_LIBRARY_CACHE_ENTRIES &&
                   !library_cache_lru.empty()) {
                const LibraryKey& oldest = library_cache_lru.back();
                if (const auto old_it = library_cache.find(oldest);
                    old_it != library_cache.end()) {
                    evicted.push_back(std::move(old_it->second.library));
                    library_cache.erase(old_it);
                }
                library_cache_lru.pop_back();
            }
            library_cache_lru.push_front(key);
            result = library_cache
                         .try_emplace(key, LibraryCacheEntry{std::move(library),
                                                             library_cache_lru.begin()})
                         .first->second.library;
        }
    }
    if (!evicted.empty()) {
        std::unique_lock lock{library_constants_mutex};
        for (const auto& lib : evicted) {
            library_has_constants.erase(lib.get());
        }
    }
    {
        std::scoped_lock plock{own_pending->mutex};
        own_pending->library = result;
        own_pending->done = true;
    }
    own_pending->cv.notify_all();
    return result;
}

namespace {

/// Persistent per-thread 8 MiB stack worker for SPIRV-Cross translation.
/// Avoids pthread_create / 8 MiB stack mmap+munmap / pthread_join on every shader stage.
class LargeStackWorker {
public:
    ~LargeStackWorker() {
        if (!started) {
            return;
        }
        {
            std::scoped_lock lock{mutex};
            stop = true;
            has_task = true;
        }
        cv.notify_one();
        pthread_join(thread, nullptr);
    }

    MslShader Run(std::function<MslShader()>& translate) {
        if (!EnsureStarted()) {
            return translate();
        }
        std::unique_lock lock{mutex};
        current_translate = &translate;
        result = MslShader{};
        exception = nullptr;
        done = false;
        has_task = true;
        cv.notify_one();
        cv.wait(lock, [this] { return done; });
        std::exception_ptr ex = std::move(exception);
        MslShader out = std::move(result);
        current_translate = nullptr;
        if (ex) {
            std::rethrow_exception(ex);
        }
        return out;
    }

private:
    bool EnsureStarted() {
        if (started) {
            return true;
        }
        if (init_failed) {
            return false;
        }
        pthread_attr_t attributes;
        if (pthread_attr_init(&attributes) != 0) {
            LOG_WARNING(Shader_MSL, "Could not init thread attributes; translating inline");
            init_failed = true;
            return false;
        }
        pthread_attr_setstacksize(&attributes, SPIRV_CROSS_STACK_SIZE);
        const int created = pthread_create(&thread, &attributes, &LargeStackWorker::Entry, this);
        pthread_attr_destroy(&attributes);
        if (created != 0) {
            LOG_WARNING(Shader_MSL, "Could not spawn a large-stack thread; translating inline");
            init_failed = true;
            return false;
        }
        started = true;
        return true;
    }

    static void* Entry(void* raw) {
        auto* self = static_cast<LargeStackWorker*>(raw);
        std::unique_lock lock{self->mutex};
        while (true) {
            self->cv.wait(lock, [self] { return self->has_task; });
            if (self->stop) {
                break;
            }
            auto* fn = self->current_translate;
            self->has_task = false;
            lock.unlock();

            MslShader local_result{};
            std::exception_ptr local_exception{};
            try {
                local_result = (*fn)();
            } catch (...) {
                local_exception = std::current_exception();
            }

            lock.lock();
            self->result = std::move(local_result);
            self->exception = std::move(local_exception);
            self->done = true;
            self->cv.notify_one();
        }
        return nullptr;
    }

    std::mutex mutex;
    std::condition_variable cv;
    pthread_t thread{};
    std::function<MslShader()>* current_translate{};
    MslShader result{};
    std::exception_ptr exception{};
    bool started{false};
    bool init_failed{false};
    bool has_task{false};
    bool done{false};
    bool stop{false};
};

} // Anonymous namespace

MslShader RunOnLargeStack(std::function<MslShader()> translate) {
#if defined(__APPLE__)
    // Workers spawned with an 8 MiB stack (and macOS main thread) already have
    // enough stack for SPIRV-Cross; run inline and skip the condvar thread-hop.
    if (pthread_get_stacksize_np(pthread_self()) >= SPIRV_CROSS_STACK_SIZE) {
        return translate();
    }
#endif
    thread_local LargeStackWorker worker;
    return worker.Run(translate);
}

ShaderWorkerPool::ShaderWorkerPool(size_t num_workers, std::string name)
    : thread_name{std::move(name)} {
    pthread_attr_t attr;
    const bool has_attr = pthread_attr_init(&attr) == 0;
    if (has_attr) {
        pthread_attr_setstacksize(&attr, SPIRV_CROSS_STACK_SIZE);
    }
    threads.reserve(num_workers);
    for (size_t i = 0; i < num_workers; ++i) {
        pthread_t tid{};
        const int rc =
            pthread_create(&tid, has_attr ? &attr : nullptr, &ShaderWorkerPool::WorkerEntry, this);
        if (rc == 0) {
            threads.push_back(tid);
        }
    }
    if (has_attr) {
        pthread_attr_destroy(&attr);
    }
    workers_total = threads.size();
}

ShaderWorkerPool::~ShaderWorkerPool() {
    {
        std::scoped_lock lock{queue_mutex};
        stop_requested = true;
    }
    queue_cv.notify_all();
    for (pthread_t tid : threads) {
        pthread_join(tid, nullptr);
    }
}

void ShaderWorkerPool::QueueWork(Common::UniqueFunction<void> work, bool high_priority) {
    active_or_queued.fetch_add(1, std::memory_order_acq_rel);
    {
        std::scoped_lock lock{queue_mutex};
        if (high_priority) {
            requests.emplace_front(std::move(work));
        } else {
            requests.emplace_back(std::move(work));
        }
        ++work_scheduled;
    }
    queue_cv.notify_one();
}

void ShaderWorkerPool::WaitForRequests(std::stop_token stop_token) {
    std::stop_callback callback(stop_token, [this] {
        {
            std::scoped_lock lock{queue_mutex};
            stop_requested = true;
        }
        queue_cv.notify_all();
        wait_cv.notify_all();
    });
    std::unique_lock lock{queue_mutex};
    wait_cv.wait(lock, [this] {
        return workers_stopped >= workers_total || work_done >= work_scheduled || stop_requested;
    });
}

void* ShaderWorkerPool::WorkerEntry(void* raw) {
    static_cast<ShaderWorkerPool*>(raw)->WorkerLoop();
    return nullptr;
}

void ShaderWorkerPool::WorkerLoop() {
    Common::SetCurrentThreadName(thread_name.c_str());
    for (;;) {
        Common::UniqueFunction<void> task;
        {
            std::unique_lock lock{queue_mutex};
            if (requests.empty()) {
                wait_cv.notify_all();
            }
            queue_cv.wait(lock, [this] { return stop_requested || !requests.empty(); });
            if (stop_requested) {
                break;
            }
            task = std::move(requests.front());
            requests.pop_front();
        }
        {
            struct ActiveGuard {
                std::atomic<size_t>& counter;
                ~ActiveGuard() {
                    counter.fetch_sub(1, std::memory_order_acq_rel);
                }
            } active_guard{active_or_queued};
            task();
        }
        {
            std::scoped_lock lock{queue_mutex};
            ++work_done;
            if (work_done >= work_scheduled) {
                wait_cv.notify_all();
            }
        }
    }
    {
        std::scoped_lock lock{queue_mutex};
        ++workers_stopped;
    }
    wait_cv.notify_all();
}

Owned<MTL::Function> GetFunction(MTL::Library* library, const std::string& entry_point) {
    if (library == nullptr) {
        return {};
    }
    ScopedAutoreleasePool pool;

    Owned<MTL::Function> function = Adopt(library->newFunction(ToNSString(entry_point)));
    if (!function) {
        LOG_ERROR(Shader_MSL, "MSL library has no entry point named '{}'", entry_point);
        return function;
    }

    // A function declaring *any* [[function_constant]] can't build a pipeline state
    // until it's specialized, and Metal aborts the process instead of erroring.
    // SPIRV-Cross emits one behind our back (spvLinearTextureAlignmentOverride, for
    // emulated 2D image atomics). Re-creating with an empty value set is enough.
    NS::Dictionary* const declared = function->functionConstantsDictionary();
    if (declared == nullptr || declared->count() == 0) {
        return function;
    }

    Owned<MTL::FunctionConstantValues> values =
        Adopt(MTL::FunctionConstantValues::alloc()->init());

    // Large enough for any scalar or vector a function constant can hold.
    const std::array<u8, 16> zero{};

    NS::Enumerator<NS::String>* keys = declared->keyEnumerator<NS::String>();
    for (NS::String* key = keys->nextObject(); key != nullptr; key = keys->nextObject()) {
        auto* constant = declared->object<MTL::FunctionConstant>(key);
        if (constant == nullptr) {
            continue;
        }
        LOG_DEBUG(Shader_MSL, "'{}' declares function constant '{}' (index {}, required {})",
                  entry_point, FromNSString(constant->name()), constant->index(),
                  constant->required());
        if (!constant->required()) {
            continue;
        }
        values->setConstantValue(zero.data(), constant->type(), constant->index());
    }

    NS::Error* error = nullptr;
    Owned<MTL::Function> specialized =
        Adopt(library->newFunction(ToNSString(entry_point), values.get(), &error));
    if (!specialized) {
        // Returning the unspecialized function would abort the process.
        LOG_ERROR(Shader_MSL, "Could not specialize '{}': {}", entry_point, DescribeError(error));
        return {};
    }
    return specialized;
}

void SetColorAttachment(MTL4::RenderPipelineDescriptor* descriptor, u32 index,
                        MTL::PixelFormat format) {
    MTL4::RenderPipelineColorAttachmentDescriptor* target =
        MTL4::RenderPipelineColorAttachmentDescriptor::alloc()->init();
    target->setPixelFormat(format);
    descriptor->colorAttachments()->setObject(target, index);
    target->release();
}

MTL4::FunctionDescriptor* MakeFunctionDescriptor(MTL::Library* library,
                                                 const std::string& entry_point) {
    MTL4::LibraryFunctionDescriptor* base = MTL4::LibraryFunctionDescriptor::alloc()->init();
    base->setLibrary(library);
    base->setName(ToNSString(entry_point));
    base->autorelease();

    if (library == nullptr) {
        return base;
    }

    // Fast path: most libraries have a single entry point with zero function constants.
    // Once probed, avoid calling library->newFunction() on every pipeline sharing it.
    // Uses a dedicated shared_mutex so concurrent pipeline builds never contend with
    // each other or with BuildLibrary().
    {
        std::shared_lock lock{library_constants_mutex};
        if (const auto it = library_has_constants.find(library);
            it != library_has_constants.end() && !it->second) {
            return base;
        }
    }

    // Same hazard GetFunction guards against, different route: Metal 4 never takes an
    // MTL::Function. Metal doesn't ask whether required constants have values, it asks
    // whether the function was created with a constant-values object at all, and
    // spvLinearTextureAlignmentOverride is optional, so specialize whenever ANY
    // constant is declared.
    Owned<MTL::Function> probe = Adopt(library->newFunction(ToNSString(entry_point)));
    if (!probe) {
        return base;
    }
    NS::Dictionary* const declared = probe->functionConstantsDictionary();
    const bool has_constants = declared != nullptr && declared->count() > 0;
    {
        std::unique_lock lock{library_constants_mutex};
        if (library_has_constants.size() < MAX_LIBRARY_CACHE_ENTRIES * 2) {
            auto [it, inserted] = library_has_constants.emplace(library, has_constants);
            if (!inserted && has_constants) {
                it->second = true;
            }
        }
    }
    if (!has_constants) {
        return base;
    }

    Owned<MTL::FunctionConstantValues> values =
        Adopt(MTL::FunctionConstantValues::alloc()->init());
    const std::array<u8, 16> zero{};

    NS::Enumerator<NS::String>* keys = declared->keyEnumerator<NS::String>();
    for (NS::String* key = keys->nextObject(); key != nullptr; key = keys->nextObject()) {
        auto* constant = declared->object<MTL::FunctionConstant>(key);
        if (constant == nullptr || !constant->required()) {
            continue;
        }
        values->setConstantValue(zero.data(), constant->type(), constant->index());
    }

    MTL4::SpecializedFunctionDescriptor* specialized =
        MTL4::SpecializedFunctionDescriptor::alloc()->init();
    specialized->setFunctionDescriptor(base);
    specialized->setConstantValues(values.get());
    return specialized->autorelease();
}

Owned<MTL::ComputePipelineState> BuildComputePipeline(const Device& device, MTL::Library* library,
                                                      const std::string& entry_point,
                                                      const char* label) {
    if (library == nullptr) {
        return {};
    }
    ScopedAutoreleasePool pool;

    MTL4::ComputePipelineDescriptor* descriptor =
        MTL4::ComputePipelineDescriptor::alloc()->init();
    descriptor->setComputeFunctionDescriptor(MakeFunctionDescriptor(library, entry_point));
    if (label != nullptr) {
        descriptor->setLabel(ToNSString(label));
    }

    NS::Error* error = nullptr;
    Owned<MTL::ComputePipelineState> pipeline =
        device.NewComputePipelineState(descriptor, &error);
    descriptor->release();

    if (!pipeline) {
        LOG_ERROR(Render_Metal, "Failed to build the '{}' compute pipeline: {}",
                  label != nullptr ? label : entry_point.c_str(), DescribeError(error));
    }
    return pipeline;
}

} // namespace Metal
