// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <fmt/format.h>
#include <cstdlib>
#include <algorithm>
#include <ranges>
#include <array>
#include <atomic>
#include <mutex>
#include <set>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_set>

#include "yuzu_common/alignment.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/metal_common/metal_device.h"
#include "yuzu_video_core/renderer_metal/maxwell_to_mtl.h"
#include "yuzu_video_core/shader_notify.h"
#include "yuzu_video_core/renderer_metal/mtl_graphics_pipeline.h"
#include "yuzu_video_core/renderer_metal/mtl_scheduler.h"

namespace Metal {

namespace {

constexpr const char* StageName(PipelineStage stage) {
    switch (stage) {
    case PipelineStage::Vertex:
        return "vertex";
    case PipelineStage::Fragment:
        return "fragment";
    case PipelineStage::Geometry:
        return "geometry";
    default:
        return "unknown";
    }
}

/// Metal exposes 31 vertex attribute slots (0..30). Indexing past the end
/// aborts the process rather than reporting a validation error, so bound it.
constexpr size_t MAX_ATTRIBUTE_SLOTS = 31;

} // Anonymous namespace

GraphicsPipeline::GraphicsPipeline(
    const Device& device_, Scheduler& scheduler_, ShaderWorkerPool* worker_thread,
    const FixedPipelineState& state,
    std::array<const Shader::Info*, NUM_PIPELINE_STAGES> infos,
    std::array<const MslShader*, NUM_PIPELINE_STAGES> shaders,
    VideoCore::ShaderNotify* shader_notify_, std::unique_ptr<GeometryEmulation> geometry_,
    std::unique_ptr<VtgEmulation> vtg_)
    : device{device_}, scheduler{scheduler_}, shader_notify{shader_notify_},
      key{state}, geometry{std::move(geometry_)}, vtg{std::move(vtg_)} {
    if (shader_notify_ != nullptr) {
        shader_notify_->MarkShaderBuilding();
    }
    // Copied: the caller's Shader::Info and MslShader are locals that die well
    // before an async build runs.
    std::array<MslShader, NUM_PIPELINE_STAGES> owned_shaders{};
    std::array<bool, NUM_PIPELINE_STAGES> present{};
    for (size_t index = 0; index < NUM_PIPELINE_STAGES; ++index) {
        if (infos[index] != nullptr) {
            const Shader::Info& info = *infos[index];
            stage_infos[index].emplace(info);
            const size_t maxwell_stage = MAXWELL_STAGE_INDEX[index];
            enabled_uniform_buffer_masks[maxwell_stage] = info.constant_buffer_mask;
            std::ranges::copy(info.constant_buffer_used_sizes,
                              uniform_buffer_sizes[maxwell_stage].begin());
        }
        if (shaders[index] != nullptr) {
            owned_shaders[index] = *shaders[index];
            expected_argument_slots[index] = shaders[index]->argument_buffer_slots;
            expected_table_argument_slots[index] = shaders[index]->table_argument_buffer_slots;
            present[index] = true;
        }
    }

    if (geometry) {
        // The capture kernel consumes the vertex argument buffer, so it owns the
        // slot count.
        expected_argument_slots[static_cast<size_t>(PipelineStage::Vertex)] =
            geometry->capture.argument_buffer_slots;
        expected_table_argument_slots[static_cast<size_t>(PipelineStage::Vertex)] =
            geometry->capture.table_argument_buffer_slots;
        expected_argument_slots[static_cast<size_t>(PipelineStage::Geometry)] =
            geometry->geometry_argument_slots;
    } else if (vtg) {
        expected_argument_slots[static_cast<size_t>(PipelineStage::Vertex)] =
            vtg->vertex_kernel.argument_buffer_slots;
        expected_table_argument_slots[static_cast<size_t>(PipelineStage::Vertex)] =
            vtg->vertex_kernel.table_argument_buffer_slots;
        expected_argument_slots[static_cast<size_t>(PipelineStage::Geometry)] =
            vtg->geometry_kernel.argument_buffer_slots;
        expected_table_argument_slots[static_cast<size_t>(PipelineStage::Geometry)] =
            vtg->geometry_kernel.table_argument_buffer_slots;
    }

    // Precompute immutable per-pipeline masks once at creation instead of scanning
    // every stage descriptor / attachment / vertex binding on every draw call.
    for (size_t stage_index = 0; stage_index < stage_infos.size(); ++stage_index) {
        const std::optional<CompactStageInfo>& info = stage_infos[stage_index];
        if (!info) {
            continue;
        }
        if (info->stores_global_memory ||
            std::ranges::any_of(info->storage_buffers_descriptors,
                                [](const auto& desc) { return desc.is_written; }) ||
            std::ranges::any_of(info->image_descriptors,
                                [](const auto& desc) { return desc.is_written; }) ||
            std::ranges::any_of(info->image_buffer_descriptors,
                                [](const auto& desc) { return desc.is_written; })) {
            has_storage_writes = true;
            if (stage_index == static_cast<size_t>(PipelineStage::Vertex)) {
                has_vertex_storage_writes = true;
            }
        }
    }
    if (const CompactStageInfo* const vertex_info = StageInfo(PipelineStage::Vertex)) {
        vertex_reads_textures =
            !vertex_info->texture_descriptors.empty() || !vertex_info->image_descriptors.empty();
        vertex_reads_buffers = !vertex_info->texture_buffer_descriptors.empty() ||
                               !vertex_info->image_buffer_descriptors.empty() ||
                               !vertex_info->storage_buffers_descriptors.empty() ||
                               vertex_info->uses_global_memory;
    }
    const CompactStageInfo* const fragment_info = StageInfo(PipelineStage::Fragment);
    if (!key.rasterizer_discard) {
        for (u32 index = 0; index < key.num_color_attachments; ++index) {
            if (key.color_attachments[index].write_mask != MTL::ColorWriteMaskNone &&
                StoresColor(key, fragment_info, index)) {
                written_color_attachments |= 1u << index;
            }
        }
    }
    for (size_t index = 0; index < key.bindings.size(); ++index) {
        if (key.bindings[index].enabled) {
            enabled_vertex_slots |= 1u << index;
        }
    }

    if (worker_thread == nullptr) {
        Build(std::move(owned_shaders), present);
        return;
    }
    worker_thread->QueueWork(
        [this, shaders = std::move(owned_shaders), present]() mutable {
            Build(std::move(shaders), present);
        });
}

bool GraphicsPipeline::StoresColor(const FixedPipelineState& key,
                                   const CompactStageInfo* fragment_info, u32 index) {
    if (fragment_info == nullptr || index >= fragment_info->stores_frag_color.size()) {
        return false;
    }
    // Dual-source blending feeds both outputs into attachment 0.
    if (key.dual_source_blend != 0 && index == 0) {
        return fragment_info->stores_frag_color[0] || fragment_info->stores_frag_color[1];
    }
    return fragment_info->stores_frag_color[index];
}

GraphicsPipeline::~GraphicsPipeline() {
    // The worker holds a bare `this`, so it must outlive a queued build.
    WaitBuilt();
    // Ensure SignalBuilt() has exited its critical section before destroying build_mutex.
    std::scoped_lock lock{build_mutex};
}

void GraphicsPipeline::SignalBuilt() {
    // Exchanged, not tested, so a second SignalBuilt cannot double-decrement.
    if (VideoCore::ShaderNotify* const notify = shader_notify.exchange(nullptr)) {
        notify->MarkShaderComplete();
    }
    std::scoped_lock lock{build_mutex};
    is_built.store(true, std::memory_order::release);
    build_condvar.notify_all();
}

void GraphicsPipeline::WaitBuilt() const {
    if (is_built.load(std::memory_order::acquire)) {
        return;
    }
    std::unique_lock lock{build_mutex};
    build_condvar.wait(lock, [this] { return is_built.load(std::memory_order::acquire); });
}

Owned<MTL::VertexDescriptor> GraphicsPipeline::BuildVertexDescriptor() const {
    Owned<MTL::VertexDescriptor> descriptor = Adopt(MTL::VertexDescriptor::alloc()->init());

    // Smallest stride each binding must declare to span its attributes. A
    // too-small stride is not a validation error, just silently wrong bytes.
    std::array<u32, MAX_VERTEX_BUFFERS> min_strides{};

    auto* attributes = descriptor->attributes();
    const size_t max_attributes =
        std::min<size_t>(key.attributes.size(), MAX_ATTRIBUTE_SLOTS);
    for (size_t index = 0; index < max_attributes; ++index) {
        const FixedPipelineState::VertexAttribute& attribute = key.attributes[index];
        if (attribute.format == MTL::VertexFormatInvalid) {
            continue;
        }
        auto* target = attributes->object(index);
        target->setFormat(attribute.format);
        target->setOffset(attribute.offset);
        target->setBufferIndex(BindingMap::VERTEX_BUFFER_BASE + attribute.buffer_index);

        if (attribute.buffer_index < min_strides.size()) {
            u32& min_stride = min_strides[attribute.buffer_index];
            min_stride = std::max(min_stride, attribute.offset + VertexFormatSize(attribute.format));
        }
    }

    auto* layouts = descriptor->layouts();
    const size_t max_bindings =
        std::min<size_t>(key.bindings.size(),
                         BindingMap::MAX_BUFFER_SLOTS - BindingMap::VERTEX_BUFFER_BASE);
    for (size_t index = 0; index < max_bindings; ++index) {
        const FixedPipelineState::VertexBinding& binding = key.bindings[index];
        if (!binding.enabled) {
            continue;
        }
        auto* layout = layouts->object(BindingMap::VERTEX_BUFFER_BASE + index);
        const u32 required = min_strides[index];
        const u32 min_stride = Common::AlignUp(std::max(required, 4u), 4u);

        if (binding.stride == 0) {
            // Metal rejects stride 0, so express it as a constant step instead.
            layout->setStride(min_stride);
            layout->setStepFunction(MTL::VertexStepFunctionConstant);
            layout->setStepRate(0);
            continue;
        }

        if (binding.stride < required) {
            // Honouring it reads past each vertex, widening it changes spacing.
            static std::atomic<u32> reported{0};
            if (reported++ < 8) {
                LOG_WARNING(Render_Metal,
                            "Vertex stream {} stride {} is below the {} bytes its attributes read",
                            index, binding.stride, required);
            }
        }

        layout->setStride(binding.stride);
        // Only a NON-ZERO divisor selects per-instance stepping. Mapping
        // `instanced && divisor == 0` to VertexStepFunctionConstant was tried and
        // is wrong: `IsInstancingEnabled` is set for streams that step per vertex,
        // so Constant made every vertex of those meshes read element 0,
        // collapsing them to a point. Do not revisit without clearing the shader
        // cache, a stale pipeline made it look like it worked.
        if (binding.divisor == 0) {
            layout->setStepFunction(MTL::VertexStepFunctionPerVertex);
            layout->setStepRate(1);
        } else {
            layout->setStepFunction(MTL::VertexStepFunctionPerInstance);
            layout->setStepRate(binding.divisor);
        }
    }

    return descriptor;
}

Owned<MTL::StageInputOutputDescriptor> GraphicsPipeline::BuildStageInputDescriptor() const {
    Owned<MTL::StageInputOutputDescriptor> descriptor =
        Adopt(MTL::StageInputOutputDescriptor::alloc()->init());

    std::array<u32, MAX_VERTEX_BUFFERS> min_strides{};

    auto* attributes = descriptor->attributes();
    const size_t max_attributes = std::min<size_t>(key.attributes.size(), MAX_ATTRIBUTE_SLOTS);
    for (size_t index = 0; index < max_attributes; ++index) {
        const FixedPipelineState::VertexAttribute& attribute = key.attributes[index];
        if (attribute.format == MTL::VertexFormatInvalid) {
            continue;
        }
        const u32 slot = BindingMap::VERTEX_BUFFER_BASE + attribute.buffer_index;
        if (IsCaptureReservedBufferSlot(slot)) {
            // A dropped attribute then reads undefined data, not zero.
            static std::atomic<u32> reported{0};
            if (reported++ < 8) {
                LOG_WARNING(Render_Metal,
                            "Capture kernel drops attribute {}: stream slot {} is one of its own "
                            "buffers",
                            index, slot);
            }
            continue;
        }
        auto* target = attributes->object(index);
        // MTLAttributeFormat mirrors MTLVertexFormat value for value.
        target->setFormat(static_cast<MTL::AttributeFormat>(static_cast<MTL::VertexFormat>(attribute.format)));
        target->setOffset(attribute.offset);
        target->setBufferIndex(slot);

        if (attribute.buffer_index < min_strides.size()) {
            u32& min_stride = min_strides[attribute.buffer_index];
            min_stride =
                std::max(min_stride, attribute.offset + VertexFormatSize(attribute.format));
        }
    }

    auto* layouts = descriptor->layouts();
    const size_t max_bindings = std::min<size_t>(
        key.bindings.size(), BindingMap::MAX_BUFFER_SLOTS - BindingMap::VERTEX_BUFFER_BASE);
    for (size_t index = 0; index < max_bindings; ++index) {
        const FixedPipelineState::VertexBinding& binding = key.bindings[index];
        const u32 slot = BindingMap::VERTEX_BUFFER_BASE + static_cast<u32>(index);
        if (!binding.enabled || IsCaptureReservedBufferSlot(slot)) {
            continue;
        }
        auto* layout = layouts->object(slot);
        const u32 min_stride = Common::AlignUp(std::max(min_strides[index], 4u), 4u);

        if (binding.stride == 0) {
            layout->setStride(min_stride);
            layout->setStepFunction(MTL::StepFunctionConstant);
            layout->setStepRate(0);
            continue;
        }
        layout->setStride(binding.stride);
        if (binding.divisor == 0) {
            // Always indexed: the capture runs off a generated index buffer.
            layout->setStepFunction(MTL::StepFunctionThreadPositionInGridXIndexed);
            layout->setStepRate(1);
        } else {
            // Instances are the dispatch's Y axis, matching the capture grid.
            layout->setStepFunction(MTL::StepFunctionThreadPositionInGridY);
            layout->setStepRate(binding.divisor);
        }
    }

    descriptor->setIndexType(MTL::IndexTypeUInt32);
    descriptor->setIndexBufferIndex(CAPTURE_INDEX_BUFFER_INDEX);
    return descriptor;
}

bool GraphicsPipeline::BuildGeometryPipelines() {
    Owned<MTL::Library> capture_library = BuildLibrary(device, geometry->capture);
    std::string{}.swap(geometry->capture.source);
    std::string{}.swap(geometry->geometry.source);
    if (!capture_library) {
        LOG_ERROR(Render_Metal, "Failed to build the vertex capture kernel library");
        return false;
    }

    // MTL4::ComputePipelineDescriptor has no stage-input descriptor, so the
    // capture kernel fetches its attributes itself. See InjectManualVertexFetch.
    capture_pipeline = BuildComputePipeline(device, capture_library.get(),
                                            geometry->capture.entry_point,
                                            "eden vertex capture kernel");
    return static_cast<bool>(capture_pipeline);
}

bool GraphicsPipeline::BuildVtgPipelines() {
    Owned<MTL::Library> vertex_library = BuildLibrary(device, vtg->vertex_kernel);
    std::string{}.swap(vtg->vertex_kernel.source);
    if (!vertex_library) {
        std::string{}.swap(vtg->geometry_kernel.source);
        LOG_ERROR(Render_Metal, "Failed to build the VTG vertex compute library");
        return false;
    }
    Owned<MTL::Library> geometry_library = BuildLibrary(device, vtg->geometry_kernel);
    std::string{}.swap(vtg->geometry_kernel.source);
    if (!geometry_library) {
        LOG_ERROR(Render_Metal, "Failed to build the VTG geometry compute library");
        return false;
    }
    vtg_vertex_pipeline = BuildComputePipeline(device, vertex_library.get(),
                                               vtg->vertex_kernel.entry_point,
                                               "nxemu vtg vertex kernel");
    if (!vtg_vertex_pipeline) {
        return false;
    }
    vtg_geometry_pipeline = BuildComputePipeline(device, geometry_library.get(),
                                                 vtg->geometry_kernel.entry_point,
                                                 "nxemu vtg geometry kernel");
    if (!vtg_geometry_pipeline) {
        return false;
    }
    return true;
}

void GraphicsPipeline::Build(std::array<MslShader, NUM_PIPELINE_STAGES> shaders,
                             std::array<bool, NUM_PIPELINE_STAGES> present) {
    ScopedAutoreleasePool pool;

    // Every exit path has to end the build, or a draw in WaitBuilt() hangs.
    const auto fail = [this] {
        build_failed.store(true, std::memory_order::release);
        SignalBuilt();
    };

    std::array<Owned<MTL::Library>, NUM_PIPELINE_STAGES> libraries{};
    for (size_t index = 0; index < NUM_PIPELINE_STAGES; ++index) {
        if (!present[index]) {
            continue;
        }
        const MslShader& shader = shaders[index];

        libraries[index] = BuildLibrary(device, shader);
        if (!libraries[index]) {
            LOG_ERROR(Render_Metal, "Failed to build the {} library",
                      StageName(static_cast<PipelineStage>(index)));
            fail();
            return;
        }
    }

    constexpr size_t VERTEX = static_cast<size_t>(PipelineStage::Vertex);
    constexpr size_t FRAGMENT = static_cast<size_t>(PipelineStage::Fragment);

    if (!libraries[VERTEX]) {
        LOG_ERROR(Render_Metal, "Graphics pipeline has no vertex library");
        fail();
        return;
    }

    if (geometry && !BuildGeometryPipelines()) {
        fail();
        return;
    }
    if (vtg && !BuildVtgPipelines()) {
        fail();
        return;
    }

    // Metal refuses an ordinary vertex function without a vertex descriptor.
    const bool passthrough_vertex = (geometry && !geometry->xfb_only) || vtg != nullptr;
    Owned<MTL::VertexDescriptor> vertex_descriptor;
    if (!passthrough_vertex) {
        vertex_descriptor = BuildVertexDescriptor();
    }

    // Rasterizer discard is never baked in: Metal only allows it when the vertex
    // function returns void, so the rasterizer skips such draws instead.
    const std::optional<CompactStageInfo>& fragment_info = stage_infos[FRAGMENT];

    NS::Error* error = nullptr;
    {
        MTL4::RenderPipelineDescriptor* mtl4_descriptor =
            MTL4::RenderPipelineDescriptor::alloc()->init();

        mtl4_descriptor->setVertexFunctionDescriptor(
            MakeFunctionDescriptor(libraries[VERTEX].get(), shaders[VERTEX].entry_point));
        if (libraries[FRAGMENT] && !key.rasterizer_discard) {
            mtl4_descriptor->setFragmentFunctionDescriptor(
                MakeFunctionDescriptor(libraries[FRAGMENT].get(), shaders[FRAGMENT].entry_point));
        }
        if (!passthrough_vertex) {
            mtl4_descriptor->setVertexDescriptor(vertex_descriptor.get());
        }
        mtl4_descriptor->setRasterSampleCount(key.sample_count);
        const bool supports_alpha_output =
            fragment_info.has_value() && fragment_info->stores_frag_color[0] &&
            key.num_color_attachments > 0 &&
            key.color_attachments[0].format != MTL::PixelFormatInvalid;
        const bool emulates_alpha_dither = key.EmulatesAlphaToCoverageDither() &&
                                           fragment_info.has_value() &&
                                           fragment_info->stores_frag_alpha[0];
        mtl4_descriptor->setAlphaToCoverageState(
            supports_alpha_output && key.alpha_to_coverage && !emulates_alpha_dither
                ? MTL4::AlphaToCoverageStateEnabled
                : MTL4::AlphaToCoverageStateDisabled);
        mtl4_descriptor->setAlphaToOneState(supports_alpha_output && key.alpha_to_one
                                                ? MTL4::AlphaToOneStateEnabled
                                                : MTL4::AlphaToOneStateDisabled);
        const MTL::PrimitiveTopologyClass input_topology_class = [&]() {
            if (vtg) {
                switch (vtg->geometry_bindings.output_topology) {
                case Shader::OutputTopology::PointList:
                    return MTL::PrimitiveTopologyClassPoint;
                case Shader::OutputTopology::LineStrip:
                    return MTL::PrimitiveTopologyClassLine;
                case Shader::OutputTopology::TriangleStrip:
                    return MTL::PrimitiveTopologyClassTriangle;
                }
            }
            return passthrough_vertex
                       ? MTL::PrimitiveTopologyClassTriangle
                       : static_cast<MTL::PrimitiveTopologyClass>(key.topology_class);
        }();
        mtl4_descriptor->setInputPrimitiveTopology(input_topology_class);

        // Metal requires the fragment function to write every attachment whose
        // write mask is enabled, and a guest depth-only pass compiles to a void
        // fragment shader, whose fragments then vanish entirely. Per attachment, an
        // output the shader never stores is masked too, see StoresColor.
        const bool writes_any_color =
            fragment_info.has_value() &&
            std::ranges::any_of(fragment_info->stores_frag_color,
                                [](bool stores) { return stores; });
        for (u32 index = 0; index < key.num_color_attachments; ++index) {
            const FixedPipelineState::ColorAttachment& attachment = key.color_attachments[index];
            if (attachment.format == MTL::PixelFormatInvalid) {
                continue;
            }
            MTL4::RenderPipelineColorAttachmentDescriptor* target =
                MTL4::RenderPipelineColorAttachmentDescriptor::alloc()->init();
            target->setPixelFormat(attachment.format);
            if (key.rasterizer_discard) {
                target->setWriteMask(MTL::ColorWriteMaskNone);
            } else {
                if (!writes_any_color && attachment.write_mask != MTL::ColorWriteMaskNone) {
                    static std::atomic<u32> reported{0};
                    if (reported++ < 12) {
                        LOG_WARNING(
                            Render_Metal,
                            "Masking off every colour attachment (first is {}, fmt {}): the "
                            "fragment shader writes no colour output at all (has_info={})",
                            index, static_cast<u32>(attachment.format), fragment_info.has_value());
                    }
                }
                const bool stores =
                    StoresColor(key, fragment_info.has_value() ? &*fragment_info : nullptr, index);
                target->setWriteMask(stores ? attachment.write_mask : MTL::ColorWriteMaskNone);
            }
            const auto out_type =
                static_cast<Shader::AttributeType>(key.color_output_types[index]);
            const bool is_integer = out_type == Shader::AttributeType::SignedInt ||
                                    out_type == Shader::AttributeType::UnsignedInt;
            const bool blend_on = attachment.blend_enable && !is_integer;
            target->setBlendingState(blend_on ? MTL4::BlendStateEnabled
                                              : MTL4::BlendStateDisabled);
            if (blend_on) {
                target->setSourceRGBBlendFactor(attachment.src_rgb);
                target->setDestinationRGBBlendFactor(attachment.dst_rgb);
                target->setRgbBlendOperation(attachment.rgb_op);
                target->setSourceAlphaBlendFactor(attachment.src_alpha);
                target->setDestinationAlphaBlendFactor(attachment.dst_alpha);
                target->setAlphaBlendOperation(attachment.alpha_op);
            }
            mtl4_descriptor->colorAttachments()->setObject(target, index);
            target->release();
        }
        // Depth/stencil formats are absent on purpose, MTL4 has no setter for
        // them and the render pass establishes the pairing instead.
        pipeline_state = device.NewRenderPipelineState(mtl4_descriptor, &error);
        mtl4_descriptor->release();
    }

    if (!pipeline_state) {
        // Metal's message names a format but not which attachment carried it.
        std::string attachments;
        for (u32 index = 0; index < key.num_color_attachments; ++index) {
            attachments += fmt::format(
                " color{}={}", index, static_cast<u32>(key.color_attachments[index].format));
        }
        LOG_ERROR(Render_Metal, "Failed to build a render pipeline state: {} (samples={}{})",
                  DescribeError(error), key.sample_count, attachments);
        fail();
        return;
    }
    SignalBuilt();
}

DepthStencilCache::DepthStencilCache(const Device& device_) : device{device_} {}

DepthStencilCache::~DepthStencilCache() = default;

MTL::DepthStencilState* DepthStencilCache::Get(const DepthStencilState& state) {
    if (has_last && last_state == state) {
        return last_result;
    }
    if (const auto it = cache.find(state); it != cache.end()) {
        has_last = true;
        last_state = state;
        last_result = it->second.get();
        return last_result;
    }

    ScopedAutoreleasePool pool;

    MTL::DepthStencilDescriptor* descriptor = MTL::DepthStencilDescriptor::alloc()->init();
    descriptor->setDepthCompareFunction(state.depth_compare);
    descriptor->setDepthWriteEnabled(state.depth_write);

    if (state.stencil_enable) {
        const auto make_face = [](const DepthStencilState::StencilFace& face) {
            MTL::StencilDescriptor* stencil = MTL::StencilDescriptor::alloc()->init();
            stencil->setStencilCompareFunction(face.compare);
            stencil->setStencilFailureOperation(face.fail);
            stencil->setDepthFailureOperation(face.depth_fail);
            stencil->setDepthStencilPassOperation(face.pass);
            stencil->setReadMask(face.read_mask);
            stencil->setWriteMask(face.write_mask);
            return stencil;
        };

        MTL::StencilDescriptor* front = make_face(state.front);
        MTL::StencilDescriptor* back = make_face(state.back);
        descriptor->setFrontFaceStencil(front);
        descriptor->setBackFaceStencil(back);
        front->release();
        back->release();
    }

    Owned<MTL::DepthStencilState> result =
        Adopt(device.GetDevice()->newDepthStencilState(descriptor));
    descriptor->release();

    if (!result) {
        LOG_ERROR(Render_Metal, "Failed to create a depth/stencil state");
        return nullptr;
    }

    MTL::DepthStencilState* const raw = cache.emplace(state, std::move(result)).first->second.get();
    has_last = true;
    last_state = state;
    last_result = raw;
    return raw;
}

} // namespace Metal
