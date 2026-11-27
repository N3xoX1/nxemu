// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstring>

#include "yuzu_common/alignment.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/metal_common/metal_wrapper.h"

namespace Metal {
/// Bump allocator for inline shader constants. Metal 4 has no setBytes, so every inline constant
/// needs a real buffer with a GPU address. One ring per command allocator, reset only when that
/// allocator is; reset it per frame instead and you hand out memory the GPU is still reading.
struct InlineConstantRing {
    MTL::Buffer* buffer{};
    u64 gpu_address{};
    u8* mapped{};
    u64 capacity{};
    u64 offset{};

    void Reset() noexcept {
        offset = 0;
    }

    [[nodiscard]] void* Allocate(u64 size, u64 alignment, u64& out_offset) noexcept {
        if (buffer == nullptr || mapped == nullptr) {
            return nullptr;
        }
        const u64 aligned = Common::AlignUp(offset, alignment);
        if (aligned + size > capacity) {
            return nullptr;
        }
        out_offset = aligned;
        offset = aligned + size;
        return mapped + aligned;
    }
};

/// Constant buffer offsets must be 256-byte aligned on macOS and 32 on Apple GPUs.
constexpr u64 INLINE_CONSTANT_ALIGNMENT = 256;

/// What a binding call writes when the caller has no resource. An MTL4 argument table is
/// persistent scheduler state, so skipping the call for a null resource leaves the old, possibly
/// freed, occupant there and the shader page faults on it. Textures and samplers unbind to a
/// zeroed MTL::ResourceID; buffers can't, address 0 is a null deref, so they get a real one.
struct NullBindings {
    MTL::GPUAddress buffer{};
};

[[nodiscard]] inline MTL::ResourceID TextureID(MTL::Texture* texture) noexcept {
    return texture != nullptr ? texture->gpuResourceID() : MTL::ResourceID{};
}

[[nodiscard]] inline MTL::ResourceID SamplerID(MTL::SamplerState* sampler) noexcept {
    return sampler != nullptr ? sampler->gpuResourceID() : MTL::ResourceID{};
}

/// Zero means there is no stand-in either, and the caller leaves the slot as it found it.
[[nodiscard]] inline MTL::GPUAddress BufferAddress(MTL::Buffer* buffer, NS::UInteger offset,
                                                   const NullBindings* nulls) noexcept {
    if (buffer != nullptr) {
        return buffer->gpuAddress() + offset;
    }
    return nulls != nullptr ? nulls->buffer : MTL::GPUAddress{};
}

/// Thin wrapper over MTL4::RenderCommandEncoder, with operator-> so call sites read like it.
class RenderEncoder {
public:
    RenderEncoder() = default;
    RenderEncoder(std::nullptr_t) {}
    explicit RenderEncoder(MTL4::RenderCommandEncoder* encoder) : mtl4{encoder} {}

    RenderEncoder* operator->() noexcept {
        return this;
    }
    const RenderEncoder* operator->() const noexcept {
        return this;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return mtl4 != nullptr;
    }
    [[nodiscard]] bool operator==(std::nullptr_t) const noexcept {
        return mtl4 == nullptr;
    }

    [[nodiscard]] MTL4::RenderCommandEncoder* Metal4() const noexcept {
        return mtl4;
    }

    /// Stores pointers to argument tables already bound on this encoder without issuing
    /// redundant Objective-C setArgumentTable calls (hot path for CurrentRender).
    void AttachArgumentTables(MTL4::ArgumentTable* vertex, MTL4::ArgumentTable* fragment,
                              InlineConstantRing* ring = nullptr,
                              const NullBindings* null_bindings = nullptr) noexcept {
        vertex_table = vertex;
        fragment_table = fragment;
        inline_ring = ring;
        nulls = null_bindings;
    }

    void SetArgumentTables(MTL4::ArgumentTable* vertex, MTL4::ArgumentTable* fragment,
                           InlineConstantRing* ring = nullptr,
                           const NullBindings* null_bindings = nullptr) {
        AttachArgumentTables(vertex, fragment, ring, null_bindings);
        if (mtl4 != nullptr) {
            if (vertex != nullptr) {
                mtl4->setArgumentTable(vertex, MTL::RenderStageVertex);
            }
            if (fragment != nullptr) {
                mtl4->setArgumentTable(fragment, MTL::RenderStageFragment);
            }
        }
    }

    void setViewport(const MTL::Viewport& v) {
        if (mtl4) mtl4->setViewport(v);
    }
    void setViewports(const MTL::Viewport* v, NS::UInteger count) {
        if (mtl4) mtl4->setViewports(v, count);
    }
    void setScissorRect(const MTL::ScissorRect& r) {
        if (mtl4) mtl4->setScissorRect(r);
    }
    void setScissorRects(const MTL::ScissorRect* r, NS::UInteger count) {
        if (mtl4) mtl4->setScissorRects(r, count);
    }
    void setRenderPipelineState(MTL::RenderPipelineState* p) {
        if (mtl4) mtl4->setRenderPipelineState(p);
    }
    void setDepthStencilState(MTL::DepthStencilState* s) {
        if (mtl4) mtl4->setDepthStencilState(s);
    }
    void setCullMode(MTL::CullMode m) {
        if (mtl4) mtl4->setCullMode(m);
    }
    void setFrontFacingWinding(MTL::Winding w) {
        if (mtl4) mtl4->setFrontFacingWinding(w);
    }
    void setDepthBias(float bias, float slope, float clamp) {
        if (mtl4) mtl4->setDepthBias(bias, slope, clamp);
    }
    void setDepthClipMode(MTL::DepthClipMode m) {
        if (mtl4) mtl4->setDepthClipMode(m);
    }
    void setTriangleFillMode(MTL::TriangleFillMode m) {
        if (mtl4) mtl4->setTriangleFillMode(m);
    }
    void setStencilReferenceValue(uint32_t v) {
        if (mtl4) mtl4->setStencilReferenceValue(v);
    }
    void setStencilReferenceValues(uint32_t front, uint32_t back) {
        if (mtl4) mtl4->setStencilReferenceValues(front, back);
    }
    void setBlendColor(float r, float g, float b, float a) {
        if (mtl4) mtl4->setBlendColor(r, g, b, a);
    }
    void setVisibilityResultMode(MTL::VisibilityResultMode mode, NS::UInteger offset) {
        if (mtl4) mtl4->setVisibilityResultMode(mode, offset);
    }
    void setLabel(const NS::String* label) {
        if (mtl4) mtl4->setLabel(label);
    }

    // --- Binding. Argument tables, addressed by GPU address / resource ID. ---

    void setVertexAddress(MTL::GPUAddress address, NS::UInteger index) {
        if (mtl4 && vertex_table) {
            const MTL::GPUAddress effective =
                address != 0 ? address : (nulls != nullptr ? nulls->buffer : MTL::GPUAddress{});
            if (effective != 0) {
                vertex_table->setAddress(effective, index);
            }
        }
    }
    void setFragmentAddress(MTL::GPUAddress address, NS::UInteger index) {
        if (mtl4 && fragment_table) {
            const MTL::GPUAddress effective =
                address != 0 ? address : (nulls != nullptr ? nulls->buffer : MTL::GPUAddress{});
            if (effective != 0) {
                fragment_table->setAddress(effective, index);
            }
        }
    }
    // A null resource still writes the slot. See NullBindings, skipping the call is the bug.
    void setVertexBuffer(MTL::Buffer* buffer, NS::UInteger offset, NS::UInteger index) {
        if (mtl4 && vertex_table) {
            if (const MTL::GPUAddress address = BufferAddress(buffer, offset, nulls)) {
                vertex_table->setAddress(address, index);
            }
        }
    }
    // No setVertexBufferOffset here, on purpose: Metal 4 has no offset-only rebind, so a caller
    // reaching for it should fail to compile instead of quietly binding the old offset.
    void setFragmentBuffer(MTL::Buffer* buffer, NS::UInteger offset, NS::UInteger index) {
        if (mtl4 && fragment_table) {
            if (const MTL::GPUAddress address = BufferAddress(buffer, offset, nulls)) {
                fragment_table->setAddress(address, index);
            }
        }
    }
    void setFragmentTexture(MTL::Texture* texture, NS::UInteger index) {
        if (mtl4 && fragment_table) {
            fragment_table->setTexture(TextureID(texture), index);
        }
    }
    void setFragmentSamplerState(MTL::SamplerState* sampler, NS::UInteger index) {
        if (mtl4 && fragment_table) {
            fragment_table->setSamplerState(SamplerID(sampler), index);
        }
    }

    void setVertexBytes(const void* bytes, NS::UInteger length, NS::UInteger index) {
        if (mtl4 && vertex_table) {
            if (const MTL::GPUAddress address = StageInline(bytes, length)) {
                vertex_table->setAddress(address, index);
            }
        }
    }
    void setFragmentBytes(const void* bytes, NS::UInteger length, NS::UInteger index) {
        if (mtl4 && fragment_table) {
            if (const MTL::GPUAddress address = StageInline(bytes, length)) {
                fragment_table->setAddress(address, index);
            }
        }
    }

    // --- Draws. MTL4 takes the index buffer as an address plus a length. ---

    void drawPrimitives(MTL::PrimitiveType type, NS::UInteger start, NS::UInteger count) {
        if (mtl4) mtl4->drawPrimitives(type, start, count);
    }
    void drawPrimitives(MTL::PrimitiveType type, NS::UInteger start, NS::UInteger count,
                        NS::UInteger instances) {
        if (mtl4) mtl4->drawPrimitives(type, start, count, instances);
    }
    void drawPrimitives(MTL::PrimitiveType type, NS::UInteger start, NS::UInteger count,
                        NS::UInteger instances, NS::UInteger base_instance) {
        if (mtl4) mtl4->drawPrimitives(type, start, count, instances, base_instance);
    }

    void drawIndexedPrimitives(MTL::PrimitiveType type, NS::UInteger index_count,
                               MTL::IndexType index_type, MTL::GPUAddress index_address,
                               NS::UInteger instances, NS::Integer base_vertex,
                               NS::UInteger base_instance) {
        if (mtl4 && index_address != 0) {
            const NS::UInteger stride = index_type == MTL::IndexTypeUInt32 ? 4u : 2u;
            mtl4->drawIndexedPrimitives(type, index_count, index_type, index_address,
                                        index_count * stride, instances, base_vertex,
                                        base_instance);
        }
    }

    void drawIndexedPrimitives(MTL::PrimitiveType type, NS::UInteger index_count,
                               MTL::IndexType index_type, MTL::Buffer* index_buffer,
                               NS::UInteger index_offset, NS::UInteger instances,
                               NS::Integer base_vertex, NS::UInteger base_instance) {
        if (mtl4 && index_buffer) {
            const NS::UInteger stride = index_type == MTL::IndexTypeUInt32 ? 4u : 2u;
            mtl4->drawIndexedPrimitives(type, index_count, index_type,
                                        index_buffer->gpuAddress() + index_offset,
                                        index_count * stride, instances, base_vertex,
                                        base_instance);
        }
    }

    /// Indirect draw. MTL4 folds the offset into the address, there's no separate offset arg.
    void drawPrimitives(MTL::PrimitiveType type, MTL::Buffer* indirect, NS::UInteger offset) {
        if (mtl4 && indirect) {
            mtl4->drawPrimitives(type, indirect->gpuAddress() + offset);
        }
    }

    void drawIndexedPrimitives(MTL::PrimitiveType type, MTL::IndexType index_type,
                               MTL::Buffer* index_buffer, NS::UInteger index_offset,
                               MTL::Buffer* indirect, NS::UInteger indirect_offset) {
        if (mtl4 && index_buffer && indirect) {
            const NS::UInteger length = index_buffer->length();
            if (index_offset >= length) {
                return;
            }
            // The index count comes from the buffer, so this is the only bound we can give.
            const NS::UInteger remaining = length - index_offset;
            mtl4->drawIndexedPrimitives(type, index_type, index_buffer->gpuAddress() + index_offset,
                                        remaining, indirect->gpuAddress() + indirect_offset);
        }
    }

    void barrierAfterQueueStages(MTL::Stages after, MTL::Stages before,
                                 MTL4::VisibilityOptions visibility =
                                     MTL4::VisibilityOptionDevice) {
        if (mtl4) mtl4->barrierAfterQueueStages(after, before, visibility);
    }

    void barrierAfterEncoderStages(MTL::Stages after, MTL::Stages before,
                                   MTL4::VisibilityOptions visibility =
                                       MTL4::VisibilityOptionDevice) {
        if (mtl4) mtl4->barrierAfterEncoderStages(after, before, visibility);
    }

    void endEncoding() {
        if (mtl4) mtl4->endEncoding();
    }

private:
    /// Returns 0 when the ring is exhausted, and the caller drops the binding.
    [[nodiscard]] MTL::GPUAddress StageInline(const void* bytes, NS::UInteger length) {
        if (inline_ring == nullptr || bytes == nullptr || length == 0) {
            return 0;
        }
        u64 at = 0;
        void* const dst = inline_ring->Allocate(length, INLINE_CONSTANT_ALIGNMENT, at);
        if (dst == nullptr) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                LOG_ERROR(Render_Metal, "Metal 4 inline constant ring exhausted");
            }
            return 0;
        }
        std::memcpy(dst, bytes, length);
        return (inline_ring->gpu_address != 0 ? inline_ring->gpu_address
                                              : inline_ring->buffer->gpuAddress()) +
               at;
    }

    MTL4::RenderCommandEncoder* mtl4{};
    MTL4::ArgumentTable* vertex_table{};
    MTL4::ArgumentTable* fragment_table{};
    InlineConstantRing* inline_ring{};
    const NullBindings* nulls{};
};

/// Compute counterpart, with one argument table.
class ComputeEncoder {
public:
    ComputeEncoder() = default;
    ComputeEncoder(std::nullptr_t) {}
    explicit ComputeEncoder(MTL4::ComputeCommandEncoder* encoder) : mtl4{encoder} {}

    ComputeEncoder* operator->() noexcept {
        return this;
    }
    const ComputeEncoder* operator->() const noexcept {
        return this;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return mtl4 != nullptr;
    }
    [[nodiscard]] bool operator==(std::nullptr_t) const noexcept {
        return mtl4 == nullptr;
    }

    [[nodiscard]] MTL4::ComputeCommandEncoder* Metal4() const noexcept {
        return mtl4;
    }

    void SetArgumentTable(MTL4::ArgumentTable* table, InlineConstantRing* ring = nullptr,
                          const NullBindings* null_bindings = nullptr) {
        argument_table = table;
        inline_ring = ring;
        nulls = null_bindings;
        if (mtl4 != nullptr && table != nullptr) {
            mtl4->setArgumentTable(table);
        }
    }

    /// Re-attaches pointers when reusing an already-open compute encoder that
    /// already has the argument table bound, skipping the redundant ObjC call.
    void AttachArgumentTable(MTL4::ArgumentTable* table, InlineConstantRing* ring = nullptr,
                             const NullBindings* null_bindings = nullptr) noexcept {
        argument_table = table;
        inline_ring = ring;
        nulls = null_bindings;
    }

    void setComputePipelineState(MTL::ComputePipelineState* p) {
        if (mtl4) mtl4->setComputePipelineState(p);
    }
    // Like the render side, a null resource still writes the slot. See NullBindings.
    void setBuffer(MTL::Buffer* buffer, NS::UInteger offset, NS::UInteger index) {
        if (mtl4 && argument_table) {
            if (const MTL::GPUAddress address = BufferAddress(buffer, offset, nulls)) {
                argument_table->setAddress(address, index);
            }
        }
    }
    void setTexture(MTL::Texture* texture, NS::UInteger index) {
        if (mtl4 && argument_table) {
            argument_table->setTexture(TextureID(texture), index);
        }
    }
    void setSamplerState(MTL::SamplerState* sampler, NS::UInteger index) {
        if (mtl4 && argument_table) {
            argument_table->setSamplerState(SamplerID(sampler), index);
        }
    }
    void setBytes(const void* bytes, NS::UInteger length, NS::UInteger index) {
        if (mtl4 && argument_table && inline_ring && bytes != nullptr && length != 0) {
            u64 at = 0;
            void* const dst = inline_ring->Allocate(length, INLINE_CONSTANT_ALIGNMENT, at);
            if (dst == nullptr) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    LOG_ERROR(Render_Metal, "Metal 4 inline constant ring exhausted (compute)");
                }
                return;
            }
            std::memcpy(dst, bytes, length);
            argument_table->setAddress(inline_ring->gpu_address + at, index);
        }
    }
    void dispatchThreadgroups(const MTL::Size& groups, const MTL::Size& per_group) {
        if (mtl4) mtl4->dispatchThreadgroups(groups, per_group);
    }
    void dispatchThreads(const MTL::Size& threads, const MTL::Size& per_group) {
        if (mtl4) mtl4->dispatchThreads(threads, per_group);
    }
    /// Indirect dispatch, grid read from a buffer the GPU wrote.
    void dispatchThreadgroups(MTL::Buffer* indirect, NS::UInteger offset,
                              const MTL::Size& per_group) {
        if (mtl4 && indirect) {
            mtl4->dispatchThreadgroups(indirect->gpuAddress() + offset, per_group);
        }
    }
    void setLabel(const NS::String* label) {
        if (mtl4) mtl4->setLabel(label);
    }

    /// Makes everything encoded here visible to LATER queue stages, the only outbound barrier
    /// direction we have. MetalFX encodes into the command buffer directly, so it never sees
    /// the inbound barrier at the top of our encoders.
    void barrierBeforeQueueStages(MTL::Stages after_encoder_stages,
                                  MTL::Stages before_queue_stages) {
        if (mtl4) {
            mtl4->barrierAfterStages(after_encoder_stages, before_queue_stages,
                                     MTL4::VisibilityOptionDevice);
        }
    }

    void barrierAfterQueueStages(MTL::Stages after, MTL::Stages before,
                                 MTL4::VisibilityOptions visibility =
                                     MTL4::VisibilityOptionDevice) {
        if (mtl4) mtl4->barrierAfterQueueStages(after, before, visibility);
    }

    void barrierAfterEncoderStages(MTL::Stages after, MTL::Stages before,
                                   MTL4::VisibilityOptions visibility =
                                       MTL4::VisibilityOptionDevice) {
        if (mtl4) mtl4->barrierAfterEncoderStages(after, before, visibility);
    }

    void endEncoding() {
        if (mtl4) mtl4->endEncoding();
    }

private:
    MTL4::ComputeCommandEncoder* mtl4{};
    MTL4::ArgumentTable* argument_table{};
    InlineConstantRing* inline_ring{};
    const NullBindings* nulls{};
};

/// Copies and fills. Metal 4 has no blit encoder, copies live on MTL4::ComputeCommandEncoder,
/// so this is a compute encoder behind a copy-shaped interface.
class BlitEncoder {
public:
    BlitEncoder() = default;
    BlitEncoder(std::nullptr_t) {}
    explicit BlitEncoder(MTL4::ComputeCommandEncoder* encoder) : mtl4{encoder} {}

    BlitEncoder* operator->() noexcept {
        return this;
    }
    const BlitEncoder* operator->() const noexcept {
        return this;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return mtl4 != nullptr;
    }
    [[nodiscard]] bool operator==(std::nullptr_t) const noexcept {
        return mtl4 == nullptr;
    }

    void copyFromTexture(const MTL::Texture* src, NS::UInteger src_slice, NS::UInteger src_level,
                         const MTL::Origin& src_origin, const MTL::Size& src_size,
                         const MTL::Texture* dst, NS::UInteger dst_slice, NS::UInteger dst_level,
                         const MTL::Origin& dst_origin) {
        if (mtl4) {
            mtl4->copyFromTexture(src, src_slice, src_level, src_origin, src_size, dst, dst_slice,
                                  dst_level, dst_origin);
        }
    }

    void copyFromBuffer(const MTL::Buffer* src, NS::UInteger src_offset, NS::UInteger bytes_per_row,
                        NS::UInteger bytes_per_image, const MTL::Size& size,
                        const MTL::Texture* dst, NS::UInteger dst_slice, NS::UInteger dst_level,
                        const MTL::Origin& dst_origin, MTL::BlitOption options) {
        if (mtl4) {
            mtl4->copyFromBuffer(src, src_offset, bytes_per_row, bytes_per_image, size, dst,
                                 dst_slice, dst_level, dst_origin, options);
        }
    }

    void copyFromBuffer(const MTL::Buffer* src, NS::UInteger src_offset, const MTL::Buffer* dst,
                        NS::UInteger dst_offset, NS::UInteger size) {
        if (mtl4) {
            mtl4->copyFromBuffer(src, src_offset, dst, dst_offset, size);
        }
    }

    /// Needed when a copy reads what an earlier one in the same encoder wrote: Metal 4 tracks
    /// no hazards, so copies are unordered otherwise.
    void barrierAfterCopies() {
        if (mtl4) {
            const MTL::Stages stages =
                static_cast<MTL::Stages>(MTL::StageDispatch | MTL::StageBlit);
            mtl4->barrierAfterEncoderStages(stages, stages, MTL4::VisibilityOptionDevice);
        }
    }

    void copyFromTexture(const MTL::Texture* src, NS::UInteger src_slice, NS::UInteger src_level,
                         const MTL::Origin& src_origin, const MTL::Size& src_size,
                         const MTL::Buffer* dst, NS::UInteger dst_offset,
                         NS::UInteger bytes_per_row, NS::UInteger bytes_per_image,
                         MTL::BlitOption options) {
        if (mtl4) {
            mtl4->copyFromTexture(src, src_slice, src_level, src_origin, src_size, dst, dst_offset,
                                  bytes_per_row, bytes_per_image, options);
        }
    }

    void fillBuffer(const MTL::Buffer* buffer, const NS::Range& range, uint8_t value) {
        if (mtl4) mtl4->fillBuffer(buffer, range, value);
    }

    void setLabel(const NS::String* label) {
        if (mtl4) mtl4->setLabel(label);
    }

    void barrierAfterQueueStages(MTL::Stages after, MTL::Stages before,
                                 MTL4::VisibilityOptions visibility =
                                     MTL4::VisibilityOptionDevice) {
        if (mtl4) mtl4->barrierAfterQueueStages(after, before, visibility);
    }

    void barrierAfterEncoderStages(MTL::Stages after, MTL::Stages before,
                                   MTL4::VisibilityOptions visibility =
                                       MTL4::VisibilityOptionDevice) {
        if (mtl4) mtl4->barrierAfterEncoderStages(after, before, visibility);
    }

    void endEncoding() {
        if (mtl4) mtl4->endEncoding();
    }

private:
    MTL4::ComputeCommandEncoder* mtl4{};
};

} // namespace Metal
