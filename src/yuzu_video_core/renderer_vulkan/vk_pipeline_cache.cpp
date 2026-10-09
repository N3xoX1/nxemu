// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <memory>
#include <thread>
#include <unordered_set>
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
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/memory_manager.h"
#include "yuzu_video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "yuzu_video_core/renderer_vulkan/maxwell_to_vk.h"
#include "yuzu_video_core/renderer_vulkan/pipeline_helper.h"
#include "yuzu_video_core/renderer_vulkan/pipeline_statistics.h"
#include "yuzu_video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "yuzu_video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "yuzu_video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_shader_util.h"
#include "yuzu_video_core/renderer_vulkan/vk_update_descriptor.h"
#include "yuzu_video_core/shader_cache.h"
#include "yuzu_video_core/shader_environment.h"
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

constexpr u32 CACHE_VERSION = 14;
constexpr std::array<char, 8> VULKAN_CACHE_MAGIC_NUMBER{'y', 'u', 'z', 'u', 'v', 'k', 'c', 'h'};

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
      serialization_thread(1, "VkPipelineSerialization") {
    const auto& float_control{device.FloatControlProperties()};
    const VkDriverId driver_id{device.GetDriverID()};
    profile = Shader::Profile{
        .supported_spirv = device.SupportedSpirvVersion(),
        .unified_descriptor_binding = true,
        .support_descriptor_aliasing = device.IsDescriptorAliasingSupported(),
        .support_sampled_image_array_nonuniform_indexing =
            device.IsSampledImageArrayNonUniformIndexingSupported(),
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
    };

    if (device.GetMaxVertexInputAttributes() < Maxwell::NumVertexAttributes) {
        LOG_WARNING(Render_Vulkan, "maxVertexInputAttributes is too low: {} < {}",
                    device.GetMaxVertexInputAttributes(), Maxwell::NumVertexAttributes);
    }
    if (device.GetMaxVertexInputBindings() < Maxwell::NumVertexArrays) {
        LOG_WARNING(Render_Vulkan, "maxVertexInputBindings is too low: {} < {}",
                    device.GetMaxVertexInputBindings(), Maxwell::NumVertexArrays);
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
}

PipelineCache::~PipelineCache() {
    if (use_vulkan_pipeline_cache && !vulkan_pipeline_cache_filename.empty()) {
        SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                     CACHE_VERSION);
    }
}

GraphicsPipeline* PipelineCache::CurrentGraphicsPipeline() {

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

    if (current_pipeline) {
        GraphicsPipeline* const next{current_pipeline->Next(graphics_key)};
        if (next) {
            current_pipeline = next;
            return BuiltPipeline(current_pipeline);
        }
    }
    return CurrentGraphicsPipelineSlowPath();
}

ComputePipeline* PipelineCache::CurrentComputePipeline() {
    const ShaderInfo* const shader{ComputeShader()};
    if (!shader) {
        return nullptr;
    }
    const auto& qmd{kepler_compute->launch_description};
    ComputePipelineCacheKey key{
        .unique_hash = shader->unique_hash,
        .shared_memory_size = qmd.shared_alloc,
        .workgroup_size{qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z},
    };
    const u32 size_mask = qmd.const_buffer_enable_mask.Value() &
                          cbuf_size_dependencies.Mask(shader->unique_hash);
    for (size_t bank = 0; bank < key.cbuf_sizes.size(); ++bank) {
        key.cbuf_sizes[bank] = ((size_mask >> bank) & 1) != 0
                                  ? qmd.const_buffer_config[bank].size.Value() : 0;
    }
    const auto [pair, is_new]{compute_cache.try_emplace(key)};
    auto& pipeline{pair->second};
    if (!is_new) {
        return pipeline.get();
    }
    pipeline = CreateComputePipeline(key, shader);
    return pipeline.get();
}

void PipelineCache::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                      const VideoCore::DiskResourceLoadCallback& callback) {
    if (title_id == 0) {
        return;
    }
    const auto shader_dir{Common::FS::GetYuzuPath(Common::FS::YuzuPath::ShaderDir)};
    const auto base_dir{shader_dir / fmt::format("{:016x}", title_id)};
    if (!Common::FS::CreateDir(shader_dir) || !Common::FS::CreateDir(base_dir)) {
        LOG_ERROR(Common_Filesystem, "Failed to create pipeline cache directories");
        return;
    }
    pipeline_cache_filename = base_dir / "vulkan.bin";

    if (use_vulkan_pipeline_cache) {
        vulkan_pipeline_cache_filename = base_dir / "vulkan_pipelines.bin";
        vulkan_pipeline_cache =
            LoadVulkanPipelineCache(vulkan_pipeline_cache_filename, CACHE_VERSION);
    }

    struct {
        std::mutex mutex;
        size_t total{};
        size_t built{};
        bool has_loaded{};
        std::unique_ptr<PipelineStatistics> statistics;
    } state;
    // Cache parsing/allocation can throw after work capturing this stack state is queued.
    // Drain active work before state and the progress callback can leave scope.
    SCOPE_EXIT { workers.WaitForRequests(); };

    if (device.IsKhrPipelineExecutablePropertiesEnabled()) {
        state.statistics = std::make_unique<PipelineStatistics>(device);
    }

    // Earlier records included sizes for every bound bank, even when the shader never
    // queried them. Normalize and deduplicate them without discarding the existing shader cache.
    std::unordered_set<ComputePipelineCacheKey> loaded_compute_keys;
    std::unordered_set<GraphicsPipelineCacheKey> loaded_graphics_keys;
    const auto load_compute{[&](std::ifstream& file, FileEnvironment env) {
        ComputePipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));

        cbuf_size_dependencies.Observe(key.unique_hash, env.CbufSizeMask());
        CbufSizeDependencies::Normalize(key.cbuf_sizes,
                                       cbuf_size_dependencies.Mask(key.unique_hash));
        if (!loaded_compute_keys.insert(key).second) {
            return;
        }

        workers.QueueWork([this, key, env_ = std::move(env), &state, &callback]() mutable {
            ShaderPools pools;
            auto pipeline{CreateComputePipeline(pools, key, env_, state.statistics.get(), false)};
            std::scoped_lock lock{state.mutex};
            if (pipeline) {
                compute_cache.emplace(key, std::move(pipeline));
            }
            ++state.built;
            if (state.has_loaded) {
                callback(VideoCore::LoadCallbackStage::Build, state.built, state.total);
            }
        });
        ++state.total;
    }};
    const auto queue_graphics{[&](const GraphicsPipelineCacheKey& key,
                                  std::vector<FileEnvironment> envs) {
        workers.QueueWork([this, key, envs_ = std::move(envs), &state, &callback]() mutable {
            ShaderPools pools;
            boost::container::static_vector<Shader::Environment*, Maxwell::MaxShaderProgram> env_ptrs;
            for (auto& env : envs_) {
                env_ptrs.push_back(&env);
            }
            auto pipeline{CreateGraphicsPipeline(pools, key, MakeSpan(env_ptrs),
                                                 state.statistics.get(), false)};

            std::scoped_lock lock{state.mutex};
            if (pipeline) {
                graphics_cache.emplace(key, std::move(pipeline));
            }
            ++state.built;
            if (state.has_loaded) {
                callback(VideoCore::LoadCallbackStage::Build, state.built, state.total);
            }
        });
        ++state.total;
    }};
    // HLE macro pipelines without replacements wait until every key is known: those with a cached
    // non-HLE twin share its pipeline instead of building an identical one.
    struct DeferredPipeline {
        GraphicsPipelineCacheKey key;
        std::vector<FileEnvironment> envs;
    };
    std::vector<DeferredPipeline> hle_unreplaced;
    std::unordered_set<GraphicsPipelineCacheKey> plain_keys;
    const auto load_graphics{[&](std::ifstream& file, std::vector<FileEnvironment> envs) {
        GraphicsPipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));

        size_t env_index{};
        for (u64 hash : key.unique_hashes) {
            if (hash != 0) {
                if (env_index >= envs.size()) {
                    LOG_ERROR(Render_Vulkan, "Cached graphics shader environment count mismatch");
                    return;
                }
                cbuf_size_dependencies.Observe(hash, envs[env_index++].CbufSizeMask());
            }
        }
        if (env_index != envs.size()) {
            LOG_ERROR(Render_Vulkan, "Cached graphics shader environment count mismatch");
            return;
        }
        for (size_t stage = 0; stage < key.cbuf_sizes.size(); ++stage) {
            const u32 mask = cbuf_size_dependencies.Mask(key.unique_hashes[stage + 1]) |
                (stage == 0 ? cbuf_size_dependencies.Mask(key.unique_hashes[0]) : 0);
            CbufSizeDependencies::Normalize(key.cbuf_sizes[stage], mask);
        }
        if (!loaded_graphics_keys.insert(key).second) {
            return;
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
            return;
        }
        using EngineHint = Tegra::Engines::Maxwell3D::EngineHint;
        if (key.state.app_stage != EngineHint::OnHLEMacro) {
            plain_keys.insert(key);
        } else if (std::ranges::none_of(envs,
                                        [](const auto& env) { return env.HasHLEMacroState(); })) {
            hle_unreplaced_keys.insert(key);
            hle_unreplaced.push_back({key, std::move(envs)});
            return;
        }
        queue_graphics(key, std::move(envs));
    }};
    VideoCommon::LoadPipelines(stop_loading, pipeline_cache_filename, CACHE_VERSION, load_compute,
                               load_graphics);

    const auto non_hle_twin{[](GraphicsPipelineCacheKey key) {
        key.state.app_stage.Assign(Tegra::Engines::Maxwell3D::EngineHint::None);
        return key;
    }};
    std::vector<GraphicsPipelineCacheKey> hle_shared_keys;
    for (auto& [key, envs] : hle_unreplaced) {
        if (plain_keys.contains(non_hle_twin(key))) {
            hle_shared_keys.push_back(key);
        } else {
            queue_graphics(key, std::move(envs));
        }
    }

    LOG_INFO(Render_Vulkan, "Total Pipeline Count: {}", state.total);

    std::unique_lock lock{state.mutex};
    callback(VideoCore::LoadCallbackStage::Build, 0, state.total);
    state.has_loaded = true;
    lock.unlock();

    workers.WaitForRequests(stop_loading);

    lock.lock();
    for (const auto& key : hle_shared_keys) {
        const auto it{graphics_cache.find(non_hle_twin(key))};
        if (it != graphics_cache.end() && it->second) {
            const std::shared_ptr<GraphicsPipeline> twin{it->second};
            twin->SetSharedKey(key);
            graphics_cache.emplace(key, twin);
        }
    }
    lock.unlock();

    if (use_vulkan_pipeline_cache) {
        SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                     CACHE_VERSION);
    }

    if (state.statistics) {
        state.statistics->Report();
    }
}

GraphicsPipeline* PipelineCache::CurrentGraphicsPipelineSlowPath() {
    const auto [pair, is_new]{graphics_cache.try_emplace(graphics_key)};
    auto& pipeline{pair->second};
    if (is_new) {
        pipeline = CreateGraphicsPipeline();
    }
    if (!pipeline) {
        return nullptr;
    }
    if (current_pipeline) {
        current_pipeline->AddTransition(pipeline.get(), graphics_key);
    }
    current_pipeline = pipeline.get();
    return BuiltPipeline(current_pipeline);
}

GraphicsPipeline* PipelineCache::BuiltPipeline(GraphicsPipeline* pipeline) const noexcept {
    if (pipeline->IsFailed()) {
        return nullptr;
    }
    if (pipeline->IsBuilt()) {
        return pipeline;
    }
    if (!use_asynchronous_shaders) {
        return pipeline;
    }
    // If something is using depth, we can assume that games are not rendering anything which
    // will be used one time.
    if (maxwell3d->regs.zeta_enable) {
        return nullptr;
    }
    // If games are using a small index count, we can assume these are full screen quads.
    // Usually these shaders are only used once for building textures so we can assume they
    // can't be built async
    const auto& draw_state = maxwell3d->draw_manager->GetDrawState();
    if (draw_state.index_buffer.count <= 6 || draw_state.vertex_buffer.count <= 6) {
        return pipeline;
    }
    return nullptr;
}

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline(
    ShaderPools& pools, const GraphicsPipelineCacheKey& key,
    std::span<Shader::Environment* const> envs, PipelineStatistics* statistics,
    bool build_in_parallel, const std::function<bool()>* skip_build) try {
    auto hash = key.Hash();
    LOG_INFO(Render_Vulkan, "0x{:016x}", hash);
    size_t env_index{0};
    std::array<Shader::IR::Program, Maxwell::MaxShaderProgram> programs;
    const bool uses_vertex_a{key.unique_hashes[0] != 0};
    const bool uses_vertex_b{key.unique_hashes[1] != 0};

    // Layer passthrough generation for devices without VK_EXT_shader_viewport_index_layer
    Shader::IR::Program* layer_source_program{};

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
                    EmitSPIRV(profile, *runtimes[stage], *stages[stage], compute_binding)};
                device.SaveShader(code);
                info->modules[stage] = BuildShader(device, code);
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
            const std::vector<u32> code{EmitSPIRV(profile, runtime_info, vtg_passthrough, binding)};
            device.SaveShader(code);
            modules[0] = BuildShader(device, code);
            infos[0] = &vtg_passthrough.info;
            previous_stage = &vtg_passthrough;
            vtg_info = std::move(info);
            LOG_INFO(Render_Vulkan, "Running vertex shader {:016x} and geometry shader {:016x} "
                                    "as compute",
                     key.unique_hashes[1], key.unique_hashes[geometry_index]);
        }
    }
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
        if (vtg_info && (index == 1 || index == geometry_index)) {
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
        const std::vector<u32> code{EmitSPIRV(profile, runtime_info, program, binding)};
        device.SaveShader(code);
        modules[stage_index] = BuildShader(device, code);
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
        vtg_info ? std::optional{vtg_info->geometry_bindings.output_topology}
                 : (modules[3] ? std::optional{programs[4].output_topology} : std::nullopt))};
    const bool rasterizes_lines{RasterizesLines(
        output_topology, FixedPipelineState::UnpackPolygonMode(key.state.polygon_mode))};
    Common::ThreadWorker* const thread_worker{build_in_parallel ? &workers : nullptr};
    return std::make_unique<GraphicsPipeline>(
        scheduler, buffer_cache, texture_cache, vulkan_pipeline_cache, &shader_notify, device,
        descriptor_pool, guest_descriptor_queue, thread_worker, statistics, render_pass_cache, key,
        std::move(modules), infos, rasterizes_lines, std::move(vtg_info));

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

std::shared_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline() {
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
        SerializePipeline(key, env_ptrs, pipeline_cache_filename, CACHE_VERSION);
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
        SerializePipeline(key, std::array<const GenericEnvironment*, 1>{&env_},
                          pipeline_cache_filename, CACHE_VERSION);
    });
    return pipeline;
}

std::unique_ptr<ComputePipeline> PipelineCache::CreateComputePipeline(
    ShaderPools& pools, const ComputePipelineCacheKey& key, Shader::Environment& env,
    PipelineStatistics* statistics, bool build_in_parallel) try {
    auto hash = key.Hash();
    if (device.HasBrokenCompute()) {
        LOG_ERROR(Render_Vulkan, "Skipping 0x{:016x}", hash);
        return nullptr;
    }

    LOG_INFO(Render_Vulkan, "0x{:016x}", hash);

    Shader::Maxwell::Flow::CFG cfg{env, pools.flow_block, env.StartAddress()};

    // Dump it before error.
    if (Settings::values.dump_shaders) {
        env.Dump(hash, key.unique_hash);
    }

    auto program{TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};
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
    const std::vector<u32> code{EmitSPIRV(profile, program)};
    device.SaveShader(code);
    vk::ShaderModule spv_module{BuildShader(device, code)};
    if (device.HasDebuggingToolAttached()) {
        const auto name{fmt::format("Shader {:016x}", key.unique_hash)};
        spv_module.SetObjectNameEXT(name.c_str());
    }
    Common::ThreadWorker* const thread_worker{build_in_parallel ? &workers : nullptr};
    return std::make_unique<ComputePipeline>(device, vulkan_pipeline_cache, descriptor_pool,
                                             guest_descriptor_queue, thread_worker, statistics,
                                             &shader_notify, program.info, std::move(spv_module));

} catch (const Shader::Exception& exception) {
    LOG_ERROR(Render_Vulkan, "{}", exception.what());
    return nullptr;
}

void PipelineCache::SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                                 const vk::PipelineCache& pipeline_cache,
                                                 u32 cache_version) try {
    std::ofstream file(filename, std::ios::binary);
    file.exceptions(std::ifstream::failbit);
    if (!file.is_open()) {
        LOG_ERROR(Common_Filesystem, "Failed to open Vulkan driver pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
        return;
    }
    file.write(VULKAN_CACHE_MAGIC_NUMBER.data(), VULKAN_CACHE_MAGIC_NUMBER.size())
        .write(reinterpret_cast<const char*>(&cache_version), sizeof(cache_version));

    size_t cache_size = 0;
    std::vector<char> cache_data;
    if (pipeline_cache) {
        pipeline_cache.Read(&cache_size, nullptr);
        cache_data.resize(cache_size);
        pipeline_cache.Read(&cache_size, cache_data.data());
    }
    file.write(cache_data.data(), cache_size);

    LOG_INFO(Render_Vulkan, "Vulkan driver pipelines cached at: {}",
             Common::FS::PathToUTF8String(filename));

} catch (const std::ios_base::failure& e) {
    LOG_ERROR(Common_Filesystem, "{}", e.what());
    if (!Common::FS::RemoveFile(filename)) {
        LOG_ERROR(Common_Filesystem, "Failed to delete Vulkan driver pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
    }
}

vk::PipelineCache PipelineCache::LoadVulkanPipelineCache(const std::filesystem::path& filename,
                                                         u32 expected_cache_version) {
    const auto create_pipeline_cache = [this](size_t data_size, const void* data) {
        VkPipelineCacheCreateInfo pipeline_cache_ci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .initialDataSize = data_size,
            .pInitialData = data};
        return device.GetLogical().CreatePipelineCache(pipeline_cache_ci);
    };
    try {
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
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

        LOG_INFO(Render_Vulkan,
                 "Loaded Vulkan driver pipeline cache: ", Common::FS::PathToUTF8String(filename));

        return create_pipeline_cache(cache_size, cache_data.data());

    } catch (const std::ios_base::failure& e) {
        LOG_ERROR(Common_Filesystem, "{}", e.what());
        if (!Common::FS::RemoveFile(filename)) {
            LOG_ERROR(Common_Filesystem, "Failed to delete Vulkan driver pipeline cache file {}",
                      Common::FS::PathToUTF8String(filename));
        }

        return create_pipeline_cache(0, nullptr);
    }
}

} // namespace Vulkan
