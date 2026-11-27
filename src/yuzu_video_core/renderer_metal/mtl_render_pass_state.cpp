// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later


#include "yuzu_video_core/renderer_metal/mtl_render_pass_state.h"

namespace Metal {

namespace {

MTL::LoadAction ToMtl(LoadAction action) {
    switch (action) {
    case LoadAction::Clear:
        return MTL::LoadActionClear;
    case LoadAction::DontCare:
        return MTL::LoadActionDontCare;
    case LoadAction::Load:
    default:
        return MTL::LoadActionLoad;
    }
}

MTL::StoreAction ToMtl(StoreAction action) {
    switch (action) {
    case StoreAction::DontCare:
        return MTL::StoreActionDontCare;
    case StoreAction::Resolve:
        // Preserve the MSAA source texture after resolving (guest may still render to
        // or read from it in subsequent passes).
        return MTL::StoreActionStoreAndMultisampleResolve;
    case StoreAction::Store:
    default:
        return MTL::StoreActionStore;
    }
}

} // Anonymous namespace

MTL::RenderPassDescriptor* BuildRenderPassDescriptor(const RenderPassState& state) {
    MTL::RenderPassDescriptor* descriptor = MTL::RenderPassDescriptor::renderPassDescriptor();

    auto* color_attachments = descriptor->colorAttachments();
    for (u32 i = 0; i < state.num_color; ++i) {
        const ColorAttachment& attachment = state.color[i];
        if (attachment.texture == nullptr) {
            continue;
        }

        auto* descriptor_attachment = color_attachments->object(i);
        descriptor_attachment->setTexture(attachment.texture);
        descriptor_attachment->setLevel(attachment.level);
        if (attachment.texture->textureType() == MTL::TextureType3D) {
            // A volume attachment selects its plane through depthPlane; slice
            // must stay zero or Metal rejects the descriptor. When rendering to
            // multiple layers (renderTargetArrayLength > 1), Metal requires
            // depthPlane == 0.
            descriptor_attachment->setDepthPlane(state.num_layers > 1 ? 0u : attachment.slice);
        } else {
            descriptor_attachment->setSlice(attachment.slice);
        }
        descriptor_attachment->setLoadAction(ToMtl(attachment.load_action));
        descriptor_attachment->setStoreAction(ToMtl(attachment.store_action));
        descriptor_attachment->setClearColor(
            MTL::ClearColor::Make(attachment.clear_color[0], attachment.clear_color[1],
                                  attachment.clear_color[2], attachment.clear_color[3]));

        if (attachment.store_action == StoreAction::Resolve) {
            descriptor_attachment->setResolveTexture(attachment.resolve_texture);
            descriptor_attachment->setResolveLevel(attachment.resolve_level);
            if (attachment.resolve_texture != nullptr &&
                attachment.resolve_texture->textureType() == MTL::TextureType3D) {
                descriptor_attachment->setResolveDepthPlane(
                    state.num_layers > 1 ? 0u : attachment.resolve_slice);
            } else {
                descriptor_attachment->setResolveSlice(attachment.resolve_slice);
            }
        }
    }

    const DepthStencilAttachment& ds = state.depth_stencil;
    if (ds.texture != nullptr) {
        auto* depth = descriptor->depthAttachment();
        depth->setTexture(ds.texture);
        depth->setLevel(ds.level);
        depth->setSlice(ds.slice);
        depth->setLoadAction(ToMtl(ds.depth_load));
        depth->setStoreAction(ToMtl(ds.depth_store));
        depth->setClearDepth(ds.clear_depth);
        if (ds.depth_store == StoreAction::Resolve && ds.resolve_texture != nullptr) {
            depth->setResolveTexture(ds.resolve_texture);
            depth->setDepthResolveFilter(MTL::MultisampleDepthResolveFilterSample0);
        }

        if (ds.has_stencil) {
            // Metal keeps depth and stencil as separate attachment slots even
            // when they share one packed texture, so both have to be bound.
            auto* stencil = descriptor->stencilAttachment();
            stencil->setTexture(ds.texture);
            stencil->setLevel(ds.level);
            stencil->setSlice(ds.slice);
            stencil->setLoadAction(ToMtl(ds.stencil_load));
            stencil->setStoreAction(ToMtl(ds.stencil_store));
            stencil->setClearStencil(ds.clear_stencil);
            if (ds.stencil_store == StoreAction::Resolve && ds.stencil_resolve_texture != nullptr) {
                stencil->setResolveTexture(ds.stencil_resolve_texture);
                stencil->setStencilResolveFilter(MTL::MultisampleStencilResolveFilterSample0);
            }
        }
    }

    // Required for attachmentless passes: with nothing bound, this is the only
    // thing that tells Metal how to size the tile.
    if (state.render_area_width > 0 && state.render_area_height > 0) {
        descriptor->setRenderTargetWidth(state.render_area_width);
        descriptor->setRenderTargetHeight(state.render_area_height);
    }
    if (state.visibility_buffer != nullptr) {
        descriptor->setVisibilityResultBuffer(state.visibility_buffer);
    }
    if (state.num_layers > 1) {
        descriptor->setRenderTargetArrayLength(state.num_layers);
    }
    if (!state.HasAttachments()) {
        descriptor->setDefaultRasterSampleCount(state.sample_count);
    }

    return descriptor;
}

MTL4::RenderPassDescriptor* BuildMTL4RenderPassDescriptor(const RenderPassState& state) {
    // MTL4::RenderPassDescriptor does not derive from MTL::RenderPassDescriptor,
    // but the attachment descriptors are the same classic types. Only depth and
    // stencil differ, MTL4 takes a descriptor through setDepthAttachment /
    // setStencilAttachment instead of handing one back to mutate.
    MTL4::RenderPassDescriptor* descriptor =
        MTL4::RenderPassDescriptor::alloc()->init()->autorelease();

    auto* color_attachments = descriptor->colorAttachments();
    for (u32 i = 0; i < state.num_color; ++i) {
        const ColorAttachment& attachment = state.color[i];
        if (attachment.texture == nullptr) {
            continue;
        }

        auto* descriptor_attachment = color_attachments->object(i);
        descriptor_attachment->setTexture(attachment.texture);
        descriptor_attachment->setLevel(attachment.level);
        if (attachment.texture->textureType() == MTL::TextureType3D) {
            descriptor_attachment->setDepthPlane(state.num_layers > 1 ? 0u : attachment.slice);
        } else {
            descriptor_attachment->setSlice(attachment.slice);
        }
        descriptor_attachment->setLoadAction(ToMtl(attachment.load_action));
        descriptor_attachment->setStoreAction(ToMtl(attachment.store_action));
        descriptor_attachment->setClearColor(
            MTL::ClearColor::Make(attachment.clear_color[0], attachment.clear_color[1],
                                  attachment.clear_color[2], attachment.clear_color[3]));

        if (attachment.store_action == StoreAction::Resolve) {
            descriptor_attachment->setResolveTexture(attachment.resolve_texture);
            descriptor_attachment->setResolveLevel(attachment.resolve_level);
            if (attachment.resolve_texture != nullptr &&
                attachment.resolve_texture->textureType() == MTL::TextureType3D) {
                descriptor_attachment->setResolveDepthPlane(
                    state.num_layers > 1 ? 0u : attachment.resolve_slice);
            } else {
                descriptor_attachment->setResolveSlice(attachment.resolve_slice);
            }
        }
    }

    const DepthStencilAttachment& ds = state.depth_stencil;
    if (ds.texture != nullptr) {
        if (ds.texture->pixelFormat() != MTL::PixelFormatStencil8) {
            MTL::RenderPassDepthAttachmentDescriptor* depth =
                MTL::RenderPassDepthAttachmentDescriptor::alloc()->init();
            depth->setTexture(ds.texture);
            depth->setLevel(ds.level);
            depth->setSlice(ds.slice);
            depth->setLoadAction(ToMtl(ds.depth_load));
            depth->setStoreAction(ToMtl(ds.depth_store));
            depth->setClearDepth(ds.clear_depth);
            if (ds.depth_store == StoreAction::Resolve && ds.resolve_texture != nullptr) {
                depth->setResolveTexture(ds.resolve_texture);
                depth->setDepthResolveFilter(MTL::MultisampleDepthResolveFilterSample0);
            }
            descriptor->setDepthAttachment(depth);
            depth->release();
        }

        if (ds.has_stencil || ds.texture->pixelFormat() == MTL::PixelFormatStencil8) {
            // Depth and stencil stay separate attachment slots even when they
            // share one packed texture.
            MTL::RenderPassStencilAttachmentDescriptor* stencil =
                MTL::RenderPassStencilAttachmentDescriptor::alloc()->init();
            stencil->setTexture(ds.texture);
            stencil->setLevel(ds.level);
            stencil->setSlice(ds.slice);
            stencil->setLoadAction(ToMtl(ds.stencil_load));
            stencil->setStoreAction(ToMtl(ds.stencil_store));
            stencil->setClearStencil(ds.clear_stencil);
            if (ds.stencil_store == StoreAction::Resolve && ds.stencil_resolve_texture != nullptr) {
                stencil->setResolveTexture(ds.stencil_resolve_texture);
                stencil->setStencilResolveFilter(MTL::MultisampleStencilResolveFilterSample0);
            }
            descriptor->setStencilAttachment(stencil);
            stencil->release();
        }
    }

    if (state.render_area_width > 0 && state.render_area_height > 0) {
        descriptor->setRenderTargetWidth(state.render_area_width);
        descriptor->setRenderTargetHeight(state.render_area_height);
    }
    if (state.visibility_buffer != nullptr) {
        descriptor->setVisibilityResultBuffer(state.visibility_buffer);
    }
    if (state.num_layers > 1) {
        descriptor->setRenderTargetArrayLength(state.num_layers);
    }
    if (!state.HasAttachments()) {
        descriptor->setDefaultRasterSampleCount(state.sample_count);
    }

    return descriptor;
}

} // namespace Metal
