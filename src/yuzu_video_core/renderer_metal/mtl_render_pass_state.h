// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstring>

#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include "yuzu_video_core/metal_common/metal_wrapper.h"

namespace Metal {

constexpr size_t NUM_RT = 8;

/// What should happen to an attachment's contents when a pass begins.
enum class LoadAction : u8 {
    Load,     ///< Preserve what is already there.
    Clear,    ///< Overwrite with the clear value. Free on tile-based hardware.
    DontCare, ///< Contents are undefined; the pass writes every pixel.
};

/// What should happen to an attachment's contents when a pass ends.
enum class StoreAction : u8 {
    Store,    ///< Write the tile back to memory.
    DontCare, ///< Discard. On a TBDR this skips the writeback entirely.
    Resolve,  ///< Resolve MSAA into the resolve texture and discard the source.
};

struct ColorAttachment {
    MTL::Texture* texture{};
    MTL::Texture* resolve_texture{};
    std::array<f64, 4> clear_color{};
    u32 level{};
    u32 slice{};
    u32 resolve_level{};
    u32 resolve_slice{};
    LoadAction load_action{LoadAction::Load};
    StoreAction store_action{StoreAction::Store};

    [[nodiscard]] bool operator==(const ColorAttachment& other) const noexcept {
        return texture == other.texture && resolve_texture == other.resolve_texture &&
               level == other.level && slice == other.slice &&
               resolve_level == other.resolve_level && resolve_slice == other.resolve_slice &&
               load_action == other.load_action && store_action == other.store_action &&
               clear_color == other.clear_color;
    }
};

struct DepthStencilAttachment {
    MTL::Texture* texture{};
    MTL::Texture* resolve_texture{};
    MTL::Texture* stencil_resolve_texture{};
    f64 clear_depth{1.0};
    u32 clear_stencil{};
    u32 level{};
    u32 slice{};
    LoadAction depth_load{LoadAction::Load};
    StoreAction depth_store{StoreAction::Store};
    LoadAction stencil_load{LoadAction::Load};
    StoreAction stencil_store{StoreAction::Store};
    bool has_stencil{};

    [[nodiscard]] bool operator==(const DepthStencilAttachment& other) const noexcept {
        return texture == other.texture && resolve_texture == other.resolve_texture &&
               stencil_resolve_texture == other.stencil_resolve_texture &&
               level == other.level && slice == other.slice &&
               depth_load == other.depth_load && depth_store == other.depth_store &&
               stencil_load == other.stencil_load && stencil_store == other.stencil_store &&
               has_stencil == other.has_stencil && clear_depth == other.clear_depth &&
               clear_stencil == other.clear_stencil;
    }
};

/// Everything needed to open a render command encoder. Kept as a plain value
/// rather than a cached object, a render pass descriptor is cheap and transient
/// so caching one would cost more than rebuilding it.
struct RenderPassState {
    std::array<ColorAttachment, NUM_RT> color{};
    DepthStencilAttachment depth_stencil{};
    u32 num_color{};
    u32 render_area_width{};
    u32 render_area_height{};
    u32 num_layers{1};
    u32 sample_count{1};

    /// Where occlusion counts land. Metal has no query pool, a pass names one
    /// buffer and each draw accumulates into the 8-byte slot the encoder's
    /// visibility mode picks. Metal overwrites the slot instead of accumulating
    /// across passes, so a counter spanning several passes needs one slot per
    /// pass and the totals summed.
    MTL::Buffer* visibility_buffer{};

    /// A declared attachment's view could not be built, so this pass has FEWER
    /// attachments than the guest's pipelines were compiled against. Draws have
    /// to be skipped, the unmatched colour writes land in real tile memory.
    /// Clears can still run, they only touch what is bound. Kept out of
    /// operator== on purpose.
    bool incomplete{};

    [[nodiscard]] bool operator==(const RenderPassState& other) const noexcept {
        if (num_color != other.num_color || render_area_width != other.render_area_width ||
            render_area_height != other.render_area_height || num_layers != other.num_layers ||
            sample_count != other.sample_count || visibility_buffer != other.visibility_buffer ||
            !(depth_stencil == other.depth_stencil)) {
            return false;
        }
        for (u32 i = 0; i < num_color; ++i) {
            if (!(color[i] == other.color[i])) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool operator!=(const RenderPassState& other) const noexcept {
        return !(*this == other);
    }

    [[nodiscard]] bool HasAttachments() const noexcept {
        return num_color > 0 || depth_stencil.texture != nullptr;
    }

    /// Whether this pass renders into the very same images as @p other, ignoring
    /// load and store actions. Compares texture identity, since two shadow
    /// cascades match on dimensions and format but are different textures, and
    /// only a real reopen makes the tile store and reload between them wasted.
    [[nodiscard]] bool SameAttachments(const RenderPassState& other) const noexcept {
        if (num_color != other.num_color ||
            depth_stencil.texture != other.depth_stencil.texture ||
            depth_stencil.level != other.depth_stencil.level ||
            depth_stencil.slice != other.depth_stencil.slice) {
            return false;
        }
        for (u32 i = 0; i < num_color; ++i) {
            if (color[i].texture != other.color[i].texture ||
                color[i].level != other.color[i].level || color[i].slice != other.color[i].slice) {
                return false;
            }
        }
        return true;
    }

    /// Whether an encoder already open with @p other can absorb this pass without
    /// being restarted. Store actions are compared, a pass wanting to resolve or
    /// discard where the open one stores needs its own encoder or the resolve
    /// never happens. Load actions are skipped, they only take effect when a pass
    /// begins so a Load against an open pass does nothing. A Clear still forces a
    /// restart, the clear has to actually happen.
    [[nodiscard]] bool IsCompatibleWith(const RenderPassState& other) const noexcept {
        if (num_color != other.num_color || num_layers != other.num_layers ||
            sample_count != other.sample_count) {
            return false;
        }
        // The visibility buffer is named by the descriptor, so an encoder opened
        // without one cannot start counting later.
        if (visibility_buffer != other.visibility_buffer) {
            return false;
        }
        // renderTargetWidth/Height are baked into the descriptor, so reusing an
        // encoder opened with a different area keeps the *old* size and clips
        // away anything the new pass draws outside it.
        if (render_area_width != other.render_area_width ||
            render_area_height != other.render_area_height) {
            return false;
        }
        if (depth_stencil.texture != other.depth_stencil.texture ||
            depth_stencil.resolve_texture != other.depth_stencil.resolve_texture ||
            depth_stencil.stencil_resolve_texture != other.depth_stencil.stencil_resolve_texture ||
            depth_stencil.level != other.depth_stencil.level ||
            depth_stencil.slice != other.depth_stencil.slice ||
            depth_stencil.has_stencil != other.depth_stencil.has_stencil ||
            depth_stencil.depth_store != other.depth_stencil.depth_store ||
            depth_stencil.stencil_store != other.depth_stencil.stencil_store) {
            return false;
        }
        if (other.depth_stencil.depth_load == LoadAction::Clear ||
            other.depth_stencil.stencil_load == LoadAction::Clear) {
            return false;
        }
        for (u32 i = 0; i < num_color; ++i) {
            if (color[i].texture != other.color[i].texture ||
                color[i].level != other.color[i].level || color[i].slice != other.color[i].slice ||
                color[i].resolve_texture != other.color[i].resolve_texture ||
                color[i].resolve_level != other.color[i].resolve_level ||
                color[i].resolve_slice != other.color[i].resolve_slice ||
                color[i].store_action != other.color[i].store_action) {
                return false;
            }
            if (other.color[i].load_action == LoadAction::Clear) {
                return false;
            }
        }
        return true;
    }
};

/// Builds a transient MTLRenderPassDescriptor from @p state. The descriptor is
/// autoreleased, so the caller needs an autorelease pool. Unused right now, the
/// Metal 4 path calls BuildMTL4RenderPassDescriptor.
[[nodiscard]] MTL::RenderPassDescriptor* BuildRenderPassDescriptor(const RenderPassState& state);

/// Metal 4 counterpart, same contents. MTL4::RenderPassDescriptor is a separate
/// class that takes depth and stencil through setters. Also autoreleased.
[[nodiscard]] MTL4::RenderPassDescriptor* BuildMTL4RenderPassDescriptor(
    const RenderPassState& state);

} // namespace Metal
