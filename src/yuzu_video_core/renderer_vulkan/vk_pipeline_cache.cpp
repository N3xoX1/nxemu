// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <cstdlib>
#include <sstream>
#include <memory>
#include <thread>
#include <unordered_set>
#include <set>
#include <variant>
#ifdef _WIN32
#include <windows.h>
#endif
#include "yuzu_common/scope_exit.h"
#include <vector>

#include "yuzu_common/bit_cast.h"
#include "yuzu_common/cityhash.h"
#include "yuzu_common/fs/fs.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/thread_worker.h"
#include "yuzu_shader_recompiler/backend/spirv/emit_spirv.h"
#include "yuzu_shader_recompiler/environment.h"
#include "yuzu_shader_recompiler/frontend/maxwell/control_flow.h"
#include "yuzu_shader_recompiler/frontend/maxwell/translate_program.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"
#include "yuzu_shader_recompiler/program_header.h"
#include "yuzu_video_core/engines/kepler_compute.h"
#include "yuzu_video_core/engines/draw_manager.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/memory_manager.h"
#include "yuzu_video_core/shader_profile_limits.h"
#include "yuzu_video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "yuzu_video_core/renderer_vulkan/driver_pipeline_cache_probe.h"
#include "yuzu_video_core/renderer_vulkan/maxwell_to_vk.h"
#include "yuzu_video_core/renderer_vulkan/pipeline_helper.h"
#include "yuzu_video_core/renderer_vulkan/pipeline_statistics.h"
#include "yuzu_video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "yuzu_video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "yuzu_video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "yuzu_video_core/renderer_vulkan/progressive_pipeline_warmup.h"
#include "yuzu_video_core/renderer_vulkan/guest_pipeline_recipe_probe.h"
#include "yuzu_video_core/renderer_vulkan/guest_pipeline_recorder.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_shader_util.h"
#include "yuzu_video_core/renderer_vulkan/vk_update_descriptor.h"
#include "yuzu_video_core/shader_cache.h"
#include "yuzu_video_core/shader_environment.h"
#include "yuzu_video_core/pipeline_cache_store.h"
#include "yuzu_video_core/shader_profile.h"
#include "yuzu_video_core/shader_profile_layout.h"
#include "yuzu_video_core/shader_profile_metrics.h"
#include "yuzu_video_core/shader_profile_candidate_budget.h"
#include "yuzu_video_core/shader_profile_export.h"
#include "yuzu_video_core/shader_profile_hash.h"
#include "yuzu_video_core/shader_profile_validation.h"
#include "yuzu_video_core/shader_profile_identity.h"
#include "yuzu_video_core/shader_profile_import_status.h"
#include "yuzu_video_core/shader_profile_location.h"
#include "yuzu_video_core/shader_profile_audit.h"
#include "yuzu_video_core/renderer_vulkan/startup_pipeline_variants.h"
#include "yuzu_video_core/shader_notify.h"
#include "yuzu_video_core/surface.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"
#include "video_settings.h"

namespace Vulkan {
namespace {
using Shader::Backend::SPIRV::EmitSPIRV;
using Shader::Maxwell::ConvertLegacyToGeneric;
using Shader::Maxwell::GenerateGeometryPassthrough;
using Shader::Maxwell::GenerateVtgPassthroughVertex;
using Shader::Maxwell::MergeDualVertexPrograms;
using Shader::Maxwell::TranslateProgram;
using VideoCommon::ComputeEnvironment;
using VideoCommon::FileEnvironment;
using VideoCommon::GenericEnvironment;
using VideoCommon::GraphicsEnvironment;

constexpr u32 CACHE_VERSION = 20;
constexpr std::array<char, 8> VULKAN_CACHE_MAGIC_NUMBER{'y', 'u', 'z', 'u', 'v', 'k', 'c', 'h'};

// Startup/shutdown diagnostics only; no probe work is added to the draw path.
bool MslCacheProbeEnabled() {
    static const bool enabled = DriverCacheProbe::EnabledFromOption(std::getenv("NXEMU_MSL_CACHE_PROBE"));
    return enabled;
}

double ProbeMilliseconds(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

template <typename Container>
auto MakeSpan(Container& container) {
    return std::span(container.data(), container.size());
}

Shader::OutputTopology MaxwellToOutputTopology(Maxwell::PrimitiveTopology topology) {
    switch (topology) {
    case Maxwell::PrimitiveTopology::Points:
        return Shader::OutputTopology::PointList;
    case Maxwell::PrimitiveTopology::Lines:
    case Maxwell::PrimitiveTopology::LineStrip:
        return Shader::OutputTopology::LineStrip;
    default:
        return Shader::OutputTopology::TriangleStrip;
    }
}

Shader::CompareFunction MaxwellToCompareFunction(Maxwell::ComparisonOp comparison) {
    switch (comparison) {
    case Maxwell::ComparisonOp::Never_D3D:
    case Maxwell::ComparisonOp::Never_GL:
        return Shader::CompareFunction::Never;
    case Maxwell::ComparisonOp::Less_D3D:
    case Maxwell::ComparisonOp::Less_GL:
        return Shader::CompareFunction::Less;
    case Maxwell::ComparisonOp::Equal_D3D:
    case Maxwell::ComparisonOp::Equal_GL:
        return Shader::CompareFunction::Equal;
    case Maxwell::ComparisonOp::LessEqual_D3D:
    case Maxwell::ComparisonOp::LessEqual_GL:
        return Shader::CompareFunction::LessThanEqual;
    case Maxwell::ComparisonOp::Greater_D3D:
    case Maxwell::ComparisonOp::Greater_GL:
        return Shader::CompareFunction::Greater;
    case Maxwell::ComparisonOp::NotEqual_D3D:
    case Maxwell::ComparisonOp::NotEqual_GL:
        return Shader::CompareFunction::NotEqual;
    case Maxwell::ComparisonOp::GreaterEqual_D3D:
    case Maxwell::ComparisonOp::GreaterEqual_GL:
        return Shader::CompareFunction::GreaterThanEqual;
    case Maxwell::ComparisonOp::Always_D3D:
    case Maxwell::ComparisonOp::Always_GL:
        return Shader::CompareFunction::Always;
    }
    UNIMPLEMENTED_MSG("Unimplemented comparison op={}", comparison);
    return {};
}

Shader::AttributeType CastAttributeType(const FixedPipelineState::VertexAttribute& attr) {
    if (attr.enabled == 0) {
        return Shader::AttributeType::Disabled;
    }
    switch (attr.Type()) {
    case Maxwell::VertexAttribute::Type::UnusedEnumDoNotUseBecauseItWillGoAway:
        ASSERT_MSG(false, "Invalid vertex attribute type!");
        return Shader::AttributeType::Disabled;
    case Maxwell::VertexAttribute::Type::SNorm:
    case Maxwell::VertexAttribute::Type::UNorm:
    case Maxwell::VertexAttribute::Type::Float:
        return Shader::AttributeType::Float;
    case Maxwell::VertexAttribute::Type::SInt:
        return Shader::AttributeType::SignedInt;
    case Maxwell::VertexAttribute::Type::UInt:
        return Shader::AttributeType::UnsignedInt;
    case Maxwell::VertexAttribute::Type::UScaled:
        return Shader::AttributeType::UnsignedScaled;
    case Maxwell::VertexAttribute::Type::SScaled:
        return Shader::AttributeType::SignedScaled;
    }
    return Shader::AttributeType::Float;
}

Shader::AttributeType AttributeType(const FixedPipelineState& state, size_t index) {
    switch (state.DynamicAttributeType(index)) {
    case 0:
        return Shader::AttributeType::Disabled;
    case 1:
        return Shader::AttributeType::Float;
    case 2:
        return Shader::AttributeType::SignedInt;
    case 3:
        return Shader::AttributeType::UnsignedInt;
    }
    return Shader::AttributeType::Disabled;
}

bool IsUnorm8(VideoCore::Surface::PixelFormat format) {
    switch (format) {
    case VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM:
    case VideoCore::Surface::PixelFormat::A8B8G8R8_SRGB:
    case VideoCore::Surface::PixelFormat::B8G8R8A8_UNORM:
    case VideoCore::Surface::PixelFormat::B8G8R8A8_SRGB:
    case VideoCore::Surface::PixelFormat::R8G8_UNORM:
    case VideoCore::Surface::PixelFormat::R8_UNORM:
        return true;
    default:
        return false;
    }
}

/// Lets the host read the pixel local texture reads of a fragment shader from the color
/// attachments they sample, which float attachments are declared as input attachments for.
void FindColorAttachmentReads(Shader::IR::Program& program, const GraphicsPipelineCacheKey& key,
                              u32 screen_position_generics) {
    if (!Shader::Optimization::FindPixelLocalTextureReads(program, screen_position_generics)) {
        return;
    }
    u32 mask{};
    for (size_t index = 0; index < key.state.color_formats.size(); ++index) {
        const auto format = static_cast<Tegra::RenderTargetFormat>(key.state.color_formats[index]);
        if (format != Tegra::RenderTargetFormat::NONE &&
            !VideoCore::Surface::IsPixelFormatInteger(
                VideoCore::Surface::PixelFormatFromRenderTargetFormat(format))) {
            mask |= 1U << index;
        }
    }
    if (mask == 0) {
        for (auto& desc : program.info.texture_descriptors) {
            desc.pixel_local_read = Shader::PixelLocalRead::None;
        }
        return;
    }
    program.info.input_attachment_mask = mask;
}

Shader::RuntimeInfo MakeRuntimeInfo(std::span<const Shader::IR::Program> programs,
                                    const GraphicsPipelineCacheKey& key,
                                    const Shader::IR::Program& program,
                                    const Shader::IR::Program* previous_program) {
    Shader::RuntimeInfo info;
    if (previous_program) {
        info.previous_stage_stores = previous_program->info.stores;
        info.previous_stage_legacy_stores_mapping = previous_program->info.legacy_stores_mapping;
        if (previous_program->is_geometry_passthrough) {
            info.previous_stage_stores.mask |= previous_program->info.passthrough.mask;
        }
    } else {
        info.previous_stage_stores.mask.set();
    }
    const Shader::Stage stage{program.stage};
    const bool has_geometry{key.unique_hashes[4] != 0 && !programs[4].is_geometry_passthrough};
    const bool gl_ndc{key.state.ndc_minus_one_to_one != 0};
    const float point_size{Common::BitCast<float>(key.state.point_size)};
    switch (stage) {
    case Shader::Stage::VertexB:
        if (!has_geometry) {
            if (key.state.topology == Maxwell::PrimitiveTopology::Points) {
                info.fixed_state_point_size = point_size;
            }
            if (key.state.xfb_enabled) {
                auto [varyings, count] =
                    VideoCommon::MakeTransformFeedbackVaryings(key.state.xfb_state);
                info.xfb_varyings = varyings;
                info.xfb_count = count;
            }
            info.convert_depth_mode = gl_ndc;
        }
        if (key.state.dynamic_vertex_input) {
            for (size_t index = 0; index < Maxwell::NumVertexAttributes; ++index) {
                info.generic_input_types[index] = AttributeType(key.state, index);
            }
        } else {
            std::ranges::transform(key.state.attributes, info.generic_input_types.begin(),
                                   &CastAttributeType);
        }
        break;
    case Shader::Stage::TessellationEval:
        info.tess_clockwise = key.state.tessellation_clockwise != 0;
        info.tess_primitive = [&key] {
            const u32 raw{key.state.tessellation_primitive.Value()};
            switch (static_cast<Maxwell::Tessellation::DomainType>(raw)) {
            case Maxwell::Tessellation::DomainType::Isolines:
                return Shader::TessPrimitive::Isolines;
            case Maxwell::Tessellation::DomainType::Triangles:
                return Shader::TessPrimitive::Triangles;
            case Maxwell::Tessellation::DomainType::Quads:
                return Shader::TessPrimitive::Quads;
            }
            ASSERT(false);
            return Shader::TessPrimitive::Triangles;
        }();
        info.tess_spacing = [&] {
            const u32 raw{key.state.tessellation_spacing};
            switch (static_cast<Maxwell::Tessellation::Spacing>(raw)) {
            case Maxwell::Tessellation::Spacing::Integer:
                return Shader::TessSpacing::Equal;
            case Maxwell::Tessellation::Spacing::FractionalOdd:
                return Shader::TessSpacing::FractionalOdd;
            case Maxwell::Tessellation::Spacing::FractionalEven:
                return Shader::TessSpacing::FractionalEven;
            }
            ASSERT(false);
            return Shader::TessSpacing::Equal;
        }();
        break;
    case Shader::Stage::Geometry:
        if (program.output_topology == Shader::OutputTopology::PointList) {
            info.fixed_state_point_size = point_size;
        }
        if (key.state.xfb_enabled != 0) {
            auto [varyings, count] =
                VideoCommon::MakeTransformFeedbackVaryings(key.state.xfb_state);
            info.xfb_varyings = varyings;
            info.xfb_count = count;
        }
        info.convert_depth_mode = gl_ndc;
        break;
    case Shader::Stage::Fragment:
        info.alpha_test_func = MaxwellToCompareFunction(
            key.state.UnpackComparisonOp(key.state.alpha_test_func.Value()));
        info.alpha_test_reference = Common::BitCast<float>(key.state.alpha_test_ref);
        info.dual_source_blend = key.state.dual_source_blend != 0;
        // Vulkan requires integer fragment outputs for integer color attachments.
        for (size_t index = 0; index < info.color_output_types.size(); ++index) {
            const auto format =
                static_cast<Tegra::RenderTargetFormat>(key.state.color_formats[index]);
            if (format == Tegra::RenderTargetFormat::NONE) {
                continue;
            }
            const auto pixel_format = VideoCore::Surface::PixelFormatFromRenderTargetFormat(format);
            if (VideoCore::Surface::IsPixelFormatInteger(pixel_format)) {
                info.color_output_types[index] =
                    VideoCore::Surface::IsPixelFormatSignedInteger(pixel_format)
                        ? Shader::AttributeType::SignedInt
                        : Shader::AttributeType::UnsignedInt;
            }
            info.color_output_unorm8[index] = IsUnorm8(pixel_format);
        }
        break;
    default:
        break;
    }
    switch (key.state.topology) {
    case Maxwell::PrimitiveTopology::Points:
        info.input_topology = Shader::InputTopology::Points;
        break;
    case Maxwell::PrimitiveTopology::Lines:
    case Maxwell::PrimitiveTopology::LineLoop:
    case Maxwell::PrimitiveTopology::LineStrip:
        info.input_topology = Shader::InputTopology::Lines;
        break;
    case Maxwell::PrimitiveTopology::Triangles:
    case Maxwell::PrimitiveTopology::TriangleStrip:
    case Maxwell::PrimitiveTopology::TriangleFan:
    case Maxwell::PrimitiveTopology::Quads:
    case Maxwell::PrimitiveTopology::QuadStrip:
    case Maxwell::PrimitiveTopology::Polygon:
    case Maxwell::PrimitiveTopology::Patches:
        info.input_topology = Shader::InputTopology::Triangles;
        break;
    case Maxwell::PrimitiveTopology::LinesAdjacency:
    case Maxwell::PrimitiveTopology::LineStripAdjacency:
        info.input_topology = Shader::InputTopology::LinesAdjacency;
        break;
    case Maxwell::PrimitiveTopology::TrianglesAdjacency:
    case Maxwell::PrimitiveTopology::TriangleStripAdjacency:
        info.input_topology = Shader::InputTopology::TrianglesAdjacency;
        break;
    }
    info.force_early_z = key.state.early_z != 0;
    info.y_negate = key.state.y_negate != 0;
    return info;
}

size_t GetTotalPipelineWorkers() {
    const size_t max_core_threads =
        std::max<size_t>(static_cast<size_t>(std::thread::hardware_concurrency()), 2ULL) - 1ULL;
#ifdef ANDROID
    // Leave at least a few cores free in android
    constexpr size_t free_cores = 3ULL;
    if (max_core_threads <= free_cores) {
        return 1ULL;
    }
    return max_core_threads - free_cores;
#else
    return max_core_threads;
#endif
}

std::optional<Shader::VtgTopology> MaxwellToVtgTopology(Maxwell::PrimitiveTopology topology) {
    switch (topology) {
    case Maxwell::PrimitiveTopology::Points:
        return Shader::VtgTopology::Points;
    case Maxwell::PrimitiveTopology::Lines:
        return Shader::VtgTopology::Lines;
    case Maxwell::PrimitiveTopology::LineLoop:
        return Shader::VtgTopology::LineLoop;
    case Maxwell::PrimitiveTopology::LineStrip:
        return Shader::VtgTopology::LineStrip;
    case Maxwell::PrimitiveTopology::Triangles:
        return Shader::VtgTopology::Triangles;
    case Maxwell::PrimitiveTopology::TriangleStrip:
        return Shader::VtgTopology::TriangleStrip;
    case Maxwell::PrimitiveTopology::TriangleFan:
    case Maxwell::PrimitiveTopology::Polygon:
        return Shader::VtgTopology::TriangleFan;
    case Maxwell::PrimitiveTopology::Quads:
        return Shader::VtgTopology::Quads;
    case Maxwell::PrimitiveTopology::QuadStrip:
        return Shader::VtgTopology::QuadStrip;
    case Maxwell::PrimitiveTopology::LinesAdjacency:
        return Shader::VtgTopology::LinesAdjacency;
    case Maxwell::PrimitiveTopology::LineStripAdjacency:
        return Shader::VtgTopology::LineStripAdjacency;
    case Maxwell::PrimitiveTopology::TrianglesAdjacency:
        return Shader::VtgTopology::TrianglesAdjacency;
    case Maxwell::PrimitiveTopology::TriangleStripAdjacency:
        return Shader::VtgTopology::TriangleStripAdjacency;
    case Maxwell::PrimitiveTopology::Patches:
        break;
    }
    return std::nullopt;
}

/// Where transform feedback captures each output attribute, from the guest layouts
Shader::VtgTransformFeedback MakeVtgTransformFeedback(const VideoCommon::TransformFeedbackState& state,
                                                      Shader::VtgTopology topology) {
    Shader::VtgTransformFeedback xfb;
    xfb.topology = topology;
    for (size_t buffer = 0; buffer < state.layouts.size(); ++buffer) {
        const auto& layout{state.layouts[buffer]};
        // Without geometry shaders, every vertex goes to the first stream
        if (layout.stream != 0 || layout.stride == 0) {
            continue;
        }
        xfb.strides[buffer] = layout.stride;
        const u32 count{std::min<u32>({layout.varying_count, layout.stride / 4,
                                        static_cast<u32>(state.varyings[buffer].size() * 4)})};
        for (u32 word = 0; word < count; ++word) {
            const auto& locations{state.varyings[buffer][word / 4]};
            const std::array attributes{locations.attribute0.Value(), locations.attribute1.Value(),
                                        locations.attribute2.Value(), locations.attribute3.Value()};
            xfb.targets[attributes[word % 4]] = {
                .buffer = static_cast<s8>(buffer),
                .offset = static_cast<u16>(word * 4),
            };
        }
    }
    return xfb;
}

/// Describes how a vertex shader running as compute reads its attributes from the vertex buffers
Shader::VtgVertexInputs MakeVtgVertexInputs(const FixedPipelineState& state) {
    using Size = Maxwell::VertexAttribute::Size;
    using Type = Maxwell::VertexAttribute::Type;
    Shader::VtgVertexInputs inputs{};
    for (size_t index = 0; index < inputs.size(); ++index) {
        const FixedPipelineState::VertexAttribute& attribute{state.attributes[index]};
        Shader::VtgVertexAttribute& input{inputs[index]};
        if (attribute.enabled == 0) {
            continue;
        }
        input.buffer = static_cast<u8>(attribute.buffer.Value());
        const auto set_size{[&input](u8 components, u8 bits) {
            input.components = components;
            input.bits = bits;
        }};
        switch (attribute.Size()) {
        case Size::Size_R32_G32_B32_A32:
            set_size(4, 32);
            break;
        case Size::Size_R32_G32_B32:
            set_size(3, 32);
            break;
        case Size::Size_R32_G32:
            set_size(2, 32);
            break;
        case Size::Size_R32:
            set_size(1, 32);
            break;
        case Size::Size_R16_G16_B16_A16:
            set_size(4, 16);
            break;
        case Size::Size_R16_G16_B16:
            set_size(3, 16);
            break;
        case Size::Size_R16_G16:
            set_size(2, 16);
            break;
        case Size::Size_R16:
            set_size(1, 16);
            break;
        case Size::Size_R8_G8_B8_A8:
        case Size::Size_X8_B8_G8_R8:
            set_size(4, 8);
            break;
        case Size::Size_R8_G8_B8:
            set_size(3, 8);
            break;
        case Size::Size_R8_G8:
        case Size::Size_G8_R8:
            set_size(2, 8);
            break;
        case Size::Size_R8:
        case Size::Size_A8:
            set_size(1, 8);
            break;
        case Size::Size_A2_B10_G10_R10:
            set_size(4, 0);
            input.packed_offsets = {0, 10, 20, 30};
            input.packed_bits = {10, 10, 10, 2};
            break;
        case Size::Size_B10_G11_R11:
            set_size(3, 0);
            input.packed_offsets = {0, 11, 22, 0};
            input.packed_bits = {11, 11, 10, 0};
            break;
        default:
            continue;
        }
        switch (attribute.Type()) {
        case Type::SNorm:
            input.type = Shader::VtgComponentType::SNorm;
            break;
        case Type::UNorm:
            input.type = Shader::VtgComponentType::UNorm;
            break;
        case Type::SInt:
            input.type = Shader::VtgComponentType::SInt;
            break;
        case Type::UInt:
            input.type = Shader::VtgComponentType::UInt;
            break;
        case Type::UScaled:
            input.type = Shader::VtgComponentType::UScaled;
            break;
        case Type::SScaled:
            input.type = Shader::VtgComponentType::SScaled;
            break;
        case Type::Float:
            input.type = Shader::VtgComponentType::Float;
            break;
        default:
            break;
        }
    }
    return inputs;
}
} // Anonymous namespace

size_t ComputePipelineCacheKey::Hash() const noexcept {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(this), sizeof *this);
    return static_cast<size_t>(hash);
}

bool ComputePipelineCacheKey::operator==(const ComputePipelineCacheKey& rhs) const noexcept {
    return std::memcmp(&rhs, this, sizeof *this) == 0;
}

// P2: Only the admission state outlives LoadDiskResources. No disk-loader stack
// references, Vulkan cache maps or imported cache budgets are shared with workers.
// Native creations execute through the existing prioritized runtime queue.
struct PipelineCache::ProgressiveWarmup {
    struct Graphics {
        GraphicsPipelineCacheKey key;
        std::vector<FileEnvironment> environments;
    };
    struct Compute {
        ComputePipelineCacheKey key;
        FileEnvironment environment;
    };
    struct Job {
        std::variant<Graphics, Compute> input;
        std::shared_ptr<GraphicsPipeline> graphics;
        std::unique_ptr<ComputePipeline> compute;
        explicit Job(Graphics value) : input(std::move(value)) {}
        explicit Job(Compute value) : input(std::move(value)) {}
        Job(Job&&) = default;
        Job& operator=(Job&&) = default;
        Job(const Job&) = delete;
        Job& operator=(const Job&) = delete;
    };

    explicit ProgressiveWarmup(PipelineCache& cache_, std::vector<Job> entries,
                               std::vector<GraphicsPipelineCacheKey> hle_shared)
        : cache(cache_), jobs(std::move(entries)) {
        for (auto alias : hle_shared) {
            auto plain = alias;
            plain.state.app_stage.Assign(Tegra::Engines::Maxwell3D::EngineHint::None);
            aliases[plain].push_back(alias);
        }
        for (size_t index = 0; index < jobs.size(); ++index) {
            if (auto* g = std::get_if<Graphics>(&jobs[index].input))
                graphics_index[g->key.Hash()].push_back(index);
            else
                compute_index[std::get<Compute>(jobs[index].input).key.Hash()].push_back(index);
        }
        scheduler = std::make_unique<ProgressivePipelineWarmup>(
            cache.runtime_builds, jobs.size(), [this](size_t index) { return Build(index); });
    }

    void Stop() { scheduler->Stop(); }
    void RecordFrame() { scheduler->RecordFrame(); }

    void PublishReady() {
        scheduler->PublishReady([&](size_t index) {
            auto& job = jobs[index];
            if (auto* g = std::get_if<Graphics>(&job.input)) {
                if (job.graphics) {
                    auto it = cache.graphics_cache.try_emplace(g->key, std::move(job.graphics)).first;
                    if (it->second) {
                        if (auto found = aliases.find(g->key); found != aliases.end()) {
                            for (const auto& alias : found->second) {
                                it->second->SetSharedKey(alias);
                                cache.graphics_cache.try_emplace(alias, it->second);
                            }
                        }
                    }
                }
            } else {
                const auto& c = std::get<Compute>(job.input);
                if (job.compute) cache.compute_cache.try_emplace(c.key, std::move(job.compute));
            }
        });
    }

    // Return true if the async draw can be skipped while the existing native
    // build finishes. Urgent/compute draws always claim and wait for it.
    bool DemandGraphics(const GraphicsPipelineCacheKey& key, bool allow_skip = false) {
        if (auto it = graphics_index.find(key.Hash()); it != graphics_index.end())
            for (auto i : it->second)
                if (std::get<Graphics>(jobs[i].input).key == key) return DemandIndex(i, allow_skip);
        // Replacement-free HLE twins may share a plain native pipeline.
        auto plain = key;
        plain.state.app_stage.Assign(Tegra::Engines::Maxwell3D::EngineHint::None);
        if (auto it = aliases.find(plain); it != aliases.end()) {
            for (const auto& alias : it->second) if (alias == key) {
                if (auto hit = graphics_index.find(plain.Hash()); hit != graphics_index.end())
                    for (auto i : hit->second)
                        if (std::get<Graphics>(jobs[i].input).key == plain) return DemandIndex(i, allow_skip);
            }
        }
        return false;
    }
    void DemandCompute(const ComputePipelineCacheKey& key) {
        if (auto it = compute_index.find(key.Hash()); it != compute_index.end())
            for (auto i : it->second)
                if (std::get<Compute>(jobs[i].input).key == key) { DemandIndex(i, false); return; }
    }

private:
    void ReleaseInputs(size_t index) {
        if (auto* g = std::get_if<Graphics>(&jobs[index].input))
            std::vector<FileEnvironment>{}.swap(g->environments);
        else
            std::get<Compute>(jobs[index].input).environment = FileEnvironment{};
    }
    bool DemandIndex(size_t index, bool allow_skip) {
        const auto result = scheduler->Demand(index, allow_skip);
        if (result == ProgressivePipelineWarmup::DemandResult::Pending) return true;
        if (result == ProgressivePipelineWarmup::DemandResult::Runtime) ReleaseInputs(index);
        PublishReady();
        return false;
    }

    bool Build(size_t index) {
        auto& job = jobs[index];
        try {
            ShaderPools pools;
            if (auto* g = std::get_if<Graphics>(&job.input)) {
                boost::container::static_vector<Shader::Environment*, Maxwell::MaxShaderProgram> ptrs;
                for (auto& env : g->environments) ptrs.push_back(&env);
                auto result = cache.CreateGraphicsPipeline(pools, g->key, MakeSpan(ptrs), nullptr, false);
                if (result && result->IsBuilt()) job.graphics = std::move(result);
            } else {
                auto& c = std::get<Compute>(job.input);
                auto result = cache.CreateComputePipeline(pools, c.key, c.environment, nullptr, false);
                if (result && !result->HasBuildFailed()) job.compute = std::move(result);
            }
        } catch (const std::exception& error) {
            LOG_WARNING(Render_Vulkan, "Progressive NXShader prewarm failed: {}", error.what());
        } catch (...) {
            LOG_WARNING(Render_Vulkan, "Progressive NXShader prewarm failed (unknown error)");
        }
        ReleaseInputs(index);
        return bool(job.graphics || job.compute);
    }

    PipelineCache& cache;
    std::vector<Job> jobs;
    std::unordered_map<GraphicsPipelineCacheKey, std::vector<GraphicsPipelineCacheKey>> aliases;
    std::unordered_map<size_t, std::vector<size_t>> graphics_index, compute_index;
    // Destroyed first: stop admission and drain callbacks before job inputs/results.
    std::unique_ptr<ProgressivePipelineWarmup> scheduler;
};

size_t GraphicsPipelineCacheKey::Hash() const noexcept {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(this), Size());
    return static_cast<size_t>(hash);
}

bool GraphicsPipelineCacheKey::operator==(const GraphicsPipelineCacheKey& rhs) const noexcept {
    return std::memcmp(&rhs, this, Size()) == 0;
}

PipelineCache::PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_,
                             const Device& device_, Scheduler& scheduler_,
                             DescriptorPool& descriptor_pool_,
                             GuestDescriptorQueue& guest_descriptor_queue_,
                             RenderPassCache& render_pass_cache_, BufferCache& buffer_cache_,
                             TextureCache& texture_cache_, VideoCore::ShaderNotify& shader_notify_,
                             StagingBufferPool& staging_pool_)
    : VideoCommon::ShaderCache{device_memory_}, device{device_}, scheduler{scheduler_},
      staging_pool{staging_pool_}, vtg_scratch{device_, scheduler_, staging_pool_},
      descriptor_pool{descriptor_pool_}, guest_descriptor_queue{guest_descriptor_queue_},
      render_pass_cache{render_pass_cache_}, buffer_cache{buffer_cache_},
      texture_cache{texture_cache_}, shader_notify{shader_notify_},
      use_asynchronous_shaders{videoSettings.use_asynchronous_shaders},
      use_vulkan_pipeline_cache{videoSettings.use_vulkan_driver_pipeline_cache},
      workers(device.HasBrokenParallelShaderCompiling() ? 1ULL : GetTotalPipelineWorkers(),
              "VkPipelineBuilder"),
      serialization_thread(1, "VkPipelineSerialization"),
      runtime_builds(device.HasBrokenParallelShaderCompiling() ? 1ULL : GetTotalPipelineWorkers()) {
    const auto& float_control{device.FloatControlProperties()};
    const VkDriverId driver_id{device.GetDriverID()};
    // MoltenVK walks every resource of a descriptor set each time it is bound
    large_texture_arrays_in_table = driver_id == VK_DRIVER_ID_MOLTENVK;
    profile = Shader::Profile{
        .supported_spirv = device.SupportedSpirvVersion(),
        .unified_descriptor_binding = true,
        .support_descriptor_aliasing = device.IsDescriptorAliasingSupported(),
        .support_sampled_image_array_nonuniform_indexing =
            device.IsSampledImageArrayNonUniformIndexingSupported(),
        .support_uniform_texel_buffer_array_nonuniform_indexing =
            device.IsUniformTexelBufferArrayNonUniformIndexingSupported(),
        .support_storage_image_array_nonuniform_indexing =
            device.IsStorageImageArrayNonUniformIndexingSupported(),
        .disable_uniform_buffer_descriptor_aliasing =
            device.IsDescriptorAliasingSupported() && device.GetDriverID() == VK_DRIVER_ID_MOLTENVK,
        .support_int8 = device.IsInt8Supported(),
        .support_int16 = device.IsShaderInt16Supported(),
        .support_int64 = device.IsShaderInt64Supported(),
        .support_vertex_instance_id = false,
        .support_float_controls = device.IsKhrShaderFloatControlsSupported(),
        .support_separate_denorm_behavior =
            float_control.denormBehaviorIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL,
        .support_separate_rounding_mode =
            float_control.roundingModeIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL,
        .support_fp16_denorm_preserve = float_control.shaderDenormPreserveFloat16 != VK_FALSE,
        .support_fp32_denorm_preserve = float_control.shaderDenormPreserveFloat32 != VK_FALSE,
        .support_fp16_denorm_flush = float_control.shaderDenormFlushToZeroFloat16 != VK_FALSE,
        .support_fp32_denorm_flush = float_control.shaderDenormFlushToZeroFloat32 != VK_FALSE,
        .support_fp16_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat16 != VK_FALSE,
        .support_fp32_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat32 != VK_FALSE,
        .support_fp64_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat64 != VK_FALSE,
        .support_explicit_workgroup_layout = device.IsKhrWorkgroupMemoryExplicitLayoutSupported(),
        .support_vote = device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_VOTE_BIT),
        .support_subgroup_in_vertex_stages =
            device.IsSubgroupStageSupported(VK_SHADER_STAGE_VERTEX_BIT),
        .support_viewport_index_layer_non_geometry =
            device.IsExtShaderViewportIndexLayerSupported(),
        .support_viewport_mask = device.IsNvViewportArray2Supported(),
        .support_typeless_image_loads = device.IsFormatlessImageLoadSupported(),
        .support_demote_to_helper_invocation =
            device.IsExtShaderDemoteToHelperInvocationSupported(),
        .support_int64_atomics = device.IsExtShaderAtomicInt64Supported(),
        .support_derivative_control = true,
        .support_geometry_shader_passthrough = device.IsNvGeometryShaderPassthroughSupported(),
        .support_native_ndc = device.IsExtDepthClipControlSupported(),
        .support_scaled_attributes = !device.MustEmulateScaledFormats(),
        .support_multi_viewport = device.SupportsMultiViewport(),
        .support_geometry_streams = device.AreTransformFeedbackGeometryStreamsSupported(),

        .warp_size_potentially_larger_than_guest = device.IsWarpSizePotentiallyBiggerThanGuest(),

        .lower_left_origin_mode = false,
        .need_declared_frag_colors = false,
        .has_unorm8_round_half_up = driver_id == VK_DRIVER_ID_MOLTENVK,
        .need_gather_subpixel_offset = driver_id == VK_DRIVER_ID_AMD_PROPRIETARY ||
                                       driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
                                       driver_id == VK_DRIVER_ID_MESA_RADV ||
                                       driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS ||
                                       driver_id == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA,

        .has_broken_spirv_clamp = driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS,
        .has_broken_spirv_position_input = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
        .has_broken_unsigned_image_offsets = false,
        .has_broken_signed_operations = false,
        .has_broken_fp16_float_controls = driver_id == VK_DRIVER_ID_NVIDIA_PROPRIETARY,
        .has_slow_fragment_no_contraction = driver_id == VK_DRIVER_ID_MOLTENVK,
        .ignore_nan_fp_comparisons = false,
        .has_broken_spirv_subgroup_mask_vector_extract_dynamic =
            driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
        .has_broken_robust =
            device.IsNvidia() && device.GetNvidiaArch() <= NvidiaArchitecture::Arch_Pascal,
        .min_ssbo_alignment = device.GetStorageBufferAlignment(),
        .max_user_clip_distances = device.GetMaxUserClipDistances(),
        // Metal refuses shaders over its limit, the other drivers build them
        .max_compute_shared_memory_size = device.GetDriverID() == VK_DRIVER_ID_MOLTENVK
                                              ? device.GetMaxComputeSharedMemorySize()
                                              : 0,
    };

    host_info = Shader::HostTranslateInfo{
        .support_float64 = device.IsFloat64Supported(),
        .support_float16 = device.IsFloat16Supported(),
        .support_int64 = device.IsShaderInt64Supported(),
        .needs_demote_reorder = driver_id == VK_DRIVER_ID_AMD_PROPRIETARY ||
                                driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
                                driver_id == VK_DRIVER_ID_SAMSUNG_PROPRIETARY,
        .support_snorm_render_buffer = true,
        .support_viewport_index_layer = device.IsExtShaderViewportIndexLayerSupported(),
        .min_ssbo_alignment = static_cast<u32>(device.GetStorageBufferAlignment()),
        .support_geometry_shader_passthrough = device.IsNvGeometryShaderPassthroughSupported(),
        .support_conditional_barrier = device.SupportsConditionalBarriers(),
        .max_bindless_descriptors_per_stage = device.MaxBindlessDescriptorsPerStage(),
        .max_storage_images_per_stage =
            std::min(device.DescriptorSetLimits().maxPerStageDescriptorStorageImages,
                     device.DescriptorSetLimits().maxDescriptorSetStorageImages),
        .support_null_descriptor = device.HasNullDescriptor(),
        .support_separate_sampler = true,
    };

    if (device.GetMaxVertexInputAttributes() < Maxwell::NumVertexAttributes ||
        device.GetMaxVertexInputBindings() < Maxwell::NumVertexArrays) {
        LOG_INFO(Render_Vulkan, "Compacting guest vertex slots into {} host attributes / {} buffers",
                 device.GetMaxVertexInputAttributes(), device.GetMaxVertexInputBindings());
    }

    dynamic_features = DynamicFeatures{
        .has_extended_dynamic_state = device.IsExtExtendedDynamicStateSupported(),
        .has_extended_dynamic_state_2 = device.IsExtExtendedDynamicState2Supported(),
        .has_extended_dynamic_state_2_extra = device.IsExtExtendedDynamicState2ExtrasSupported(),
        .has_extended_dynamic_state_3_blend = device.IsExtExtendedDynamicState3BlendingSupported(),
        .has_extended_dynamic_state_3_enables = device.IsExtExtendedDynamicState3EnablesSupported(),
        .has_dynamic_vertex_input = device.IsExtVertexInputDynamicStateSupported(),
        .has_vertex_stride_workaround = device.NeedsVertexAttributeStrideWorkaround(),
    };
    guest_recipe_probe = GuestPipelineRecipeProbe::FromEnvironment();
}

void PipelineCache::WaitForBuilds() {
    // Stop admission first, then drain callbacks while their session still lives.
    if (progressive_warmup) progressive_warmup->Stop();
    runtime_builds.WaitForRequests();
    if (progressive_warmup) {
        progressive_warmup->PublishReady();
        progressive_warmup.reset();
    }
    // Builds can still refer to shader inputs and the Vulkan cache. Drain them before
    // taking the final cache snapshot, then finish portable-cache serialization.
    FinalizeReadyGraphicsPipelines();
    workers.WaitForRequests();
    if (guest_recipe_recorder) guest_recipe_recorder->Flush();
    serialization_thread.WaitForRequests();
}

PipelineCache::~PipelineCache() {
    LOG_INFO(Render_Vulkan,"Cross-version recipes: validated={} rejected={}",cross_version_accepted,cross_version_rejected);
    WaitForBuilds();
    if (guest_recipe_probe) guest_recipe_probe->Flush();
    if(runtime_report && !runtime_report->Flush(quarantined_graphics,quarantined_compute))
        LOG_WARNING(Render_Vulkan,"Cannot write NXShader runtime diagnostic report");
    if (use_vulkan_pipeline_cache && !vulkan_pipeline_cache_filename.empty()) {
        SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                     CACHE_VERSION);
    }
}

void PipelineCache::RecordDiagnosticFrame() {
    if (progressive_warmup) progressive_warmup->RecordFrame();
    if (guest_recipe_recorder) guest_recipe_recorder->Flush();
    if(runtime_report && runtime_report->active)runtime_report->Frame();
}

GraphicsPipeline* PipelineCache::CurrentGraphicsPipeline() {
    FinalizeReadyGraphicsPipelines();
    if (progressive_warmup) progressive_warmup->PublishReady();
    skipped_unfinished = false;

    if (!RefreshStages(graphics_key.unique_hashes)) {
        current_pipeline = nullptr;
        return nullptr;
    }
    graphics_key.state.Refresh(*maxwell3d, dynamic_features);
    graphics_cbuf_size_state.Refresh(graphics_key.cbuf_sizes, graphics_key.unique_hashes,
        cbuf_size_dependencies, [&](size_t stage, size_t bank) {
            const auto& buffer = maxwell3d->state.shader_stages[stage].const_buffers[bank];
            return buffer.enabled ? buffer.size : 0;
        });
    graphics_key.texture_pool = {};
    if ((graphics_cbuf_size_state.ExtraMask() & VideoCommon::TEXTURE_POOL_SIZE_BIT) != 0) {
        const auto pool = VideoCommon::GraphicsTexturePoolInfo(maxwell3d->regs);
        graphics_key.texture_pool = {pool.num_textures,
                                     pool.linked_samplers ? 0U : pool.num_samplers};
    }

    if (guest_recipe_probe) {
        guest_recipe_probe->Observe(maxwell3d->regs, maxwell3d->draw_manager->GetDrawState().topology,
                                    maxwell3d->engine_state, graphics_key, dynamic_features);
    }

    u64* recipe_prefix{};
    GraphicsPipeline* const next{current_pipeline ? current_pipeline->Next(graphics_key, recipe_prefix)
                                                  : nullptr};
    if (guest_recipe_recorder) {
        guest_recipe_recorder->Observe(*maxwell3d, graphics_key, dynamic_features, recipe_prefix);
    }
    if (next) {
        current_pipeline = next;
        return BuiltPipeline(current_pipeline);
    }
    return CurrentGraphicsPipelineSlowPath();
}

ComputePipeline* PipelineCache::CurrentComputePipeline() {
    if (progressive_warmup) progressive_warmup->PublishReady();
    const ShaderInfo* const shader{ComputeShader()};
    if (!shader) {
        return nullptr;
    }
    const auto& qmd{kepler_compute->launch_description};
    ComputePipelineCacheKey key{
        .unique_hash = shader->unique_hash,
        .shared_memory_size = qmd.shared_alloc,
        .workgroup_size{qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z},
        .texture_table_size = std::min(kepler_compute->regs.tic.limit + 1U, 1U << 20),
    };
    const u32 size_mask = qmd.const_buffer_enable_mask.Value() &
                          cbuf_size_dependencies.Mask(shader->unique_hash);
    for (size_t bank = 0; bank < key.cbuf_sizes.size(); ++bank) {
        key.cbuf_sizes[bank] = ((size_mask >> bank) & 1) != 0
                                  ? qmd.const_buffer_config[bank].size.Value() : 0;
    }
    if(runtime_report)quarantined_compute.Requested(key);
    // Each pipeline serves one key; its first dispatch of the session is recorded.
    const auto recorded = [&](ComputePipeline* pipeline) {
        if (pipeline && guest_recipe_recorder && !pipeline->recipe_recorded) {
            pipeline->recipe_recorded = true;
            static_assert(sizeof(key) == VideoCommon::ShaderProfile::Journal::ComputeWords * sizeof(u32));
            std::array<u32, VideoCommon::ShaderProfile::Journal::ComputeWords> words;
            std::memcpy(words.data(), &key, sizeof(key));
            guest_recipe_recorder->ObserveCompute(words);
        }
        return pipeline;
    };
    if(const auto found=compute_cache.find(key);found!=compute_cache.end())return recorded(found->second.get());
    if (progressive_warmup) {
        progressive_warmup->DemandCompute(key);
        if (const auto found = compute_cache.find(key); found != compute_cache.end())
            return recorded(found->second.get());
    }
    if(auto* reused=TryReuseCompute(key,shader))return recorded(reused);
    const auto [pair, is_new]{compute_cache.try_emplace(key)};
    auto& pipeline{pair->second};
    if (!is_new) {
        return recorded(pipeline.get());
    }
    pipeline = CreateComputePipeline(key, shader);
    return recorded(pipeline.get());
}

void PipelineCache::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                      const VideoCore::DiskResourceLoadCallback& callback, std::string_view content_identity, [[maybe_unused]] std::string_view game_path) {
    namespace Timing = VideoCommon::ShaderProfile::Timing;
    // The default retains the existing complete, blocking prewarm on every OS.
    const char* progressive_opt = std::getenv("NXEMU_NXSHADER_PROGRESSIVE");
    const bool progressive_mode = progressive_opt ? ProgressiveWarmupEnabled(progressive_opt) : videoSettings.nxshader_warmup_mode == 1;
    InitialWarmupBudget initial_budget(progressive_mode,
        InitialWarmupBudget::ParseOption(std::getenv("NXEMU_NXSHADER_INITIAL_MIN"),
            std::clamp(videoSettings.nxshader_initial_minimum,0,8192),8192),
        InitialWarmupBudget::ParseOption(std::getenv("NXEMU_NXSHADER_INITIAL_MS"),
            std::clamp(videoSettings.nxshader_initial_seconds,0,600)*1000,600000));
    const size_t initial_wave = device.HasBrokenParallelShaderCompiling() ? 1 : GetTotalPipelineWorkers();
    std::vector<ProgressiveWarmup::Job> progressive_jobs;
    if (title_id == 0) {
        return;
    }
    const auto shader_dir{Common::FS::GetYuzuPath(Common::FS::YuzuPath::ShaderDir)};
    const auto base_dir{shader_dir / fmt::format("{:016x}", title_id)};
    if (!Common::FS::CreateDir(shader_dir) || !Common::FS::CreateDir(base_dir)) {
        LOG_ERROR(Common_Filesystem, "Failed to create pipeline cache directories");
        return;
    }
    if(const char* output=std::getenv("NXEMU_NXSHADER_RUNTIME_REPORT");output && *output)
        runtime_report=std::make_unique<VideoCommon::ShaderProfile::Runtime::Report>(std::filesystem::u8path(output));
    pipeline_cache_filename = base_dir / "vulkan.nxc";
    capture_context = VideoCommon::ShaderProfile::ValidContext(content_identity) ? std::string(content_identity) : std::string{};
    // Unidentified content (mods, unrecognized dumps) still reloads its own local
    // pipelines. That context never enables recording, export or update candidates.
    cache_context = capture_context.empty() ? VideoCommon::ShaderProfile::UnidentifiedContext(title_id) : capture_context;
    try {
        // Explicit diagnostic A/B switch; normal runs record automatically.
        const char* disable_recording = std::getenv("NXEMU_RECIPE_RECORD_DISABLE");
        if (!capture_context.empty() && videoSettings.record_shader_recipes &&
            (!disable_recording || std::string_view(disable_recording) != "1"))
            guest_recipe_recorder = std::make_unique<GuestPipelineRecorder>(VideoCommon::ShaderProfile::RecordingPath(base_dir,capture_context), serialization_thread);
    } catch (const std::exception& e) {
        LOG_ERROR(Render_Vulkan, "Cannot initialize automatic guest recipes: {}", e.what());
    }
    // One-time move of the legacy cache into the local store; without it every
    // existing installation would start from a cold cache.
    if (const auto legacy = base_dir / "vulkan.bin"; std::filesystem::exists(legacy)) {
        try {
            size_t migrated{};
            if (VideoCommon::MigrateLegacyPipelines(stop_loading, legacy, pipeline_cache_filename,
                                                    CACHE_VERSION, sizeof(ComputePipelineCacheKey),
                                                    sizeof(GraphicsPipelineCacheKey), cache_context,
                                                    migrated)) {
                std::filesystem::remove(legacy);
                LOG_INFO(Render_Vulkan, "Moved {} pipelines from the legacy shader cache", migrated);
            }
        } catch (const std::exception& error) {
            LOG_WARNING(Render_Vulkan, "Legacy shader cache kept for a later retry: {}", error.what());
        }
    }

    if (use_vulkan_pipeline_cache) {
        vulkan_pipeline_cache_filename = base_dir / "vulkan_pipelines.bin";
        vulkan_pipeline_cache =
            LoadVulkanPipelineCache(vulkan_pipeline_cache_filename, CACHE_VERSION);
    }
    if (MslCacheProbeEnabled()) {
        const char* timing_option = std::getenv("NXEMU_NXSHADER_TIMING");
        LOG_INFO(Render_Vulkan,
                 "MSL-cache probe: driver_cache_enabled={} driver_id={} "
                 "timing_enabled={} (timings require NXEMU_NXSHADER_TIMING=1)",
                 use_vulkan_pipeline_cache, static_cast<u32>(device.GetDriverID()),
                 timing_option && std::string_view(timing_option) != "0" && *timing_option);
    }
    // MoltenVK imports disk MSL libraries serially. An empty shared driver
    // cache also holds its shader-cache mutex while compiling each library,
    // serialising our pipeline workers. Warm up from the portable shader cache
    // without a VkPipelineCache; retain any existing driver blob on disk.

    struct {
        std::mutex mutex;
        size_t total{};
        size_t built{};
        size_t progress_total{};
        VideoCore::LoadCallbackStage build_stage{VideoCore::LoadCallbackStage::Build};
        std::chrono::steady_clock::time_point last_build_progress{};
        bool has_loaded{};
        size_t failures{};
        size_t deferred{};
        std::unique_ptr<VideoCommon::ShaderProfile::NativeWarmupFailures> warmup_failures;
        std::unique_ptr<PipelineStatistics> statistics;
        std::unique_ptr<Timing::Metrics> timings;
    } state;
    const char* timing_option = std::getenv("NXEMU_NXSHADER_TIMING");
    if (timing_option && *timing_option && std::string_view(timing_option) != "0")
        state.timings = std::make_unique<Timing::Metrics>();
    Timing::Timer startup_timer(state.timings.get(), Timing::Phase::StartupWall);
    if (device.IsKhrPipelineExecutablePropertiesEnabled()) {
        state.statistics = std::make_unique<PipelineStatistics>(device);
    }

    // Earlier records included sizes for every bound bank, even when the shader never
    // queried them. Normalize and deduplicate them without discarding the existing shader cache.
    struct GraphicsInput { GraphicsPipelineCacheKey key; std::vector<FileEnvironment> envs; bool candidate{}; std::string record_id; };
    struct ComputeInput { ComputePipelineCacheKey key; FileEnvironment env; bool candidate{}; std::string record_id; };
    std::vector<GraphicsInput> graphics_inputs;
    std::vector<ComputeInput> compute_inputs;
    size_t buffered_input_bytes{};
    std::unordered_set<const void*> accounted_code;
    // Never poison the regular cache when optional foreign-version candidates exceed
    // the startup input budget. Reserve all stages atomically before retaining them.
    size_t skipped_candidate_inputs{};
    const auto account_inputs = [&](std::span<const FileEnvironment> envs,bool candidate) {
        std::array<VideoCommon::ShaderProfile::StartupInput,6> inputs{};
        if(envs.size()>inputs.size())throw std::runtime_error("Too many shader stages");
        for(size_t i=0;i<envs.size();++i)
            inputs[i]={envs[i].CodeIdentity(),envs[i].CompilationInputBytes(),envs[i].SharedCodeBytes()};
        if(VideoCommon::ShaderProfile::AccountStartupInputs(
               std::span(inputs).first(envs.size()),accounted_code,buffered_input_bytes))return true;
        if(candidate){++skipped_candidate_inputs;return false;}
        throw std::runtime_error("Startup shader input memory budget exceeded");
    };
    const char* cross_audit_path = std::getenv("NXEMU_NXSHADER_AUDIT_CROSS_VERSION");
    const bool comparing_foreign = cross_audit_path && *cross_audit_path;
    const char* candidate_option=std::getenv("NXEMU_NXSHADER_CANDIDATE_LIMIT");
    const auto candidate_limit=candidate_option ?
        VideoCommon::ShaderProfile::CandidateWarmupBudget::ParseLimit(candidate_option) :
        VideoCommon::ShaderProfile::CandidateWarmupBudget::DefaultCandidates;
    VideoCommon::ShaderProfile::CandidateWarmupBudget foreign_budget(candidate_limit);
    const auto report_build=[&] {
        if(!state.has_loaded)return;
        const auto now=std::chrono::steady_clock::now();
        const size_t progress_total = state.progress_total ? state.progress_total : state.total;
        if(state.built!=progress_total && now-state.last_build_progress<std::chrono::milliseconds(50))return;
        state.last_build_progress=now;
        callback(state.build_stage,state.built,progress_total);
    };
    // Drain workers before their captured progress helper and input budget leave scope.
    SCOPE_EXIT { workers.WaitForRequests(); };
    const auto load_compute{[&](std::istream& file, FileEnvironment env, bool candidate=false, std::string record_id={}) {
        ComputePipelineCacheKey key{};
        file.read(reinterpret_cast<char*>(&key), sizeof(key));
        if(!candidate)cbuf_size_dependencies.Observe(key.unique_hash, env.CbufSizeMask());
        if (!account_inputs(std::span<const FileEnvironment>{&env, 1}, candidate)) return true;
        compute_inputs.push_back({key, std::move(env), candidate, std::move(record_id)});
        return true;
    }};
    const auto warmup_id=[](char kind, const auto& key, std::string_view inputs) {
        std::string bytes(1,kind);
        bytes.append(reinterpret_cast<const char*>(&key),sizeof(key));
        bytes.append(inputs);
        return VideoCommon::ShaderProfile::ResourceSha256(bytes);
    };
    const auto defer_warmup=[&](const std::string& id) {
        std::scoped_lock lock{state.mutex};
        if (state.warmup_failures && state.warmup_failures->Defer(id)) {
            ++state.deferred;
            return true;
        }
        return false;
    };
    const auto queue_compute = [&](const ComputePipelineCacheKey& key, FileEnvironment env, bool candidate=false, std::string record_id={}) {
        const auto id=warmup_id('C',key,env.CompilationIdentity());
        if (defer_warmup(id)) return;
        { std::scoped_lock lock{state.mutex}; ++state.total; }
        workers.QueueWork([this, key, id, candidate, stop_loading, record_id=std::move(record_id), env_ = std::move(env), &state, &report_build, &buffered_input_bytes, &foreign_budget]() mutable {
            if(stop_loading.stop_requested()) {
                std::scoped_lock lock{state.mutex};
                ++state.deferred;++state.built;report_build();return;
            }
            size_t reservation{};
            if(candidate) {
                std::scoped_lock lock{state.mutex};
                reservation=foreign_budget.Reserve(1,false,buffered_input_bytes);
                if(!reservation){++state.deferred;++state.built;report_build();return;}
            }
            // Reserve only when the worker starts, still before Vulkan. Queued
            // jobs hold no proof maps yet and need no worst-case reservation.
            SCOPE_EXIT { if(reservation){std::scoped_lock lock{state.mutex};foreign_budget.Release(reservation);} };
            Timing::Context timing_context(state.timings.get());
            ShaderPools pools;
            if(candidate)env_.BeginReuseProof(false);
            auto pipeline{CreateComputePipeline(pools, key, env_, state.statistics.get(), false)};
            // This build is synchronous. A non-null wrapper can still contain
            // a rejected native pipeline; retain it as a retryable failure.
            const bool success = pipeline && !pipeline->HasBuildFailed();
            std::scoped_lock lock{state.mutex};
            if (state.warmup_failures) state.warmup_failures->Complete(id,success);
            if (success) {
                if(candidate) {
                    const auto proof_bytes=env_.ReuseProofBytes();
                    // Admission reserved the worst possible proof size before Vulkan work.
                    if(!foreign_budget.Commit(reservation,proof_bytes,buffered_input_bytes)) {
                        ++state.deferred;++state.built;report_build();return;
                    }
                    reservation=0;
                    env_.SealReuseProof(program_equivalence.CanRebase(env_.ProgramStorage(),env_.ShaderStage(),env_.StartAddress(),env_.StartAddress()));
                    auto value=std::make_shared<ForeignCompute>();value->key=key;
                    value->environment=std::move(env_);value->record_id=std::move(record_id);value->pipeline=std::move(pipeline);
                    foreign_compute[WithoutSizes(key)].push_back(std::move(value));
                } else compute_cache.emplace(key, std::move(pipeline));
            } else ++state.failures;
            ++state.built;
            report_build();
        });
    };
    const auto queue_graphics{[&](const GraphicsPipelineCacheKey& key,
                                  std::vector<FileEnvironment> envs, bool candidate=false, std::string record_id={}) {
        std::string inputs;
        for (const auto& env : envs) inputs += env.CompilationIdentity();
        const auto id=warmup_id('G',key,inputs);
        if (defer_warmup(id)) return;
        { std::scoped_lock lock{state.mutex}; ++state.total; }
        workers.QueueWork([this, key, id, candidate, stop_loading, record_id=std::move(record_id), envs_ = std::move(envs), &state, &report_build, &buffered_input_bytes, &foreign_budget]() mutable {
            if(stop_loading.stop_requested()) {
                std::scoped_lock lock{state.mutex};
                ++state.deferred;++state.built;report_build();return;
            }
            size_t reservation{};
            if(candidate) {
                std::scoped_lock lock{state.mutex};
                reservation=foreign_budget.Reserve(envs_.size(),
                    key.state.app_stage==Tegra::Engines::Maxwell3D::EngineHint::OnHLEMacro,
                    buffered_input_bytes);
                if(!reservation){++state.deferred;++state.built;report_build();return;}
            }
            SCOPE_EXIT { if(reservation){std::scoped_lock lock{state.mutex};foreign_budget.Release(reservation);} };
            Timing::Context timing_context(state.timings.get());
            ShaderPools pools;
            if(candidate)for(auto& env:envs_)env.BeginReuseProof(key.state.app_stage==Tegra::Engines::Maxwell3D::EngineHint::OnHLEMacro);
            boost::container::static_vector<Shader::Environment*, Maxwell::MaxShaderProgram> env_ptrs;
            for (auto& env : envs_) {
                env_ptrs.push_back(&env);
            }
            auto pipeline{CreateGraphicsPipeline(pools, key, MakeSpan(env_ptrs),
                                                 state.statistics.get(), false)};

            std::scoped_lock lock{state.mutex};
            if (state.warmup_failures) state.warmup_failures->Complete(id,pipeline && pipeline->IsBuilt());
            if (pipeline && pipeline->IsBuilt()) {
                if(candidate) {
                    size_t proof_bytes{};for(auto& env:envs_){env.SealReuseProof(program_equivalence.CanRebase(env.ProgramStorage(),env.ShaderStage(),env.StartAddress(),env.StartAddress()));proof_bytes+=env.ReuseProofBytes();}
                    if(!foreign_budget.Commit(reservation,proof_bytes,buffered_input_bytes)) {
                        ++state.deferred;++state.built;report_build();return;
                    }
                    reservation=0;
                    auto value=std::make_shared<ForeignGraphics>();value->key=key;
                    value->environments=std::move(envs_);value->record_id=std::move(record_id);value->pipeline=std::move(pipeline);
                    foreign_graphics[WithoutSizes(key)].push_back(std::move(value));
                } else graphics_cache.emplace(key, std::move(pipeline));
            } else { ++state.failures; }
            ++state.built;
            report_build();
        });
    }};
    // HLE macro pipelines without replacements wait until every key is known: those with a cached
    // non-HLE twin share its pipeline instead of building an identical one.
    struct DeferredPipeline {
        GraphicsPipelineCacheKey key;
        std::vector<FileEnvironment> envs;
    };
    std::vector<DeferredPipeline> hle_unreplaced;
    std::unordered_set<GraphicsPipelineCacheKey> plain_keys;
    const auto load_graphics{[&](std::istream& file, std::vector<FileEnvironment> envs, bool candidate=false, std::string record_id={}) {
        GraphicsPipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));

        size_t env_index{};
        for (u64 hash : key.unique_hashes) {
            if (hash != 0) {
                if (env_index >= envs.size()) {
                    LOG_ERROR(Render_Vulkan, "Cached graphics shader environment count mismatch");
                    return false;
                }
                if(!candidate)cbuf_size_dependencies.Observe(hash, envs[env_index].CbufSizeMask());
                ++env_index;
            }
        }
        if (env_index != envs.size()) {
            LOG_ERROR(Render_Vulkan, "Cached graphics shader environment count mismatch");
            return false;
        }
        if ((key.state.extended_dynamic_state != 0) !=
                dynamic_features.has_extended_dynamic_state ||
            (key.state.extended_dynamic_state_2 != 0) !=
                dynamic_features.has_extended_dynamic_state_2 ||
            (key.state.extended_dynamic_state_2_extra != 0) !=
                dynamic_features.has_extended_dynamic_state_2_extra ||
            (key.state.extended_dynamic_state_3_blend != 0) !=
                dynamic_features.has_extended_dynamic_state_3_blend ||
            (key.state.extended_dynamic_state_3_enables != 0) !=
                dynamic_features.has_extended_dynamic_state_3_enables ||
            (key.state.dynamic_vertex_input != 0) != dynamic_features.has_dynamic_vertex_input ||
            (key.state.vertex_stride_workaround != 0) !=
                (dynamic_features.has_vertex_stride_workaround &&
                 !dynamic_features.has_dynamic_vertex_input)) {
            return true;
        }
        if (!account_inputs(envs, candidate)) return true;
        graphics_inputs.push_back({key, std::move(envs), candidate, std::move(record_id)});
        return true;
    }};
    // Parse the existing modern store once. Its already-verified frame IDs prove
    // that a previous integration still exists, even after clean truncation.
    std::vector<VideoCommon::CacheStore::Record> cached_records;
    std::set<std::string> cached_record_ids;
    bool cached_store_valid{};
    Timing::Measure(state.timings.get(), Timing::Phase::ExistingCacheLoad, [&] {
        try {
            if (std::filesystem::exists(pipeline_cache_filename)) {
                // Regroup what earlier sessions appended into compressed blocks:
                // while loading, never while playing.
                try {
                    if (VideoCommon::CacheStore::Compact(pipeline_cache_filename, CACHE_VERSION))
                        LOG_INFO(Render_Vulkan, "Compacted the local shader cache");
                } catch (const std::exception& error) {
                    LOG_WARNING(Render_Vulkan, "Local shader cache not compacted: {}", error.what());
                }
                cached_records=VideoCommon::CacheStore::Read(pipeline_cache_filename,CACHE_VERSION,{},&cached_record_ids);
                cached_store_valid=true;
            }
        } catch(const std::exception& error) {
            LOG_ERROR(Render_Vulkan,"Local shader cache rejected without modifying source: {}",error.what());
        }
    });
    const auto read_small=[](const std::filesystem::path& file,size_t limit){
        if(std::filesystem::file_size(file)>limit)throw std::runtime_error("Profile metadata size limit");
        std::ifstream input(file,std::ios::binary);if(!input)throw std::runtime_error("Cannot read profile metadata");
        return std::string((std::istreambuf_iterator<char>(input)),{});
    };
    const auto publish_metadata=[](const std::filesystem::path& target,const JsonValue& metadata) {
        auto temporary=target;temporary+=".partial";
        std::filesystem::create_directories(target.parent_path());
        std::ofstream out(temporary,std::ios::binary|std::ios::trunc);
        out<<VideoCommon::ShaderProfile::Canonical(metadata);out.flush();out.close();
        if(!out)throw std::runtime_error("Cannot write local shader metadata");
#ifdef _WIN32
        if(!MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot publish local shader metadata");
#else
        std::filesystem::rename(temporary,target);
#endif
    };
    const auto profile_directory=VideoCommon::ShaderProfile::ProfileDirectory(
        Common::FS::GetYuzuPath(Common::FS::YuzuPath::YuzuDir));
    // A failure to create the drop folder must not prevent normal startup.
    if(!Common::FS::CreateDir(profile_directory))
        LOG_WARNING(Render_Vulkan,"Cannot create NXShader profile directory");
    // Profiles are verified/projected only during startup. Explicit CLI input wins.
    std::string import_path,import_root,import_stamp,integration_stamp,selection_request,recipe_scope;
    JsonValue integrated;
    std::map<std::string,std::string> reusable_recipes;
    const bool explicit_profile_request=[] {const char* p=std::getenv("NXEMU_IMPORT_NXSHADER");return p && *p;}();
    if(const char* option=std::getenv("NXEMU_IMPORT_NXSHADER");option && *option)import_path=option;
    if(const char* option=std::getenv("NXEMU_SHADER_GAME_ROOT");option && *option)import_root=option;
    try {

        if(import_path.empty() && !game_path.empty() && !capture_context.empty()) {
            // A dropped title-named profile takes precedence over an older GUI selection.
            const auto dropped=VideoCommon::ShaderProfile::FindProfileForContext(
                {profile_directory,base_dir,base_dir.parent_path()},title_id,capture_context);
            if(!dropped.empty()) {
                import_path=Common::FS::PathToUTF8String(dropped);
                selection_request=dropped.filename().string();
            }
        }
        if(import_path.empty() && std::filesystem::exists(VideoCommon::ShaderProfile::ImportSelectionPath(base_dir))) {
            const auto selection=VideoCommon::ShaderProfile::ParseMetadata(read_small(VideoCommon::ShaderProfile::ImportSelectionPath(base_dir),16384));
            const auto file=selection["file"].asString();
            if(file.empty() || file.find_first_of("/\\:\0",0,4)!=std::string::npos)
                throw std::runtime_error("Invalid community profile name");
            import_path=Common::FS::PathToUTF8String((selection["location"].asString()=="nxshader"?profile_directory:base_dir)/std::filesystem::u8path(file));
            import_root=selection["root"].asString();
            selection_request=file;
        }
        if(import_path.empty() && !game_path.empty() && !capture_context.empty()) {
            const auto profile=VideoCommon::ShaderProfile::FindProfileForContext(
                {std::filesystem::u8path(game_path).parent_path()},title_id,capture_context);
            if(!profile.empty())import_path=Common::FS::PathToUTF8String(profile);
        }
        // Always resolve automatic profiles against the currently launched local game.
        if(import_root.empty() && !game_path.empty())
            import_root=Common::FS::PathToUTF8String(std::filesystem::u8path(game_path).parent_path());
        if(!import_path.empty()) {
        u32 flags{};unsigned bit{};
        for(bool feature:{dynamic_features.has_extended_dynamic_state,dynamic_features.has_extended_dynamic_state_2,
            dynamic_features.has_extended_dynamic_state_2_extra,dynamic_features.has_extended_dynamic_state_3_blend,
            dynamic_features.has_extended_dynamic_state_3_enables,dynamic_features.has_dynamic_vertex_input,
            dynamic_features.has_vertex_stride_workaround})flags|=u32(feature)<<bit++;
        const auto profile_hash=VideoCommon::ShaderProfile::ResourceSha256(read_small(std::filesystem::u8path(import_path),VideoCommon::ShaderProfile::MaxProfileFileBytes));
        import_stamp=VideoCommon::ShaderProfile::ImportIdentity(profile_hash,content_identity,title_id,flags,CACHE_VERSION);
        integration_stamp=import_stamp.empty()?VideoCommon::ShaderProfile::ResourceSha256(
            "local-unidentified-integration-1:"+profile_hash+":"+std::to_string(title_id)+":"+std::to_string(flags)+":"+std::to_string(CACHE_VERSION)):import_stamp;
        recipe_scope=VideoCommon::ShaderProfile::ImportIdentity("",content_identity,title_id,flags,CACHE_VERSION);
        try {
            if(std::filesystem::exists(VideoCommon::ShaderProfile::ImportIntegratedPath(base_dir)))
                integrated=VideoCommon::ShaderProfile::ParseMetadata(read_small(VideoCommon::ShaderProfile::ImportIntegratedPath(base_dir),16*1024*1024),16*1024*1024);
        } catch(const std::exception&) { /* No trusted receipt; known content can be revalidated. */ }
        if(cached_store_valid) reusable_recipes=VideoCommon::ShaderProfile::ReusableRecipeRecords(integrated,recipe_scope,cached_record_ids);
        const bool present=cached_store_valid && VideoCommon::ShaderProfile::IntegratedRecordsPresent(
            integrated,integration_stamp,selection_request,cached_record_ids);
        std::string applied_selection=integrated["selection"].asString();
        try {
            const auto attempted_path=VideoCommon::ShaderProfile::ImportAttemptedPath(base_dir);
            if(std::filesystem::exists(attempted_path)) {
                const auto attempted=VideoCommon::ShaderProfile::ParseMetadata(read_small(attempted_path,16384),16384);
                if(attempted["stamp"].asString()==integration_stamp)
                    applied_selection=attempted["selection"].asString();
            }
        } catch(const std::exception&) { /* An invalid attempt receipt never proves integration. */ }
        if(!VideoCommon::ShaderProfile::ShouldReconstructProfile(!content_identity.empty(),explicit_profile_request,present,
            selection_request,applied_selection)) {
            if(!present && content_identity.empty())
                LOG_WARNING(Render_Vulkan,"Content identity unknown: retaining local cache without automatic profile reconstruction; explicitly select the profile to retry");
            import_path.clear();
        } else if(content_identity.empty() && !selection_request.empty()) {
            // Consume a GUI request even if reconstruction fails or is interrupted.
            // Unknown content requires a fresh explicit selection for another try.
            JsonValue attempted;attempted["stamp"]=integration_stamp;attempted["selection"]=selection_request;
            publish_metadata(VideoCommon::ShaderProfile::ImportAttemptedPath(base_dir),attempted);
        }
        }
    } catch(const std::exception& error){LOG_WARNING(Render_Vulkan,"Community profile selection rejected: {}",error.what());import_path.clear();}
    // Cache records and imported recipes share immutable byte strings. Deserialize
    // each distinct environment once, sharing its immutable program across keys.
    std::unordered_map<const void*, FileEnvironment> decoded_environments;
    std::unordered_map<const void*,std::string> program_identities;
    std::map<std::string,std::shared_ptr<const std::vector<u64>>> shared_programs;
    const auto deserialize_environment = [&](const VideoCommon::SharedShaderBytes& bytes) {
        const auto found = decoded_environments.find(bytes.Identity());
        if (found != decoded_environments.end()) return found->second.ShareCompilationInputs();
        std::istringstream stream(bytes.String(), std::ios::binary);
        std::string program_identity;
        if (bytes.ProgramIdentity()) {
            auto [it,inserted]=program_identities.try_emplace(bytes.ProgramIdentity());
            if(inserted)it->second=VideoCommon::ShaderProfile::ResourceSha256(bytes.ProgramBytes());
            program_identity=it->second;
        }
        const auto known=shared_programs.find(program_identity);
        FileEnvironment env;
        env.Deserialize(stream,known==shared_programs.end()?nullptr:known->second,program_identity);
        if(!program_identity.empty())shared_programs.try_emplace(program_identity,env.ProgramStorage());
        auto result = env.ShareCompilationInputs();
        decoded_environments.emplace(bytes.Identity(), std::move(env));
        return result;
    };
    std::vector<VideoCommon::CacheStore::Record> candidate_records;
    std::set<std::string> staged_candidate_ids;
    // Admission order of candidates: 0 for the selected profile, then other
    // local versions by recency. Unranked records come last.
    std::map<std::string,size_t> candidate_rank;
    size_t skipped_candidate_limit{};
    const auto stage_candidate=[&](VideoCommon::CacheStore::Record record) {
        if(!staged_candidate_ids.insert(record.id).second)return;
        if(!candidate_limit){++skipped_candidate_limit;return;}
        candidate_records.push_back(std::move(record));
    };
    bool import_records_ready{};
    size_t import_unresolved{},imported_records{};
    VideoCommon::ShaderProfile::ImportReport import_report;
    std::set<std::string> imported_record_ids,newly_loaded_ids;
    auto profile_progress_stage=VideoCore::LoadCallbackStage::Prepare;
    size_t last_profile_done=SIZE_MAX,last_profile_total=SIZE_MAX;
    auto last_profile_progress=std::chrono::steady_clock::time_point{};
    const auto profile_progress=[&](VideoCore::LoadCallbackStage stage,size_t done,size_t total) {
        if(stop_loading.stop_requested())return false;
        const auto now=std::chrono::steady_clock::now();
        const bool changed=stage!=profile_progress_stage;
        const bool endpoint=(done==0 || done==total) && (done!=last_profile_done || total!=last_profile_total);
        if(changed || endpoint || now-last_profile_progress>=std::chrono::milliseconds(50)) {
            profile_progress_stage=stage;last_profile_done=done;last_profile_total=total;last_profile_progress=now;
            callback(stage,done,total);
        }
        return true;
    };
    if (!import_path.empty()) {
        try {
            profile_progress(VideoCore::LoadCallbackStage::ReadProfile,0,0);
            const char* path=import_path.c_str();const char* root=import_root.c_str();
            if (!*root) throw std::runtime_error("Local game resource directory is required");
            size_t unresolved_count{};
            std::set<std::string> existing_recipes;
            for(const auto& [id,record]:reusable_recipes)existing_recipes.insert(id);
            VideoCommon::ShaderProfile::ProgramMap available_programs;
            std::unordered_set<const void*> indexed_programs;
            for(const auto& record:cached_records)for(const auto& env:record.environments) {
                if(env.ProgramIdentity() && indexed_programs.insert(env.ProgramIdentity()).second)
                    for(const auto hash:VideoCommon::ShaderProfile::ProgramHashCandidates(env.ProgramBytes()))
                        if(hash)available_programs.emplace(hash,env.ProgramStorage());
            }
            const auto recipes = VideoCommon::ShaderProfile::Read(
                std::filesystem::u8path(path), std::filesystem::u8path(root), title_id, &unresolved_count,
                state.timings.get(),[&](size_t done,size_t total){return profile_progress(VideoCore::LoadCallbackStage::ReadProfile,done,total);}, &import_report, capture_context, &existing_recipes, &available_programs, true);
            state.build_stage=VideoCore::LoadCallbackStage::BuildProfile;
            profile_progress(VideoCore::LoadCallbackStage::ImportProfile,0,recipes.size());
            // Validate every guest projection before queueing work or appending local records.
            for (const auto& recipe : recipes) {
                Timing::Timer projection_timer(state.timings.get(), Timing::Phase::GuestProjection);
                if (!recipe.compute && !GuestPipelineState::FromWords(recipe.guest_words))
                    throw std::runtime_error("Unsupported nxshader guest pipeline state");
            }
            std::map<std::string,std::string> recipe_records;
            for(const auto& id:import_report.reused_ids) {
                const auto& record=reusable_recipes.at(id);
                recipe_records.emplace(id,record); imported_record_ids.insert(record);
            }
            const auto publish_integration=[&](bool complete) {
                JsonValue receipt;
                receipt["schema"]=1U;receipt["complete"]=complete;receipt["stamp"]=integration_stamp;receipt["selection"]=selection_request;
                receipt["guarded_candidates"]=static_cast<u32>(import_report.candidates);
                receipt["skipped_recipes"]=static_cast<u32>(import_report.skipped);
                receipt["missing_resources"]=static_cast<u32>(import_report.missing_resources);
                receipt["incompatible_resources"]=static_cast<u32>(import_report.incompatible_resources);
                receipt["content_state"]=content_identity.empty()?"unknown":"identified";
                receipt["recipe_scope"]=recipe_scope;
                receipt["recipe_records"]=JsonValue(JsonValueType::Object);
                for(const auto& [recipe,record]:recipe_records)receipt["recipe_records"][recipe]=VideoCommon::ShaderProfile::LocalRecordHex(record);
                receipt["reused_recipes"]=static_cast<u32>(import_report.reused);
                receipt["incompatible_contexts"]=static_cast<u32>(import_report.incompatible_contexts);
                receipt["record_count"]=static_cast<u32>(imported_record_ids.size());
                receipt["record_ids"]=JsonValue(JsonValueType::Array);
                for(const auto& id:imported_record_ids)receipt["record_ids"].Append(VideoCommon::ShaderProfile::LocalRecordHex(id));
                publish_metadata(VideoCommon::ShaderProfile::ImportIntegratedPath(base_dir),receipt);
            };
            size_t imported{};
            // Integrate recipes for the running game first; optional foreign
            // contexts must not exhaust the input budget ahead of local work.
            for (bool foreign_pass : {false, true}) for (const auto& recipe : recipes) {
                if (recipe.requires_validation != foreign_pass) continue;
                if (stop_loading.stop_requested()) break;
                std::vector<FileEnvironment> envs;
                Timing::Timer environment_timer(state.timings.get(), Timing::Phase::EnvironmentDeserialization);
                if(!recipe.requires_validation)for (const auto& bytes : recipe.environments)
                    envs.push_back(deserialize_environment(bytes));
                environment_timer.Stop();
                VideoCommon::CacheStore::Record record;
                record.environments = recipe.environments;
                Timing::Timer projection_timer(state.timings.get(), Timing::Phase::GuestProjection);
                if (recipe.compute) {
                    ComputePipelineCacheKey key{};
                    key.unique_hash = recipe.shader_hashes[0];
                    key.shared_memory_size = recipe.shared_memory_size;
                    key.workgroup_size = recipe.workgroup_size;
                    key.cbuf_sizes = recipe.compute_cbuf_sizes;
                    key.texture_table_size = recipe.texture_table_size;
                    record.key.assign(reinterpret_cast<const char*>(&key), sizeof(key));

                } else {
                    const auto guest = GuestPipelineState::FromWords(recipe.guest_words);
                    if (!guest) throw std::runtime_error("Unsupported nxshader guest pipeline state");
                    GraphicsPipelineCacheKey key{};
                    key.unique_hashes = recipe.shader_hashes;
                    key.cbuf_sizes = recipe.cbuf_sizes;
                    key.texture_pool = recipe.texture_pool;
                    key.state = guest->Project(dynamic_features);
                    record.key.assign(reinterpret_cast<const char*>(&key), sizeof(key));

                }
                projection_timer.Stop();
                std::string id;
                Timing::Measure(state.timings.get(), Timing::Phase::CacheAppend, [&] {
                    VideoCommon::CacheStore::Append(pipeline_cache_filename, CACHE_VERSION, record, &id, recipe.requires_validation?recipe.source_context:cache_context);
                    newly_loaded_ids.insert(id);
                    recipe_records[recipe.identity]=id;
                    imported_record_ids.insert(id);
                });
                if(recipe.requires_validation) {
                    record.id=id;stage_candidate(std::move(record));
                } else {
                    std::istringstream stream(record.key,std::ios::binary);
                    if(recipe.compute)load_compute(stream,std::move(envs.front()),false,id);
                    else load_graphics(stream,std::move(envs),false,id);
                }
                ++imported;
                profile_progress(VideoCore::LoadCallbackStage::ImportProfile,imported,recipes.size());
                if(imported%128==0)publish_integration(false);
            }
            if (imported==recipes.size() && !stop_loading.stop_requested()) {
                publish_integration(true);
                import_records_ready=true;
            } else if (!recipe_records.empty()) publish_integration(false);
            import_unresolved=unresolved_count;imported_records=imported;
            LOG_INFO(Render_Vulkan, "NXShader imported {} recipes; reused={} skipped={} missing_resources={} incompatible_resources={} incompatible_contexts={}",
                imported, import_report.reused, import_report.skipped, import_report.missing_resources, import_report.incompatible_resources, import_report.incompatible_contexts);
            if (unresolved_count) LOG_WARNING(Render_Vulkan,
                "NXShader retains {} observations without serialized environments; these cannot be prewarmed", unresolved_count);
        } catch (const std::exception& error) {
            state.build_stage=VideoCore::LoadCallbackStage::Build;
            LOG_ERROR(Render_Vulkan, "NXShader import failed: {}", error.what());
            callback(VideoCore::LoadCallbackStage::Prepare,0,0);
        }
    }
    // Other-version records from this selected profile remain guarded candidates.
    std::set<std::string> selected_ids=imported_record_ids;
    if(!integration_stamp.empty() && integrated["stamp"].asString()==integration_stamp && integrated["record_ids"].isArray())
        for(const auto& value:std::as_const(integrated)["record_ids"]) {
            const auto id=VideoCommon::ShaderProfile::LocalRecordBinary(value.asString());
            if(cached_record_ids.contains(id))selected_ids.insert(id);
        }
    // Warm verified current-context records directly; candidates stay separate.
    Timing::Timer existing_cache_timer(state.timings.get(), Timing::Phase::ExistingCacheLoad);
    const auto graphics_before = graphics_inputs.size(), compute_before = compute_inputs.size();
    bool modern_loaded{import_records_ready};
    try {
        if (cached_store_valid) {
            const auto contextual_ids = VideoCommon::CacheStore::ReadContext(pipeline_cache_filename,cache_context);
            if (!progressive_mode && !capture_context.empty()) {
                // A second start of an update must not hide the other local
                // versions. Keep their entries as guarded candidates on every
                // start, without ever treating them as verified target records.
                std::vector<std::pair<std::filesystem::file_time_type,std::string>> others;
                std::error_code scan_error;
                for(auto entry=std::filesystem::directory_iterator(VideoCommon::ShaderProfile::VersionsDirectory(base_dir),scan_error);
                    !scan_error && entry!=std::filesystem::directory_iterator{};entry.increment(scan_error)) {
                    const auto context=VideoCommon::ShaderProfile::VersionContext(entry->path());
                    if(context.empty() || context==capture_context)continue;
                    if(others.size()>=64) {
                        LOG_WARNING(Render_Vulkan,"Other shader contexts limited to 64 local versions");
                        break;
                    }
                    std::error_code time_error;
                    const auto played=std::filesystem::last_write_time(
                        VideoCommon::CacheStore::ContextIndexPath(pipeline_cache_filename,context),time_error);
                    others.emplace_back(time_error?std::filesystem::file_time_type{}:played,context);
                }
                if(scan_error && scan_error!=std::errc::no_such_file_or_directory)
                    LOG_WARNING(Render_Vulkan,"Cannot enumerate optional shader contexts: {}",scan_error.message());
                // The version played most recently is usually the closest one:
                // its entries go first, so older versions cannot fill the budget.
                std::sort(others.begin(),others.end(),std::greater<>{});
                for(size_t rank=0;rank<others.size();++rank) {
                    try {
                        for(const auto& id:VideoCommon::CacheStore::ReadContext(pipeline_cache_filename,others[rank].second))
                            if(cached_record_ids.contains(id))candidate_rank.emplace(id,rank+1);
                    } catch(const std::exception& error) {
                        LOG_WARNING(Render_Vulkan,"Other shader context ignored: {}",error.what());
                    }
                }
            }
            // Read the running game's cache first. Foreign records are optional
            // and must not consume the bounded input pool ahead of local entries.
            for (bool foreign_pass : {false, true}) for(const auto& record:cached_records) {
                const bool candidate=!contextual_ids.contains(record.id);
                if (candidate != foreign_pass) continue;
                if(candidate && (capture_context.empty() ||
                    (!selected_ids.contains(record.id) && !candidate_rank.contains(record.id)) ||
                    newly_loaded_ids.contains(record.id)))continue;
                if(stop_loading.stop_requested())break;
                if(candidate){stage_candidate(record);continue;}
                std::vector<FileEnvironment> envs;
                for(const auto& bytes:record.environments) {
                    envs.push_back(deserialize_environment(bytes));
                }
                std::istringstream key(record.key,std::ios::binary);
                key.exceptions(std::ios::failbit|std::ios::badbit);
                if(envs.front().ShaderStage()==Shader::Stage::Compute) {
                    if(envs.size()!=1)throw std::runtime_error("Invalid compute cache stage count");
                    load_compute(key,std::move(envs.front()),candidate,record.id);
                }
                else load_graphics(key,std::move(envs),candidate,record.id);
            }
            cached_records.clear();
            modern_loaded = import_records_ready || graphics_inputs.size() != graphics_before || compute_inputs.size() != compute_before;
        }
    } catch (const std::exception& error) {
        graphics_inputs.resize(graphics_before); compute_inputs.resize(compute_before);
        LOG_ERROR(Render_Vulkan, "Local shader cache rejected: {}", error.what());
        import_records_ready = false;
        std::error_code marker_error; std::filesystem::remove(VideoCommon::ShaderProfile::ImportStatusPath(base_dir), marker_error);
    }
    // Admit the selected profile's candidates first, then other local versions
    // from the most recently played one. Stable: file order within each source.
    for(const auto& id:selected_ids)candidate_rank[id]=0;
    std::stable_sort(candidate_records.begin(),candidate_records.end(),[&](const auto& a,const auto& b){
        const auto rank=[&](const std::string& id){const auto it=candidate_rank.find(id);return it==candidate_rank.end()?SIZE_MAX:it->second;};
        return rank(a.id)<rank(b.id);
    });
    // Filter covered keys before applying the count limit: native duplicates
    // must not crowd out later foreign records that could actually be useful.
    std::unordered_set<ComputePipelineCacheKey> native_compute_keys;
    std::unordered_set<GraphicsPipelineCacheKey> native_graphics_keys;
    for(const auto& input:compute_inputs)native_compute_keys.insert(WithoutSizes(input.key));
    for(const auto& input:graphics_inputs)native_graphics_keys.insert(WithoutSizes(input.key));
    size_t admitted_candidate_inputs{},covered_candidates{};
    // Required inputs from both sources now own the budget. Optional candidates
    // cannot invalidate them; bad or oversized candidate records are skipped alone.
    for(const auto& record:candidate_records) {
        if(stop_loading.stop_requested())break;
        try {
            if(record.key.size()==sizeof(ComputePipelineCacheKey)) {
                ComputePipelineCacheKey key{};
                std::memcpy(&key,record.key.data(),sizeof(key));
                if(native_compute_keys.contains(WithoutSizes(key)) && !comparing_foreign){++covered_candidates;continue;}
            } else if(record.key.size()==sizeof(GraphicsPipelineCacheKey)) {
                GraphicsPipelineCacheKey key{};
                std::memcpy(&key,record.key.data(),sizeof(key));
                if(native_graphics_keys.contains(WithoutSizes(key)) && !comparing_foreign){++covered_candidates;continue;}
            } else throw std::runtime_error("Invalid candidate key size");
            if(admitted_candidate_inputs>=candidate_limit){++skipped_candidate_limit;continue;}
            const auto previous=compute_inputs.size()+graphics_inputs.size();
            std::vector<FileEnvironment> envs;
            for(const auto& bytes:record.environments)envs.push_back(deserialize_environment(bytes));
            std::istringstream key(record.key,std::ios::binary);
            key.exceptions(std::ios::failbit|std::ios::badbit);
            if(envs.front().ShaderStage()==Shader::Stage::Compute) {
                if(envs.size()!=1)throw std::runtime_error("Invalid candidate stage count");
                load_compute(key,std::move(envs.front()),true,record.id);
            } else load_graphics(key,std::move(envs),true,record.id);
            admitted_candidate_inputs+=(compute_inputs.size()+graphics_inputs.size()!=previous);
        } catch(const std::exception& error) {
            ++skipped_candidate_inputs;
            LOG_WARNING(Render_Vulkan,"Skipping optional shader candidate: {}",error.what());
        }
    }
    candidate_records.clear();
    decoded_environments.clear();
    shared_programs.clear();
    program_identities.clear();
    existing_cache_timer.Stop();

    // The hardware/build scope is stable across unrelated cache growth. Each
    // failure ID also contains its own verified compiler inputs.
    if ((cached_store_valid || import_records_ready) && modern_loaded) {
        const auto properties=device.GetPhysical().GetProperties();
        std::string scope="native-warmup-2:" __DATE__ ":" __TIME__;
        scope+=":"+std::to_string(CACHE_VERSION)+":"+std::to_string(properties.vendorID)+
            ":"+std::to_string(properties.deviceID)+":"+std::to_string(device.GetDriverVersion())+
            ":"+std::to_string(device.GetDriverID())+":"+std::string(device.GetModelName());
        scope.append(reinterpret_cast<const char*>(properties.pipelineCacheUUID),VK_UUID_SIZE);
        state.warmup_failures=std::make_unique<VideoCommon::ShaderProfile::NativeWarmupFailures>(
            VideoCommon::ShaderProfile::ResourceSha256(scope));
        try {
            const auto path=VideoCommon::ShaderProfile::WarmupPath(base_dir);
            if(std::filesystem::exists(path))
                state.warmup_failures->Load(VideoCommon::ShaderProfile::ParseMetadata(read_small(path,4*1024*1024),4*1024*1024));
        } catch(const std::exception& e) {
            LOG_WARNING(Render_Vulkan,"Ignoring invalid local warmup backoff: {}",e.what());
        }
    }

    // Wait until all records are known before choosing a candidate. Equality of
    // keys alone cannot select among different constants, texture answers or HLE replacements.
    const char* alias_audit_path = std::getenv("NXEMU_NXSHADER_AUDIT_HLE_ALIASES");
    const char* variant_audit_path = std::getenv("NXEMU_NXSHADER_AUDIT_VARIANTS");
    if (cross_audit_path && *cross_audit_path) variant_audit_path = cross_audit_path;
    if (alias_audit_path && *alias_audit_path) variant_audit_path = alias_audit_path;
    struct AuditPair { GraphicsPipelineCacheKey key; std::vector<FileEnvironment> first, second; GraphicsPipelineCacheKey second_key; };
    std::vector<AuditPair> audit_pairs;
    size_t audit_bytes{}; bool audit_truncated{};
    const auto clone=[](const auto& envs) {
        std::vector<FileEnvironment> result;result.reserve(envs.size());
        for(const auto& env:envs)result.push_back(env.CloneForDiagnostic());
        return result;
    };
    // Explicit offline diagnostic: compare both HLE hints using their own key and
    // captured environment. This never changes which entries are safe to warm.
    if (alias_audit_path && *alias_audit_path) {
        using EngineHint = Tegra::Engines::Maxwell3D::EngineHint;
        std::unordered_map<GraphicsPipelineCacheKey, size_t> plain;
        for (size_t i = 0; i < graphics_inputs.size(); ++i) {
            auto key = Normalized(graphics_inputs[i].key);
            if (key.state.app_stage == EngineHint::None) plain.try_emplace(key, i);
        }
        std::unordered_set<GraphicsPipelineCacheKey> seen;
        for (const auto& input : graphics_inputs) {
            auto key = Normalized(input.key);
            if (key.state.app_stage != EngineHint::OnHLEMacro ||
                std::ranges::any_of(input.envs, [](const auto& env) { return env.HasHLEMacroState(); })) continue;
            auto twin = key;
            twin.state.app_stage.Assign(EngineHint::None);
            const auto it = plain.find(twin);
            if (it == plain.end() || !seen.insert(key).second) continue;
            const auto& other = graphics_inputs[it->second].envs;
            size_t bytes{};
            for (const auto& env : input.envs) bytes += env.CompilationInputBytes();
            for (const auto& env : other) bytes += env.CompilationInputBytes();
            if (audit_pairs.size() < 256 && bytes <= 32*1024*1024ULL-audit_bytes) {
                audit_bytes += bytes;
                audit_pairs.push_back({key, clone(input.envs), clone(other), twin});
            } else audit_truncated = true;
        }
    }
    // Explicit offline comparison only: never admit an unverified foreign entry
    // into the active cache. Compare each captured version with its own real key.
    if (cross_audit_path && *cross_audit_path) {
        std::unordered_map<GraphicsPipelineCacheKey, std::vector<size_t>> foreign;
        for (size_t i = 0; i < graphics_inputs.size(); ++i)
            if (graphics_inputs[i].candidate)
                foreign[WithoutSizes(graphics_inputs[i].key)].push_back(i);
        for (const auto& input : graphics_inputs) {
            if (input.candidate) continue;
            const auto found = foreign.find(WithoutSizes(input.key));
            if (found == foreign.end()) continue;
            for (const auto index : found->second) {
                const auto& previous = graphics_inputs[index];
                size_t bytes{};
                for (const auto& env : previous.envs) bytes += env.CompilationInputBytes();
                for (const auto& env : input.envs) bytes += env.CompilationInputBytes();
                if (audit_pairs.size() < 256 && bytes <= 32*1024*1024ULL-audit_bytes) {
                    audit_bytes += bytes;
                    audit_pairs.push_back({previous.key,clone(previous.envs),clone(input.envs),input.key});
                } else audit_truncated = true;
            }
        }
    }
    StartupPipelineVariants<GraphicsPipelineCacheKey, std::vector<FileEnvironment>> graphics_variants;
    StartupPipelineVariants<ComputePipelineCacheKey, FileEnvironment> compute_variants;
    // Normal game pipelines must be scheduled ahead of optional update candidates.
    std::vector<ComputeInput> foreign_compute_inputs;
    std::vector<GraphicsInput> foreign_graphics_inputs;
    std::unordered_set<std::string> prepared_candidates;
    std::unordered_set<ComputePipelineCacheKey> ready_compute;
    std::unordered_set<GraphicsPipelineCacheKey> ready_graphics;
    for(const auto& input:compute_inputs)if(!input.candidate)ready_compute.insert(WithoutSizes(input.key));
    for(const auto& input:graphics_inputs)if(!input.candidate)ready_graphics.insert(WithoutSizes(input.key));
    if (progressive_mode) {
        // A stable estimate keeps the loading bar moving during timed waves.
        // Final progress is clamped to the actual initial request count below.
        state.progress_total = compute_inputs.size() + graphics_inputs.size();
    }
    for (auto& input : compute_inputs) {
        if (input.candidate) { foreign_compute_inputs.push_back(std::move(input)); continue; }
        CbufSizeDependencies::Normalize(input.key.cbuf_sizes, cbuf_size_dependencies.Mask(input.key.unique_hash));
        compute_variants.Observe(input.key, std::move(input.env), [](const auto& a, const auto& b) {
            return a.HasSameCompilationInputs(b);
        });
    }
    compute_inputs.clear();
    for (auto& input : graphics_inputs) {
        if (input.candidate) { foreign_graphics_inputs.push_back(std::move(input)); continue; }
        NormalizeCbufSizes(input.key);
        graphics_variants.Observe(input.key, std::move(input.envs), [&](const auto& a, const auto& b) {
            bool equal=a.size()==b.size();
            for(size_t i=0;equal && i<a.size();++i)equal=a[i].HasSameCompilationInputs(b[i]);
            if(!equal && variant_audit_path && *variant_audit_path) {
                size_t bytes{};for(const auto& env:a)bytes+=env.CompilationInputBytes();for(const auto& env:b)bytes+=env.CompilationInputBytes();
                if(audit_pairs.size()<256 && bytes<=32*1024*1024ULL-audit_bytes) {
                    audit_bytes+=bytes;audit_pairs.push_back({input.key,clone(a),clone(b),input.key});
                } else audit_truncated=true;
            }
            return equal;
        });
    }
    graphics_inputs.clear();
    if(variant_audit_path && *variant_audit_path) {
        JsonValue report(JsonValueType::Object);report["schema"]=2U;report["truncated"]=audit_truncated;
        report["shader_instructions_included"]=false;report["pairs"]=JsonValue(JsonValueType::Array);
        size_t same{},different{},failed{},nondeterministic{},unsupported{};
        for(auto& pair:audit_pairs) {
            if(stop_loading.stop_requested()){report["cancelled"]=true;break;}
            const auto compile=[&](const auto& key,auto envs,auto& hashes,
                                   std::vector<FileEnvironment>* targets=nullptr,
                                   const GraphicsPipelineCacheKey* target_key=nullptr,
                                   bool* proof_matches=nullptr){
                ShaderPools pools;VideoCommon::ShaderProfile::Audit::Context audit(hashes);
                boost::container::static_vector<Shader::Environment*,Maxwell::MaxShaderProgram> pointers;
                for(auto& env:envs) {
                    if(comparing_foreign)env.BeginReuseProof(key.state.app_stage==Tegra::Engines::Maxwell3D::EngineHint::OnHLEMacro);
                    pointers.push_back(&env);
                }
                auto pipeline=CreateGraphicsPipeline(pools,key,MakeSpan(pointers),nullptr,false);
                if(proof_matches && targets && target_key) {
                    *proof_matches=envs.size()==targets->size();
                    for(size_t i=0;*proof_matches && i<envs.size();++i) {
                        auto& old=envs[i];auto& target=(*targets)[i];
                        const bool relative=old.StartAddress()!=target.StartAddress() &&
                            program_equivalence.CanRebase(old.ProgramStorage(),old.ShaderStage(),old.StartAddress(),target.StartAddress());
                        *proof_matches=old.HasReusableProgramLayout() &&
                            program_equivalence.MatchDigest(old.ProgramDigest(),old.ProgramBytes(),target.ProgramStorage());
                        old.SealReuseProof(program_equivalence.CanRebase(old.ProgramStorage(),old.ShaderStage(),old.StartAddress(),old.StartAddress()));
                        VideoCommon::ShaderProfile::Audit::CapturedEnvironment live(target,
                            target_key->state.app_stage==Tegra::Engines::Maxwell3D::EngineHint::OnHLEMacro);
                        try {*proof_matches=*proof_matches && old.ReuseProof().Matches(live,relative);}
                        catch(const std::exception&){*proof_matches=false;}
                    }
                }
                return pipeline && pipeline->IsBuilt() && !hashes.spirv.empty() && hashes.interfaces_captured;
            };
            VideoCommon::ShaderProfile::Audit::Capture a,b,control;
            bool proof_matches{};
            const bool valid=compile(pair.key,clone(pair.first),a,&pair.second,&pair.second_key,&proof_matches) && compile(pair.key,clone(pair.first),control) && compile(pair.second_key,clone(pair.second),b);
            JsonValue result(JsonValueType::Object);result["key_hash"]=std::to_string(pair.key.Hash());
            result["dependency_proof_matches_captured_target"]=proof_matches;
            result["second_key_hash"]=std::to_string(pair.second_key.Hash());
            result["native_creations_succeeded"]=valid;
            result["difference_masks"]=JsonValue(JsonValueType::Array);
            for(size_t i=0;i<pair.first.size() && i<pair.second.size();++i)
                result["difference_masks"].Append(pair.first[i].CompilationDifferenceMask(pair.second[i]));
            const auto array=[](const auto& values){JsonValue out(JsonValueType::Array);for(const auto& v:values)out.Append(v);return out;};
            result["first_spirv"]=array(a.spirv);result["control_spirv"]=array(control.spirv);result["second_spirv"]=array(b.spirv);
            result["spirv_equal"]=a.spirv==b.spirv;
            result["interfaces_equal"]=a.interfaces==b.interfaces;
            result["rasterization_equal"]=a.rasterizes_lines==b.rasterizes_lines;
            result["interface_supported"]=!a.has_vtg && !b.has_vtg && !control.has_vtg;
            if(!valid){++failed;result["result"]="failed";}
            else if(a!=control){++nondeterministic;result["result"]="nondeterministic";}
            else if(a.has_vtg || b.has_vtg){++unsupported;result["result"]="unsupported_vtg_interface";}
            else if(a==b){++same;result["result"]="identical_spirv_and_interfaces";}
            else{++different;result["result"]="different_spirv_or_interfaces";}
            report["pairs"].Append(std::move(result));
        }
        report["identical"]=static_cast<u32>(same);report["different"]=static_cast<u32>(different);
        report["unsupported"]=static_cast<u32>(unsupported);report["failed"]=static_cast<u32>(failed);report["nondeterministic"]=static_cast<u32>(nondeterministic);
        std::ofstream file(std::filesystem::u8path(variant_audit_path),std::ios::binary|std::ios::trunc);
        file<<VideoCommon::ShaderProfile::Canonical(report);
        LOG_INFO(Render_Vulkan,"Startup environment audit: identical={} different={} failed={} nondeterministic={} truncated={}",same,different,failed,nondeterministic,audit_truncated);
    }
    if (progressive_mode) {
        std::scoped_lock lock{state.mutex};
        callback(state.build_stage,0,state.progress_total);
        state.has_loaded = true;
        state.last_build_progress = std::chrono::steady_clock::now();
    }
    const auto defer_initial = [&] {
        if (!progressive_mode) return false;
        if (initial_budget.NeedsCheckpoint(initial_wave)) {
            workers.WaitForRequests(stop_loading);
            if (stop_loading.stop_requested()) workers.WaitForRequests();
        }
        return (progressive_mode && stop_loading.stop_requested()) || initial_budget.DeferNext();
    };
    const auto schedule_compute = [&](const ComputePipelineCacheKey& key, FileEnvironment env) {
        if (defer_initial())
            progressive_jobs.emplace_back(ProgressiveWarmup::Compute{key, std::move(env)});
        else { queue_compute(key, std::move(env)); }
    };
    const auto schedule_graphics = [&](const GraphicsPipelineCacheKey& key, std::vector<FileEnvironment> envs) {
        if (defer_initial())
            progressive_jobs.emplace_back(ProgressiveWarmup::Graphics{key, std::move(envs)});
        else { queue_graphics(key, std::move(envs)); }
    };
    const auto ambiguous_compute = compute_variants.ConsumeSafe(schedule_compute,[&](const auto& key){if(runtime_report)quarantined_compute.Reject(key);});
    const auto ambiguous_graphics = graphics_variants.ConsumeSafe([&](const auto& key, auto envs) {
        using EngineHint = Tegra::Engines::Maxwell3D::EngineHint;
        if (key.state.app_stage != EngineHint::OnHLEMacro) plain_keys.insert(key);
        else if (std::ranges::none_of(envs, [](const auto& env) { return env.HasHLEMacroState(); })) {
            hle_unreplaced_keys.insert(key);
            hle_unreplaced.push_back({key, std::move(envs)});
            return;
        }
        schedule_graphics(key, std::move(envs));
    },[&](const auto& key){if(runtime_report)quarantined_graphics.Reject(key);});
    if(ambiguous_compute || ambiguous_graphics)
        LOG_WARNING(Render_Vulkan, "Skipped ambiguous startup keys: graphics={} compute={}; keeping every disk variant for investigation and normal runtime compilation", ambiguous_graphics, ambiguous_compute);

    const auto non_hle_twin{[](GraphicsPipelineCacheKey key) {
        key.state.app_stage.Assign(Tegra::Engines::Maxwell3D::EngineHint::None);
        return key;
    }};
    std::vector<GraphicsPipelineCacheKey> hle_shared_keys;
    for (auto& [key, envs] : hle_unreplaced) {
        if (plain_keys.contains(non_hle_twin(key))) {
            hle_shared_keys.push_back(key);
        } else {
            schedule_graphics(key, std::move(envs));
        }
    }
    // Only after scheduling all current-context work, prepare eligible foreign
    // pipelines. A rejected/limited candidate still uses normal runtime compilation.
    // Cross-version candidates use the existing full-mode proof/budget path.
    // In progressive mode they remain available for ordinary runtime compilation;
    // do not carry stack-owned proof budgets into gameplay.
    for (auto& input : foreign_compute_inputs) {
        if (progressive_mode) continue;
        if (!ready_compute.contains(WithoutSizes(input.key)) &&
            prepared_candidates.insert(input.record_id).second)
            queue_compute(input.key, std::move(input.env), true, std::move(input.record_id));
    }
    foreign_compute_inputs.clear();
    for (auto& input : foreign_graphics_inputs) {
        if (progressive_mode) continue;
        if (!ready_graphics.contains(WithoutSizes(input.key)) &&
            prepared_candidates.insert(input.record_id).second)
            queue_graphics(input.key, std::move(input.envs), true, std::move(input.record_id));
    }
    foreign_graphics_inputs.clear();
    LOG_INFO(Render_Vulkan, "Total Pipeline Count: {}", state.total);

    std::unique_lock lock{state.mutex};
    state.progress_total = 0;
    callback(state.build_stage,state.built,state.total);
    state.last_build_progress=std::chrono::steady_clock::now();
    state.has_loaded = true;
    lock.unlock();

    Timing::Measure(state.timings.get(), Timing::Phase::WarmupWait, [&] {
        workers.WaitForRequests(stop_loading);
        // Cancellation wakes the first wait early. Drain in-flight work before
        // inspecting shared maps/statistics; queued jobs now skip compilation.
        if(stop_loading.stop_requested())workers.WaitForRequests();
    });

    lock.lock();
    LOG_INFO(Render_Vulkan,"Prepared cross-version candidate keys: graphics={} compute={}",foreign_graphics.size(),foreign_compute.size());
    LOG_INFO(Render_Vulkan,
             "Foreign warmup: accepted={} skipped_by_admission={} skipped_by_input_budget={} skipped_by_limit={} covered_by_native={} outstanding_proof_bytes={} retained_input_and_proof_bytes={}",
             foreign_budget.Accepted(), foreign_budget.Skipped(), skipped_candidate_inputs, skipped_candidate_limit, covered_candidates, foreign_budget.Reserved(), buffered_input_bytes);
    const size_t completed_requests=state.built,failed_requests=state.failures;
    for (const auto& key : hle_shared_keys) {
        const auto it{graphics_cache.find(non_hle_twin(key))};
        if (it != graphics_cache.end() && it->second) {
            const std::shared_ptr<GraphicsPipeline> twin{it->second};
            twin->SetSharedKey(key);
            graphics_cache.emplace(key, twin);
        }
    }
    lock.unlock();

    if (state.warmup_failures && !stop_loading.stop_requested() && progressive_jobs.empty() && !progressive_mode) {
        try {
            auto report=state.warmup_failures->Save();
            report["queued"]=static_cast<u32>(state.total);
            report["completed"]=static_cast<u32>(completed_requests);
            report["failed"]=static_cast<u32>(failed_requests);
            report["deferred"]=static_cast<u32>(state.deferred);
            publish_metadata(VideoCommon::ShaderProfile::WarmupPath(base_dir),report);
            LOG_INFO(Render_Vulkan,"Native warmup: completed={} failed={} deferred={}",completed_requests,failed_requests,state.deferred);
        } catch(const std::exception& e) {
            LOG_WARNING(Render_Vulkan,"Cannot save local warmup backoff: {}",e.what());
        }
    }

    // This marker means the compatible subset was imported AND all its queued
    // native creations completed. It is never written merely after queueing work.
    if (progressive_jobs.empty() && !progressive_mode &&
        VideoCommon::ShaderProfile::CanPublishImport(import_records_ready,import_stamp,stop_loading.stop_requested(),
        completed_requests,state.total,failed_requests+state.deferred)) {
        JsonValue status(JsonValueType::Object);
        status["schema"]=2U;status["stamp"]=import_stamp;
        status["status"]=(foreign_graphics.empty() && foreign_compute.empty() &&
            foreign_budget.Skipped()==0 && skipped_candidate_inputs==0 && skipped_candidate_limit==0)?
            "ready-compatible-subset":"ready-pending-update-validation";
        const auto number=[](size_t n){return JsonValue(static_cast<u32>(n));};
        status["records_imported"]=number(imported_records);status["unresolved"]=number(import_unresolved);
        status["ambiguous_graphics"]=number(ambiguous_graphics);status["ambiguous_compute"]=number(ambiguous_compute);
        status["candidate_prepared"]=number(foreign_budget.Accepted());
        status["candidate_skipped"]=number(foreign_budget.Skipped()+skipped_candidate_inputs+skipped_candidate_limit);
        status["native_queued"]=number(state.total);status["native_completed"]=number(completed_requests);status["native_failures"]=0U;
        try { publish_metadata(VideoCommon::ShaderProfile::ImportStatusPath(base_dir),status); }
        catch(const std::exception& e) { LOG_WARNING(Render_Vulkan,"Cannot publish community warmup completion: {}",e.what()); }
    }
    if (use_vulkan_pipeline_cache && !vulkan_pipeline_cache_filename.empty()) {
        SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                     CACHE_VERSION);
    }

    if (state.statistics) {
        state.statistics->Report();
    }
    if (state.timings) {
        state.timings->Add(Timing::Counter::HleAliases, hle_shared_keys.size());
        state.timings->Add(Timing::Counter::PipelineRequests, state.total);
        state.timings->Add(Timing::Counter::PipelineRequestsCompleted, completed_requests);
        state.timings->Add(Timing::Counter::StopRequested, stop_loading.stop_requested());
        startup_timer.Stop();
        const auto report = state.timings->Json();
        LOG_INFO(Render_Vulkan, "NXShader timings: {}", report);
        if (const char* output = std::getenv("NXEMU_NXSHADER_TIMING_FILE"); output && *output) {
            std::ofstream file(std::filesystem::u8path(output), std::ios::binary | std::ios::trunc);
            file << report;
            if (!file) LOG_WARNING(Render_Vulkan, "Cannot write NXShader timing report");
        }
    }
    if(runtime_report)runtime_report->active=true;
    // All disk-loader workers have drained. The remaining jobs now own their
    // compiler inputs and use only the persistent runtime queue.
    if (progressive_mode && !stop_loading.stop_requested() && !progressive_jobs.empty()) {
        LOG_INFO(Render_Vulkan, "NXShader progressive: {} initial jobs, {} deferred native builds",
                 initial_budget.Scheduled(), progressive_jobs.size());
        progressive_warmup = std::make_unique<ProgressiveWarmup>(
            *this, std::move(progressive_jobs), std::move(hle_shared_keys));
    }
}

GraphicsPipeline* PipelineCache::WaitForCurrentGraphicsPipeline() {
    if (!skipped_unfinished) {
        return nullptr;
    }
    skipped_unfinished = false;
    wait_for_current_draw = true;
    SCOPE_EXIT { wait_for_current_draw = false; };
    return CurrentGraphicsPipelineSlowPath();
}

bool PipelineCache::ShouldWaitForAsyncPipeline() const noexcept {
    if (wait_for_current_draw) {
        return true;
    }
    // Transform feedback writes buffers that later draws read, so skipping it is not temporary.
    if (maxwell3d->regs.transform_feedback_enabled != 0) {
        return true;
    }
    if (maxwell3d->regs.zeta_enable) {
        return false;
    }
    const auto& draw = maxwell3d->draw_manager->GetDrawState();
    // The unused count can be zero or stale after a different kind of draw.
    return (draw.draw_indexed ? draw.index_buffer.count : draw.vertex_buffer.count) <= 6;
}

bool PipelineCache::QueueGraphicsPipeline(const GraphicsPipelineCacheKey& key) {
    // Backpressure falls back to synchronous compilation instead of holding an
    // unbounded number of guest-memory snapshots or blocking on a queue slot.
    constexpr size_t MaxPending = 512;
    if (pending_graphics.size() >= MaxPending) {
        return false;
    }
    u32 stage_mask{};
    for (size_t index = 0; index < key.unique_hashes.size(); ++index) {
        if (key.unique_hashes[index]) {
            stage_mask |= 1U << (index == 0 ? 0 : index - 1);
        }
    }
    auto snapshot = VideoCommon::GraphicsEnvironment::CaptureSnapshot(
        *maxwell3d, *gpu_memory, stage_mask, snapshot_cache);
    if (!snapshot) {
        return false;
    }
    auto job = std::make_shared<PendingGraphicsPipeline>();
    GetGraphicsEnvironments(job->environments, key.unique_hashes);
    for (size_t index = 0; index < key.unique_hashes.size(); ++index) {
        if (key.unique_hashes[index]) {
            job->environments.envs[index].SetSnapshot(snapshot);
        }
    }
    job->key = key;
    if (HasUnknownSizes(key)) {
        pending_unknown_sizes.try_emplace(WithoutSizes(key), job);
    }
    pending_graphics.emplace(key, job);
    try {
        job->request = runtime_builds.QueueWork([this, key, job] {
            if (!job->build.TryBuild([&] {
                    // Reuse the IR pools on this compiler thread. All engine and
                    // guest-memory reads use the immutable captured environments.
                    thread_local ShaderPools pools;
                    pools.ReleaseContents();
                    return CreateGraphicsPipeline(pools, key, job->environments.Span(), nullptr, false);
                })) {
                return;
            }
            std::scoped_lock lock{completed_mutex};
            completed_graphics.push_back(key);
            has_completed_graphics.store(true, std::memory_order_release);
        }, false);
    } catch (...) {
        pending_graphics.erase(key);
        if (const auto it = pending_unknown_sizes.find(WithoutSizes(key));
            it != pending_unknown_sizes.end() && it->second == job) {
            pending_unknown_sizes.erase(it);
        }
        throw;
    }
    return true;
}

void PipelineCache::SerializeGraphicsEnvironments(const GraphicsPipelineCacheKey& key,
                                                 GraphicsEnvironments& environments) {
    if (pipeline_cache_filename.empty()) {
        return;
    }
    for (auto& env : environments.envs) {
        env.ReleaseSnapshot();
    }
    serialization_thread.QueueWork([this, key, envs = std::move(environments.envs)] {
        boost::container::static_vector<const GenericEnvironment*, Maxwell::MaxShaderProgram> env_ptrs;
        for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index]) {
                env_ptrs.push_back(&envs[index]);
            }
        }
        VideoCommon::ShaderProfile::Runtime::Span cache_write(
            runtime_report && runtime_report->active ? runtime_report->Begin(key.Hash(),false,quarantined_graphics.Contains(key)) : nullptr,
            VideoCommon::ShaderProfile::Runtime::Work::DiskCache);
        SerializePipeline(key, env_ptrs, pipeline_cache_filename, CACHE_VERSION, cache_context);
    });
}

void PipelineCache::FinishGraphicsPipeline(const GraphicsPipelineCacheKey& key) {
    const auto it = pending_graphics.find(key);
    if (it == pending_graphics.end() || !it->second->build.IsComplete()) {
        return;
    }
    auto job = it->second;
    // The request owns its callback, which captures this job. Break that cycle
    // once completion makes the callback's lifetime independent of the entry.
    job->request.reset();
    try {
        std::shared_ptr<GraphicsPipeline> pipeline = job->build.Take();
        for (size_t index = 0; index < key.unique_hashes.size(); ++index) {
            cbuf_size_dependencies.Observe(key.unique_hashes[index],
                                          job->environments.envs[index].CbufSizeMask());
        }
        // A failed pipeline stays cached as null, like a synchronous failure, so later draws skip
        // it instead of translating and compiling it again on every draw.
        graphics_cache[key] = pipeline;
        if (pipeline) {
            GraphicsPipelineCacheKey normalized_key{key};
            NormalizeCbufSizes(normalized_key);
            if (PublishNormalizedPipelineAlias(key, normalized_key, pipeline,
                                               graphics_cache, hle_unreplaced_keys)) {
                pipeline->SetSharedKey(normalized_key);
            }
            SerializeGraphicsEnvironments(key, job->environments);
        }
    } catch (const std::exception& exception) {
        LOG_ERROR(Render_Vulkan, "Asynchronous graphics pipeline {:016x} failed: {}", key.Hash(), exception.what());
        graphics_cache[key] = nullptr;
    } catch (...) {
        LOG_ERROR(Render_Vulkan, "Asynchronous graphics pipeline {:016x} failed", key.Hash());
        graphics_cache[key] = nullptr;
    }
    pending_graphics.erase(it);
    if (const auto unknown = pending_unknown_sizes.find(WithoutSizes(key));
        unknown != pending_unknown_sizes.end() && unknown->second == job) {
        pending_unknown_sizes.erase(unknown);
    }
}

bool PipelineCache::WithdrawGraphicsPipeline(const std::shared_ptr<PendingGraphicsPipeline>& job) {
    // Waiting for an asynchronous build compiles the pipelines of consecutive draws one after the
    // other. A build no compiler has started is withdrawn, so the draw that cannot be skipped
    // builds its pipeline like a synchronous draw: queued first, while this thread moves on.
    if (!runtime_builds.Cancel(job->request)) {
        return false;
    }
    job->request.reset();
    pending_graphics.erase(job->key);
    // The draw that queued the build left an empty entry; the build that replaces it fills it.
    if (const auto it = graphics_cache.find(job->key); it != graphics_cache.end() && !it->second) {
        graphics_cache.erase(it);
    }
    if (const auto it = pending_unknown_sizes.find(WithoutSizes(job->key));
        it != pending_unknown_sizes.end() && it->second == job) {
        pending_unknown_sizes.erase(it);
    }
    return true;
}

void PipelineCache::FinalizeReadyGraphicsPipelines() {
    if (!has_completed_graphics.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<GraphicsPipelineCacheKey> ready;
    {
        std::scoped_lock lock{completed_mutex};
        ready.swap(completed_graphics);
        has_completed_graphics.store(false, std::memory_order_release);
    }
    for (const auto& key : ready) {
        FinishGraphicsPipeline(key);
    }
}

void PipelineCache::AssociateForeignRecord(std::string record_id) {
    if(capture_context.empty() || pipeline_cache_filename.empty())return;
    try {
        serialization_thread.QueueWork([record_id=std::move(record_id),
                                        path=pipeline_cache_filename,context=capture_context] {
            try {VideoCommon::CacheStore::AssociateContext(path,CACHE_VERSION,record_id,context);}
            catch(const std::exception& error){LOG_WARNING(Render_Vulkan,"Cannot persist validated update recipe: {}",error.what());}
        });
    } catch(const std::exception& error){LOG_WARNING(Render_Vulkan,"Cannot queue validated update recipe: {}",error.what());}
}

ComputePipelineCacheKey PipelineCache::WithoutSizes(ComputePipelineCacheKey key) {
    key.cbuf_sizes.fill(0);return key;
}

bool PipelineCache::TryReuseGraphics() {
    if(foreign_graphics.empty())return false;
    const auto found=foreign_graphics.find(WithoutSizes(graphics_key));
    if(found==foreign_graphics.end())return false;
    GraphicsEnvironments live;
    GetGraphicsEnvironments(live,graphics_key.unique_hashes);
    for(const auto candidate:found->second) {
        bool equivalent=true;size_t index{};
        try {
            for(size_t slot=0;slot<graphics_key.unique_hashes.size();++slot)if(graphics_key.unique_hashes[slot]) {
                const auto& source=candidate->environments.at(index++);
                auto& target=live.envs[slot];
                target.SetQueryRecording(false);
                if(!source.HasReusableProgramLayout() || !program_equivalence.MatchDigest(source.ProgramDigest(),source.ProgramBytes(),target.CachedProgram()) ||
                   !source.ReuseProof().Matches(target,source.StartAddress()!=target.StartAddress() &&
                       source.ReusableAt(target.StartAddress()))) {equivalent=false;break;}
            }
            equivalent=equivalent && index==candidate->environments.size();
        } catch(const std::exception&) {equivalent=false;}
        if(!equivalent){++cross_version_rejected;continue;}
        const auto original=graphics_key;index=0;
        for(auto hash:graphics_key.unique_hashes)if(hash)
            cbuf_size_dependencies.Observe(hash,candidate->environments[index++].CbufSizeMask());
        NormalizeCbufSizes(graphics_key);
        candidate->pipeline->SetSharedKey(graphics_key);
        graphics_cache[graphics_key]=candidate->pipeline;
        graphics_cache[original]=candidate->pipeline;
        AssociateForeignRecord(candidate->record_id);
        ++cross_version_accepted;
        foreign_graphics.erase(found);
        return true;
    }
    // The ordinary compiler records the actual target dependencies and content
    // association. Failed candidates never enter the active pipeline cache.
    foreign_graphics.erase(found);
    return false;
}

ComputePipeline* PipelineCache::TryReuseCompute(ComputePipelineCacheKey& key,const ShaderInfo* shader) {
    if(foreign_compute.empty())return nullptr;
    const auto found=foreign_compute.find(WithoutSizes(key));
    if(found==foreign_compute.end())return nullptr;
    const auto& qmd=kepler_compute->launch_description;
    ComputeEnvironment live{*kepler_compute,*gpu_memory,kepler_compute->regs.code_loc.Address(),qmd.program_start};
    live.SetCachedCode(shader->code,shader->size_bytes);
    live.SetQueryRecording(false);
    for(const auto candidate:found->second) {
        bool equivalent{};
        try {
            equivalent=candidate->environment.HasReusableProgramLayout() && program_equivalence.MatchDigest(candidate->environment.ProgramDigest(),candidate->environment.ProgramBytes(),live.CachedProgram()) &&
                candidate->environment.ReuseProof().Matches(live,candidate->environment.StartAddress()!=live.StartAddress() &&
                    candidate->environment.ReusableAt(live.StartAddress()));
        }catch(const std::exception&){equivalent=false;}
        if(!equivalent){++cross_version_rejected;continue;}
        cbuf_size_dependencies.Observe(key.unique_hash,candidate->environment.CbufSizeMask());
        CbufSizeDependencies::Normalize(key.cbuf_sizes,cbuf_size_dependencies.Mask(key.unique_hash));
        auto* result=candidate->pipeline.get();
        compute_cache.emplace(key,std::move(candidate->pipeline));
        AssociateForeignRecord(candidate->record_id);
        ++cross_version_accepted;foreign_compute.erase(found);return result;
    }
    foreign_compute.erase(found);return nullptr;
}

GraphicsPipeline* PipelineCache::CurrentGraphicsPipelineSlowPath() {
    if (const auto it = pending_graphics.find(graphics_key); it != pending_graphics.end()) {
        auto job = it->second;
        bool withdrawn{};
        if (!job->build.IsComplete() && ShouldWaitForAsyncPipeline()) {
            withdrawn = WithdrawGraphicsPipeline(job);
            if (!withdrawn) {
                VideoCommon::ShaderProfile::Runtime::Span wait_timer(
                    runtime_report ? runtime_report->Begin(job->key.Hash(),false,quarantined_graphics.Contains(job->key)) : nullptr,
                    VideoCommon::ShaderProfile::Runtime::Work::Wait);
                job->build.Wait();
            }
        }
        if (!withdrawn) {
            if (!job->build.IsComplete()) {
                // Pipelines still drawn go before the ones queued for a draw the game no longer
                // makes.
                runtime_builds.Promote(job->request);
                skipped_unfinished = true;
                return nullptr;
            }
            FinishGraphicsPipeline(graphics_key);
        }
    }
    if (use_asynchronous_shaders && !pending_unknown_sizes.empty() &&
        !graphics_cache.contains(graphics_key)) {
        // Until the first build of these shaders is translated, every draw specializes on all
        // constant buffer sizes, so draws that differ only in unused sizes would each queue a build
        // of the same pipeline. They wait for that first build instead. So do draws whose key
        // became narrower while it builds, after another pipeline translated the same shaders.
        if (const auto it = pending_unknown_sizes.find(WithoutSizes(graphics_key));
            it != pending_unknown_sizes.end() &&
            (HasUnknownSizes(graphics_key) || Normalized(it->second->key) == graphics_key)) {
            const auto job = it->second;
            bool withdrawn{};
            if (!job->build.IsComplete() && ShouldWaitForAsyncPipeline()) {
                // This draw builds its own pipeline below, in parallel with the other draws that
                // cannot be skipped, unless a compiler is already building the first one.
                withdrawn = WithdrawGraphicsPipeline(job);
                if (!withdrawn) {
                    VideoCommon::ShaderProfile::Runtime::Span wait_timer(
                    runtime_report ? runtime_report->Begin(job->key.Hash(),false,quarantined_graphics.Contains(job->key)) : nullptr,
                    VideoCommon::ShaderProfile::Runtime::Work::Wait);
                job->build.Wait();
                }
            }
            if (!withdrawn) {
                if (!job->build.IsComplete()) {
                    runtime_builds.Promote(job->request);
                    skipped_unfinished = true;
                    return nullptr;
                }
                FinishGraphicsPipeline(job->key);
                NormalizeCbufSizes(graphics_key);
                if (const auto ready = graphics_cache.find(graphics_key);
                    ready != graphics_cache.end() && ready->second) {
                    if (current_pipeline) {
                        current_pipeline->AddTransition(ready->second.get(), graphics_key);
                    }
                    current_pipeline = ready->second.get();
                    return BuiltPipeline(current_pipeline);
                }
            }
        }
    }
    if (progressive_warmup && !graphics_cache.contains(graphics_key) &&
        progressive_warmup->DemandGraphics(graphics_key,
            use_asynchronous_shaders && !ShouldWaitForAsyncPipeline())) {
        skipped_unfinished = true;
        return nullptr;
    }

    if(runtime_report)quarantined_graphics.Requested(graphics_key);
    auto [pair, is_new]=graphics_cache.try_emplace(graphics_key);
    if(is_new && TryReuseGraphics()) {pair=graphics_cache.find(graphics_key);is_new=false;}
    auto& pipeline{pair->second};
    if (is_new) {
        // Check a proven replacement-free HLE twin before queuing a plain
        // asynchronous build. The synchronous constructor already did this,
        // but the worker path otherwise bypassed that reuse entirely.
        using EngineHint = Tegra::Engines::Maxwell3D::EngineHint;
        if (graphics_key.state.app_stage == EngineHint::None) {
            auto twin = graphics_key;
            twin.state.app_stage.Assign(EngineHint::OnHLEMacro);
            pipeline = FindUnreplacedPipelineAlias(twin, graphics_cache, hle_unreplaced_keys);
            if (pipeline) {
                pipeline->SetSharedKey(graphics_key);
            }
        }
        if (!pipeline) {
            if (use_asynchronous_shaders && !ShouldWaitForAsyncPipeline() &&
                graphics_key.state.app_stage != EngineHint::OnHLEMacro &&
                QueueGraphicsPipeline(graphics_key)) {
                skipped_unfinished = true;
                return nullptr;
            }
            // A draw that cannot be skipped waits for its pipeline, which goes
            // before every pipeline already queued.
            pipeline = CreateGraphicsPipeline(use_asynchronous_shaders && ShouldWaitForAsyncPipeline());
        }
    }
    if (!pipeline) {
        return nullptr;
    }
    if (is_new) {
        // The key of shaders seen for the first time holds every constant buffer size, since only
        // translation tells which ones they depend on. Later draws use the narrower key, which
        // must find this pipeline instead of building it again.
        GraphicsPipelineCacheKey normalized_key{graphics_key};
        NormalizeCbufSizes(normalized_key);
        if (PublishNormalizedPipelineAlias(graphics_key, normalized_key, pipeline,
                                           graphics_cache, hle_unreplaced_keys)) {
            pipeline->SetSharedKey(normalized_key);
        }
    }
    if (current_pipeline) {
        current_pipeline->AddTransition(pipeline.get(), graphics_key);
    }
    current_pipeline = pipeline.get();
    return BuiltPipeline(current_pipeline);
}

GraphicsPipelineCacheKey PipelineCache::WithoutSizes(GraphicsPipelineCacheKey key) {
    key.state.NormalizeInactiveFields();
    for (auto& sizes : key.cbuf_sizes) {
        sizes.fill(0);
    }
    key.texture_pool = {};
    return key;
}

bool PipelineCache::HasUnknownSizes(const GraphicsPipelineCacheKey& key) const {
    return std::ranges::any_of(key.unique_hashes,
                               [this](u64 hash) { return !cbuf_size_dependencies.IsKnown(hash); });
}

GraphicsPipelineCacheKey PipelineCache::Normalized(GraphicsPipelineCacheKey key) const {
    NormalizeCbufSizes(key);
    return key;
}

void PipelineCache::NormalizeCbufSizes(GraphicsPipelineCacheKey& key) const {
    // Also canonicalize keys read from caches captured before inactive-state
    // normalization; the import and live paths must use the same identity.
    key.state.NormalizeInactiveFields();
    for (size_t stage = 0; stage < key.cbuf_sizes.size(); ++stage) {
        const u32 mask = cbuf_size_dependencies.Mask(key.unique_hashes[stage + 1]) |
            (stage == 0 ? cbuf_size_dependencies.Mask(key.unique_hashes[0]) : 0);
        CbufSizeDependencies::Normalize(key.cbuf_sizes[stage], mask);
    }
    if (std::ranges::none_of(key.unique_hashes, [this](u64 hash) {
            return (cbuf_size_dependencies.Mask(hash) & VideoCommon::TEXTURE_POOL_SIZE_BIT) != 0;
        })) {
        key.texture_pool = {};
    }
}

GraphicsPipeline* PipelineCache::BuiltPipeline(GraphicsPipeline* pipeline) noexcept {
    if (pipeline->IsFailed()) {
        return nullptr;
    }
    if (pipeline->IsBuilt()) {
        return pipeline;
    }
    if (!use_asynchronous_shaders || ShouldWaitForAsyncPipeline()) {
        return pipeline;
    }
    skipped_unfinished = true;
    return nullptr;
}

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline(
    ShaderPools& pools, const GraphicsPipelineCacheKey& key,
    std::span<Shader::Environment* const> envs, PipelineStatistics* statistics,
    bool build_in_parallel, const std::function<bool()>* skip_build) try {
    namespace Runtime = VideoCommon::ShaderProfile::Runtime;
    Runtime::Context runtime_context(runtime_report && runtime_report->active ? runtime_report->Begin(key.Hash(),false,quarantined_graphics.Contains(key)) : nullptr);
    Runtime::Span runtime_timer(Runtime::current,Runtime::Work::Compiler);
    namespace Timing = VideoCommon::ShaderProfile::Timing;
    Timing::Timer compiler_timer(Timing::current, Timing::Phase::CompilerWall);
    auto hash = key.Hash();
    LOG_INFO(Render_Vulkan, "0x{:016x}", hash);
    size_t env_index{0};
    std::array<Shader::IR::Program, Maxwell::MaxShaderProgram> programs;
    const bool uses_vertex_a{key.unique_hashes[0] != 0};
    const bool uses_vertex_b{key.unique_hashes[1] != 0};

    // Layer passthrough generation for devices without VK_EXT_shader_viewport_index_layer
    Shader::IR::Program* layer_source_program{};
    Shader::Environment* vertex_env{};

    Timing::Timer translation_timer(Timing::current, Timing::Phase::GuestTranslation);
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        if (key.unique_hashes[index] == 0 && is_emulated_stage) {
            auto topology = MaxwellToOutputTopology(key.state.topology);
            programs[index] = GenerateGeometryPassthrough(pools.inst, pools.block, host_info,
                                                          *layer_source_program, topology);
            continue;
        }
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        Shader::Environment& env{*envs[env_index]};
        ++env_index;
        if (index == 1) {
            vertex_env = &env;
        }

        const u32 cfg_offset{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
        Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
        if (!uses_vertex_a || index != 1) {
            // Normal path
            programs[index] = TranslateProgram(pools.inst, pools.block, env, cfg, host_info);
        } else {
            // VertexB path when VertexA is present.
            auto& program_va{programs[0]};
            auto program_vb{TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};
            programs[index] = MergeDualVertexPrograms(program_va, program_vb, env);
        }

        if (Settings::values.dump_shaders) {
            env.Dump(hash, key.unique_hashes[index]);
        }

        if (programs[index].info.requires_layer_emulation) {
            layer_source_program = &programs[index];
        }
    }
    translation_timer.Stop();
    if (skip_build && (*skip_build)()) {
        return nullptr;
    }
    // Without host geometry shaders, a geometry shader that only selects the layer of the
    // triangle it forwards is folded into the vertex shader
    std::optional<Shader::IR::Attribute> forwarded_layer;
    const size_t geometry_index{static_cast<size_t>(Maxwell::ShaderType::Geometry)};
    if (!device.IsGeometryShaderSupported() && device.IsExtShaderViewportIndexLayerSupported() &&
        key.unique_hashes[geometry_index] != 0 && key.unique_hashes[2] == 0 &&
        key.unique_hashes[3] == 0 && uses_vertex_b) {
        const u32 input_vertices{Shader::InputTopologyVertices::vertices(
            MakeRuntimeInfo(programs, key, programs[geometry_index], nullptr).input_topology)};
        forwarded_layer = Shader::Optimization::FindForwardedLayerAttribute(
            programs[geometry_index], input_vertices);
        if (forwarded_layer) {
            programs[1].info.stores.Set(Shader::IR::Attribute::Layer);
        }
    }
    std::array<const Shader::Info*, Maxwell::MaxShaderStage> infos{};
    std::array<vk::ShaderModule, Maxwell::MaxShaderStage> modules;

    const Shader::IR::Program* previous_stage{};
    Shader::Backend::Bindings binding;

    // Without host geometry shaders, the vertex and geometry shaders run as compute shaders
    // before the draw and a generated vertex shader feeds their output to the rasterizer
    std::unique_ptr<VtgPipelineInfo> vtg_info;
    Shader::IR::Program vtg_passthrough;
    const std::optional<Shader::VtgTopology> vtg_topology{
        MaxwellToVtgTopology(key.state.topology)};
    if (!device.IsGeometryShaderSupported() && !forwarded_layer && vtg_topology &&
        key.unique_hashes[geometry_index] != 0 && key.unique_hashes[2] == 0 &&
        key.unique_hashes[3] == 0 && uses_vertex_b && key.state.dynamic_vertex_input == 0 &&
        key.state.xfb_enabled == 0) {
        Shader::IR::Program& vertex{programs[1]};
        Shader::IR::Program& geometry{programs[geometry_index]};
        const auto vertex_runtime{MakeRuntimeInfo(programs, key, vertex, nullptr)};
        ConvertLegacyToGeneric(vertex, vertex_runtime);
        const auto geometry_runtime{MakeRuntimeInfo(programs, key, geometry, &vertex)};
        ConvertLegacyToGeneric(geometry, geometry_runtime);

        Shader::VaryingState geometry_stores{geometry.info.stores};
        if (!device.IsExtShaderViewportIndexLayerSupported()) {
            geometry_stores.Set(Shader::IR::Attribute::Layer, false);
            geometry_stores.Set(Shader::IR::Attribute::ViewportIndex, false);
        }
        const Shader::VtgVaryingLayout vertex_layout{vertex.info.stores};
        const Shader::VtgVaryingLayout geometry_layout{geometry_stores};
        const u32 input_vertices{
            Shader::InputTopologyVertices::vertices(geometry_runtime.input_topology)};
        const Shader::OutputTopology geometry_topology{geometry.output_topology};

        auto info{std::make_unique<VtgPipelineInfo>()};
        if (Shader::Optimization::VtgVertexToCompute(vertex, MakeVtgVertexInputs(key.state),
                                                     vertex_layout, info->vertex_bindings)) {
            if (!Shader::Optimization::VtgGeometryToCompute(geometry, *vtg_topology,
                                                            input_vertices, vertex_layout,
                                                            geometry_layout,
                                                            info->geometry_bindings)) {
                LOG_ERROR(Render_Vulkan, "Geometry shader {:016x} cannot run as compute",
                          key.unique_hashes[geometry_index]);
                return nullptr;
            }
            const std::array<Shader::IR::Program*, VtgPipelineInfo::NUM_COMPUTE_STAGES> stages{
                &vertex, &geometry};
            const std::array<const Shader::RuntimeInfo*, VtgPipelineInfo::NUM_COMPUTE_STAGES>
                runtimes{&vertex_runtime, &geometry_runtime};
            for (size_t stage = 0; stage < stages.size(); ++stage) {
                Shader::Backend::Bindings compute_binding;
                const std::vector<u32> code{
                    Timing::Measure(Timing::current, Timing::Phase::SpirvEmission,
                        [&] { return EmitSPIRV(profile, *runtimes[stage], *stages[stage], compute_binding); })};
                VideoCommon::ShaderProfile::Audit::Record(code);
            device.SaveShader(code);
                info->modules[stage] = Timing::Measure(Timing::current, Timing::Phase::ShaderModuleCreation,
                    [&] { return BuildShader(device, code); });
                info->infos[stage] = stages[stage]->info;
            }
            info->topology = *vtg_topology;
            info->vertex_stride = vertex_layout.stride;
            info->geometry_stride = geometry_layout.stride;
            info->scratch = &vtg_scratch;
            info->staging_pool = &staging_pool;

            vtg_passthrough = GenerateVtgPassthroughVertex(pools.inst, pools.block, geometry,
                                                           geometry_stores, geometry_layout);
            auto runtime_info{MakeRuntimeInfo(programs, key, vtg_passthrough, nullptr)};
            runtime_info.convert_depth_mode = key.state.ndc_minus_one_to_one != 0;
            if (geometry_topology == Shader::OutputTopology::PointList) {
                runtime_info.fixed_state_point_size = Common::BitCast<float>(key.state.point_size);
            }
            const std::vector<u32> code{Timing::Measure(Timing::current, Timing::Phase::SpirvEmission,
                [&] { return EmitSPIRV(profile, runtime_info, vtg_passthrough, binding); })};
            VideoCommon::ShaderProfile::Audit::Record(code);
            device.SaveShader(code);
            modules[0] = Timing::Measure(Timing::current, Timing::Phase::ShaderModuleCreation,
                [&] { return BuildShader(device, code); });
            infos[0] = &vtg_passthrough.info;
            previous_stage = &vtg_passthrough;
            vtg_info = std::move(info);
            LOG_INFO(Render_Vulkan, "Running vertex shader {:016x} and geometry shader {:016x} "
                                    "as compute",
                     key.unique_hashes[1], key.unique_hashes[geometry_index]);
        }
    }
    // Without host transform feedback, a copy of the vertex shader runs as compute before the draw
    // and writes what it would capture
    const bool emulates_xfb{key.state.xfb_enabled != 0 &&
                            !device.IsExtTransformFeedbackSupported()};
    const char* const xfb_limitation{[&]() -> const char* {
        if (!emulates_xfb) {
            return nullptr;
        }
        if (key.unique_hashes[geometry_index] != 0) {
            return "a geometry shader";
        }
        if (key.unique_hashes[2] != 0 || key.unique_hashes[3] != 0) {
            return "tessellation";
        }
        if (uses_vertex_a) {
            return "a vertex A shader";
        }
        if (key.state.dynamic_vertex_input != 0) {
            return "dynamic vertex input";
        }
        if (!vtg_topology) {
            return "an unsupported topology";
        }
        return nullptr;
    }()};
    if (xfb_limitation) {
        LOG_WARNING(Render_Vulkan,
                    "Transform feedback of vertex shader {:016x} is not emulated, the pipeline "
                    "uses {}",
                    key.unique_hashes[1], xfb_limitation);
    }
    if (!vtg_info && emulates_xfb && !xfb_limitation && vertex_env) {
        const u32 cfg_offset{
            static_cast<u32>(vertex_env->StartAddress() + sizeof(Shader::ProgramHeader))};
        Shader::Maxwell::Flow::CFG cfg(*vertex_env, pools.flow_block, cfg_offset, false);
        Shader::IR::Program vertex{
            TranslateProgram(pools.inst, pools.block, *vertex_env, cfg, host_info)};
        const auto vertex_runtime{MakeRuntimeInfo(programs, key, vertex, nullptr)};
        ConvertLegacyToGeneric(vertex, vertex_runtime);
        auto info{std::make_unique<VtgPipelineInfo>()};
        info->transform_feedback = MakeVtgTransformFeedback(key.state.xfb_state, *vtg_topology);
        if (Shader::Optimization::VtgVertexToTransformFeedback(vertex, MakeVtgVertexInputs(key.state),
                                                               info->transform_feedback,
                                                               info->vertex_bindings)) {
            Shader::Backend::Bindings compute_binding;
            const std::vector<u32> code{Timing::Measure(Timing::current, Timing::Phase::SpirvEmission,
                [&] { return EmitSPIRV(profile, vertex_runtime, vertex, compute_binding); })};
            VideoCommon::ShaderProfile::Audit::Record(code);
            device.SaveShader(code);
            info->modules[VtgPipelineInfo::VERTEX] = Timing::Measure(Timing::current, Timing::Phase::ShaderModuleCreation,
                [&] { return BuildShader(device, code); });
            info->infos[VtgPipelineInfo::VERTEX] = vertex.info;
            info->topology = *vtg_topology;
            info->scratch = &vtg_scratch;
            info->staging_pool = &staging_pool;
            info->is_transform_feedback = true;
            vtg_info = std::move(info);
            LOG_INFO(Render_Vulkan, "Running vertex shader {:016x} as compute for transform feedback",
                     key.unique_hashes[1]);
        } else {
            LOG_WARNING(Render_Vulkan, "Vertex shader {:016x} cannot emulate transform feedback",
                        key.unique_hashes[1]);
        }
    }
    const bool vtg_replaces_stages{vtg_info && !vtg_info->is_transform_feedback};
    const bool reads_color_attachments{device.SupportsColorAttachmentFetch() &&
                                       videoSettings.barrier_feedback_loops &&
                                       !vtg_replaces_stages};
    u32 screen_position_generics{};
    for (size_t index = uses_vertex_a && uses_vertex_b ? 1 : 0; index < Maxwell::MaxShaderProgram;
         ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        if (key.unique_hashes[index] == 0 && !is_emulated_stage) {
            continue;
        }
        if (forwarded_layer && index == geometry_index) {
            continue;
        }
        if (vtg_replaces_stages && (index == 1 || index == geometry_index)) {
            continue;
        }
        UNIMPLEMENTED_IF(index == 0);

        Shader::IR::Program& program{programs[index]};
        const size_t stage_index{index - 1};
        infos[stage_index] = &program.info;

        auto runtime_info{MakeRuntimeInfo(programs, key, program, previous_stage)};
        if (forwarded_layer && program.stage == Shader::Stage::VertexB) {
            runtime_info.forwarded_layer_attribute = forwarded_layer;
            runtime_info.convert_depth_mode = key.state.ndc_minus_one_to_one != 0;
        }
        ConvertLegacyToGeneric(program, runtime_info);
        if (program.stage == Shader::Stage::VertexB && key.state.dynamic_vertex_input == 0 &&
            (device.GetMaxVertexInputAttributes() < Maxwell::NumVertexAttributes ||
             device.GetMaxVertexInputBindings() < Maxwell::NumVertexArrays)) {
            const auto mapping = MakeVertexInputMapping(key.state, program.info.loads);
            if (mapping.num_attributes > device.GetMaxVertexInputAttributes() ||
                mapping.num_bindings > device.GetMaxVertexInputBindings()) {
                LOG_ERROR(Render_Vulkan, "Vertex input needs {} attributes and {} buffers after "
                                         "compaction, exceeding host limits {}/{}",
                          mapping.num_attributes, mapping.num_bindings,
                          device.GetMaxVertexInputAttributes(), device.GetMaxVertexInputBindings());
                return nullptr;
            }
            runtime_info.vertex_input_locations = mapping.locations;
        }
        if (large_texture_arrays_in_table) {
            // Bindless arrays seldom change between draws. The host keeps set 1 bound.
            for (auto& desc : program.info.texture_descriptors) {
                desc.in_table_set = !desc.is_direct && desc.count >= LARGE_IMAGE_ARRAY_SIZE;
            }
        }
        if (reads_color_attachments) {
            if (program.stage == Shader::Stage::Fragment) {
                FindColorAttachmentReads(program, key, screen_position_generics);
            } else {
                screen_position_generics =
                    Shader::Optimization::FindScreenPositionGenerics(program);
            }
        }
        const std::vector<u32> code{Timing::Measure(Timing::current, Timing::Phase::SpirvEmission,
            [&] { return EmitSPIRV(profile, runtime_info, program, binding); })};
        VideoCommon::ShaderProfile::Audit::Record(code);
        device.SaveShader(code);
        modules[stage_index] = Timing::Measure(Timing::current, Timing::Phase::ShaderModuleCreation,
            [&] { return BuildShader(device, code); });

        if (device.HasDebuggingToolAttached()) {
            const std::string name{fmt::format("Shader {:016x}", key.unique_hashes[index])};
            modules[stage_index].SetObjectNameEXT(name.c_str());
        }
        previous_stage = &program;
    }
    const bool has_tessellation{static_cast<bool>(modules[2])};
    const bool tessellation_isolines{
        static_cast<Maxwell::Tessellation::DomainType>(key.state.tessellation_primitive.Value()) ==
        Maxwell::Tessellation::DomainType::Isolines};
    const auto output_topology{GetRasterizationOutputTopology(
        key.state.topology, has_tessellation, tessellation_isolines,
        vtg_replaces_stages ? std::optional{vtg_info->geometry_bindings.output_topology}
                 : (modules[3] ? std::optional{programs[4].output_topology} : std::nullopt))};
    const bool rasterizes_lines{RasterizesLines(
        output_topology, FixedPipelineState::UnpackPolygonMode(key.state.polygon_mode))};
    if (auto* audit = VideoCommon::ShaderProfile::Audit::current) {
        audit->interfaces_captured = true;
        audit->has_vtg = static_cast<bool>(vtg_info);
        audit->rasterizes_lines = rasterizes_lines;
        for (size_t stage = 0; stage < infos.size(); ++stage) {
            if (infos[stage]) audit->interfaces.emplace_back(stage, *infos[stage]);
        }
    }
    NativePipelineBuildQueue* const thread_worker{build_in_parallel ? &runtime_builds : nullptr};
    return std::make_unique<GraphicsPipeline>(
        scheduler, buffer_cache, texture_cache, vulkan_pipeline_cache, &shader_notify, device,
        descriptor_pool, guest_descriptor_queue, thread_worker, statistics, render_pass_cache, key,
        std::move(modules), infos, rasterizes_lines, std::move(vtg_info),
        build_in_parallel && urgent_graphics_build);
} catch (const Shader::Exception& exception) {
    auto hash = key.Hash();
    size_t env_index{0};
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        Shader::Environment& env{*envs[env_index]};
        ++env_index;

        const u32 cfg_offset{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
        Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
        env.Dump(hash, key.unique_hashes[index]);
    }
    LOG_ERROR(Render_Vulkan, "{}", exception.what());
    return nullptr;
} catch (const vk::Exception& exception) {
    LOG_ERROR(Render_Vulkan, "Graphics pipeline {:016x} shader creation failed: {}", key.Hash(),
              exception.what());
    return nullptr;
}

std::shared_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline(bool urgent) {
    urgent_graphics_build = urgent;
    SCOPE_EXIT { urgent_graphics_build = false; };
    using EngineHint = Tegra::Engines::Maxwell3D::EngineHint;
    const bool is_hle{graphics_key.state.app_stage == EngineHint::OnHLEMacro};
    GraphicsPipelineCacheKey twin_key{graphics_key};
    twin_key.state.app_stage.Assign(is_hle ? EngineHint::None : EngineHint::OnHLEMacro);
    const auto twin_it{graphics_cache.find(twin_key)};
    const std::shared_ptr<GraphicsPipeline> twin{twin_it != graphics_cache.end() ? twin_it->second
                                                                                 : nullptr};
    if (!is_hle && twin && hle_unreplaced_keys.contains(twin_key)) {
        twin->SetSharedKey(graphics_key);
        return twin;
    }
    GraphicsEnvironments environments;
    GetGraphicsEnvironments(environments, graphics_key.unique_hashes);
    const auto has_replacements{[&environments] {
        return std::ranges::any_of(environments.envs,
                                   [](const auto& env) { return env.HasCbufReplacements(); });
    }};
    // Replacements are only known once the shaders are translated; without any, the native
    // pipeline of the non-HLE twin is reused instead of building an identical one.
    bool reuse_twin{false};
    const std::function<bool()> skip_build{[&] {
        reuse_twin = !has_replacements();
        return reuse_twin;
    }};

    main_pools.ReleaseContents();
    std::shared_ptr<GraphicsPipeline> pipeline{
        CreateGraphicsPipeline(main_pools, graphics_key, environments.Span(), nullptr, true,
                               is_hle && twin ? &skip_build : nullptr)};
    for (size_t index = 0; index < graphics_key.unique_hashes.size(); ++index) {
        cbuf_size_dependencies.Observe(graphics_key.unique_hashes[index],
                                      environments.envs[index].CbufSizeMask());
    }
    if (reuse_twin) {
        twin->SetSharedKey(graphics_key);
        pipeline = twin;
    } else if (pipeline && is_hle && !has_replacements()) {
        hle_unreplaced_keys.insert(graphics_key);
    }
    if (!pipeline || pipeline_cache_filename.empty()) {
        return pipeline;
    }
    serialization_thread.QueueWork([this, key = graphics_key, envs = std::move(environments.envs)] {
        boost::container::static_vector<const GenericEnvironment*, Maxwell::MaxShaderProgram>
            env_ptrs;
        for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] != 0) {
                env_ptrs.push_back(&envs[index]);
            }
        }
        VideoCommon::ShaderProfile::Runtime::Span cache_write(
            runtime_report && runtime_report->active ? runtime_report->Begin(key.Hash(),false,quarantined_graphics.Contains(key)) : nullptr,
            VideoCommon::ShaderProfile::Runtime::Work::DiskCache);
        SerializePipeline(key, env_ptrs, pipeline_cache_filename, CACHE_VERSION, cache_context);
    });
    return pipeline;
}

std::unique_ptr<ComputePipeline> PipelineCache::CreateComputePipeline(
    const ComputePipelineCacheKey& key, const ShaderInfo* shader) {
    const GPUVAddr program_base{kepler_compute->regs.code_loc.Address()};
    const auto& qmd{kepler_compute->launch_description};
    ComputeEnvironment env{*kepler_compute, *gpu_memory, program_base, qmd.program_start};
    env.SetCachedSize(shader->size_bytes);

    main_pools.ReleaseContents();
    auto pipeline{CreateComputePipeline(main_pools, key, env, nullptr, true)};
    cbuf_size_dependencies.Observe(key.unique_hash, env.CbufSizeMask());
    if (!pipeline || pipeline_cache_filename.empty()) {
        return pipeline;
    }
    serialization_thread.QueueWork([this, key, env_ = std::move(env)] {
        VideoCommon::ShaderProfile::Runtime::Span cache_write(
            runtime_report && runtime_report->active ? runtime_report->Begin(key.Hash(),true,quarantined_compute.Contains(key)) : nullptr,
            VideoCommon::ShaderProfile::Runtime::Work::DiskCache);
        SerializePipeline(key, std::array<const GenericEnvironment*, 1>{&env_},
                          pipeline_cache_filename, CACHE_VERSION, cache_context);
    });
    return pipeline;
}

std::unique_ptr<ComputePipeline> PipelineCache::CreateComputePipeline(
    ShaderPools& pools, const ComputePipelineCacheKey& key, Shader::Environment& env,
    PipelineStatistics* statistics, bool build_in_parallel) try {
    namespace Runtime = VideoCommon::ShaderProfile::Runtime;
    Runtime::Context runtime_context(runtime_report && runtime_report->active ? runtime_report->Begin(key.Hash(),true,quarantined_compute.Contains(key)) : nullptr);
    Runtime::Span runtime_timer(Runtime::current,Runtime::Work::Compiler);
    namespace Timing = VideoCommon::ShaderProfile::Timing;
    Timing::Timer compiler_timer(Timing::current, Timing::Phase::CompilerWall);
    auto hash = key.Hash();
    if (device.HasBrokenCompute()) {
        LOG_ERROR(Render_Vulkan, "Skipping 0x{:016x}", hash);
        return nullptr;
    }

    LOG_INFO(Render_Vulkan, "0x{:016x}", hash);

    Timing::Timer translation_timer(Timing::current, Timing::Phase::GuestTranslation);
    Shader::Maxwell::Flow::CFG cfg{env, pools.flow_block, env.StartAddress()};

    // Dump it before error.
    if (Settings::values.dump_shaders) {
        env.Dump(hash, key.unique_hash);
    }

    auto program{TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};
    translation_timer.Stop();
    // Only where the host enforces its limit
    const u32 max_shared_memory{profile.max_compute_shared_memory_size};
    if (max_shared_memory != 0 && program.shared_memory_size > max_shared_memory &&
        !profile.support_explicit_workgroup_layout) {
        const u32 requested_shared_memory{program.shared_memory_size};
        if (Shader::Optimization::TryPackSharedMemory16To12(program, max_shared_memory)) {
            LOG_INFO(Render_Vulkan,
                     "Compute shader {:#016x}: compacted shared memory from {} to {} bytes",
                     key.unique_hash, requested_shared_memory, program.shared_memory_size);
        }
    }
    const std::vector<u32> code{Timing::Measure(Timing::current, Timing::Phase::SpirvEmission,
        [&] { return EmitSPIRV(profile, program); })};
    VideoCommon::ShaderProfile::Audit::Record(code);
            device.SaveShader(code);
    vk::ShaderModule spv_module{Timing::Measure(Timing::current, Timing::Phase::ShaderModuleCreation,
        [&] { return BuildShader(device, code); })};
    if (device.HasDebuggingToolAttached()) {
        const auto name{fmt::format("Shader {:016x}", key.unique_hash)};
        spv_module.SetObjectNameEXT(name.c_str());
    }
    NativePipelineBuildQueue* const thread_worker{build_in_parallel ? &runtime_builds : nullptr};
    return std::make_unique<ComputePipeline>(device, vulkan_pipeline_cache, descriptor_pool,
                                             guest_descriptor_queue, thread_worker, statistics,
                                             &shader_notify, program.info, std::move(spv_module), staging_pool);
} catch (const Shader::Exception& exception) {
    env.Dump(key.Hash(), key.unique_hash);
    LOG_ERROR(Render_Vulkan, "Compute shader {:#016x}, TIC table size={}: {}",
              key.unique_hash, key.texture_table_size, exception.what());
    return nullptr;
}

void PipelineCache::SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                                 const vk::PipelineCache& pipeline_cache,
                                                 u32 cache_version) try {
    const bool probe = MslCacheProbeEnabled();
    const auto started = probe ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (!pipeline_cache) {
        if (probe) LOG_INFO(Render_Vulkan, "MSL-cache probe: save skipped (no VkPipelineCache)");
        return;
    }
    // Read before truncating the last disk cache. ReadData tolerates concurrent
    // growth and only returns the bytes the driver actually wrote.
    const auto cache_data = pipeline_cache.ReadData();
    if (cache_data.empty()) {
        if (probe) LOG_INFO(Render_Vulkan, "MSL-cache probe: save skipped (empty driver blob)");
        return;
    }
    const double read_ms = probe ? ProbeMilliseconds(started) : 0.0;
    std::ofstream file(filename, std::ios::binary);
    file.exceptions(std::ofstream::failbit | std::ofstream::badbit);
    if (!file.is_open()) {
        LOG_ERROR(Common_Filesystem, "Failed to open Vulkan driver pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
        return;
    }
    file.write(VULKAN_CACHE_MAGIC_NUMBER.data(), VULKAN_CACHE_MAGIC_NUMBER.size())
        .write(reinterpret_cast<const char*>(&cache_version), sizeof(cache_version));

    file.write(cache_data.data(), static_cast<std::streamsize>(cache_data.size()));
    file.close();

    if (probe) {
        LOG_INFO(Render_Vulkan,
                 "MSL-cache probe: saved driver_blob_bytes={} read_ms={:.3f} total_ms={:.3f}",
                 cache_data.size(), read_ms, ProbeMilliseconds(started));
    }

    LOG_INFO(Render_Vulkan, "Vulkan driver pipelines cached at: {}",
             Common::FS::PathToUTF8String(filename));
} catch (const std::ios_base::failure& e) {
    if (MslCacheProbeEnabled()) LOG_WARNING(Render_Vulkan, "MSL-cache probe: save=io-failure");
    LOG_ERROR(Common_Filesystem, "{}", e.what());
    if (!Common::FS::RemoveFile(filename)) {
        LOG_ERROR(Common_Filesystem, "Failed to delete Vulkan driver pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
    }
} catch (const vk::Exception& e) {
    if (MslCacheProbeEnabled()) LOG_WARNING(Render_Vulkan, "MSL-cache probe: save=driver-failure");
    LOG_ERROR(Render_Vulkan, "Failed to read Vulkan driver pipeline cache: {}", e.what());
}

vk::PipelineCache PipelineCache::LoadVulkanPipelineCache(const std::filesystem::path& filename,
                                                         u32 expected_cache_version) {
    const bool probe = MslCacheProbeEnabled();
    const auto started = probe ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto create_pipeline_cache = [this, probe, started](size_t data_size, const void* data) {
        const auto create_started = probe ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        VkPipelineCacheCreateInfo pipeline_cache_ci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .initialDataSize = data_size,
            .pInitialData = data};
        auto result = device.GetLogical().CreatePipelineCache(pipeline_cache_ci);
        if (probe) LOG_INFO(Render_Vulkan,
            "MSL-cache probe: created input_bytes={} create_ms={:.3f} total_ms={:.3f}",
            data_size, ProbeMilliseconds(create_started), ProbeMilliseconds(started));
        return result;
    };
    try {
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            if (probe) {
                std::error_code error;
                const bool exists = std::filesystem::exists(filename, error);
                LOG_INFO(Render_Vulkan, "MSL-cache probe: load={}, creating empty cache",
                         error ? "stat-failure" : exists ? "open-failure" : "missing");
            }
            return create_pipeline_cache(0, nullptr);
        }
        file.exceptions(std::ifstream::failbit);
        const auto end{file.tellg()};
        file.seekg(0, std::ios::beg);

        std::array<char, 8> magic_number;
        u32 cache_version;
        file.read(magic_number.data(), magic_number.size())
            .read(reinterpret_cast<char*>(&cache_version), sizeof(cache_version));
        if (magic_number != VULKAN_CACHE_MAGIC_NUMBER || cache_version != expected_cache_version) {
            if (probe) {
                LOG_INFO(Render_Vulkan, "MSL-cache probe: load=invalid-wrapper "
                                        "version={} expected={}",
                         cache_version, expected_cache_version);
            }
            file.close();
            if (Common::FS::RemoveFile(filename)) {
                if (magic_number != VULKAN_CACHE_MAGIC_NUMBER) {
                    LOG_ERROR(Common_Filesystem, "Invalid Vulkan driver pipeline cache file");
                }
                if (cache_version != expected_cache_version) {
                    LOG_INFO(Common_Filesystem, "Deleting old Vulkan driver pipeline cache");
                }
            } else {
                LOG_ERROR(Common_Filesystem,
                          "Invalid Vulkan pipeline cache file and failed to delete it in \"{}\"",
                          Common::FS::PathToUTF8String(filename));
            }
            return create_pipeline_cache(0, nullptr);
        }

        static constexpr size_t header_size = magic_number.size() + sizeof(cache_version);
        const size_t cache_size = static_cast<size_t>(end) - header_size;
        std::vector<char> cache_data(cache_size);
        file.read(cache_data.data(), cache_size);

        if (probe) {
            LOG_INFO(Render_Vulkan,
                     "MSL-cache probe: load=wrapper-valid driver_header={} "
                     "driver_blob_bytes={} read_ms={:.3f}",
                     DriverCacheProbe::Header(cache_data, device.GetPhysical().GetProperties()),
                     cache_data.size(), ProbeMilliseconds(started));
        }

        LOG_INFO(Render_Vulkan,
                 "Loaded Vulkan driver pipeline cache: {}", Common::FS::PathToUTF8String(filename));

        return create_pipeline_cache(cache_size, cache_data.data());
    } catch (const std::ios_base::failure& e) {
        if (probe) LOG_WARNING(Render_Vulkan, "MSL-cache probe: load=io-failure");
        LOG_ERROR(Common_Filesystem, "{}", e.what());
        if (!Common::FS::RemoveFile(filename)) {
            LOG_ERROR(Common_Filesystem, "Failed to delete Vulkan driver pipeline cache file {}",
                      Common::FS::PathToUTF8String(filename));
        }

        return create_pipeline_cache(0, nullptr);
    }
}
} // namespace Vulkan
