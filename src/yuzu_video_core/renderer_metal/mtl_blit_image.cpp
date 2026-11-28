// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <string>

#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/metal_common/metal_device.h"
#include "yuzu_video_core/renderer_metal/mtl_blit_image.h"
#include "yuzu_video_core/renderer_metal/mtl_render_pass_state.h"
#include "yuzu_video_core/renderer_metal/mtl_scheduler.h"
#include "yuzu_video_core/renderer_metal/mtl_shader_util.h"

namespace Metal {

namespace {

constexpr u32 BLIT_UNIFORM_INDEX = 1;
constexpr u32 BLIT_TEXTURE_INDEX = 0;
constexpr u32 BLIT_SAMPLER_INDEX = 0;

/// Source rectangle in normalised texture coordinates, pushed with setVertexBytes.
struct BlitRect {
    f32 left;
    f32 top;
    f32 right;
    f32 bottom;
};

constexpr const char* BLIT_MSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

struct BlitRect {
    float left;
    float top;
    float right;
    float bottom;
};

vertex VertexOut blit_vertex(uint vid [[vertex_id]],
                             constant BlitRect& rect [[buffer(1)]]) {
    const float2 positions[3] = { float2(-1.0, -3.0), float2(-1.0, 1.0), float2(3.0, 1.0) };
    const float2 uvs[3]       = { float2( 0.0,  2.0), float2( 0.0, 0.0), float2(2.0, 0.0) };

    VertexOut out;
    out.position = float4(positions[vid], 0.0, 1.0);
    out.uv = float2(mix(rect.left, rect.right, uvs[vid].x),
                    mix(rect.top,  rect.bottom, uvs[vid].y));
    return out;
}

fragment float4 blit_fragment(VertexOut in [[stage_in]],
                              texture2d<float> src [[texture(0)]],
                              sampler src_sampler [[sampler(0)]]) {
    return src.sample(src_sampler, in.uv);
}

fragment uint4 blit_fragment_u(VertexOut in [[stage_in]],
                               texture2d<uint> src [[texture(0)]]) {
    const float2 dim = float2(src.get_width(), src.get_height());
    const uint2 coord = uint2(clamp(floor(in.uv * dim), float2(0.0), dim - 1.0));
    return src.read(coord);
}

fragment int4 blit_fragment_i(VertexOut in [[stage_in]],
                              texture2d<int> src [[texture(0)]]) {
    const float2 dim = float2(src.get_width(), src.get_height());
    const uint2 coord = uint2(clamp(floor(in.uv * dim), float2(0.0), dim - 1.0));
    return src.read(coord);
}

// Packs a single-sample texture into every sample of a multisampled attachment.
// info = (scale_x, scale_y, bias_x, bias_y), where bias = src_origin - dst_origin * scale.
// When scale == (1, 1) this broadcasts each texel to all samples; when scale == (2, 2)
// (e.g. Maxwell 3840x2160 1x -> 1920x1080 4x) it maps Maxwell's spatial sample grid losslessly.
static inline uint2 msaa_pack_coord(float2 pos, uint sample_id, int4 info, uint w, uint h) {
    const int2 scale = max(info.xy, int2(1));
    const int2 sub = int2(int(sample_id) % scale.x, (int(sample_id) / scale.x) % scale.y);
    const int2 coord = clamp(int2(pos) * scale + info.zw + sub, int2(0), int2(int(w) - 1, int(h) - 1));
    return uint2(coord);
}

fragment float4 expand_fragment(VertexOut in [[stage_in]],
                                uint sample_id [[sample_id]],
                                texture2d<float> src [[texture(0)]],
                                constant int4& info [[buffer(0)]]) {
    return src.read(msaa_pack_coord(in.position.xy, sample_id, info, src.get_width(), src.get_height()));
}

fragment uint4 expand_fragment_u(VertexOut in [[stage_in]],
                                 uint sample_id [[sample_id]],
                                 texture2d<uint> src [[texture(0)]],
                                 constant int4& info [[buffer(0)]]) {
    return src.read(msaa_pack_coord(in.position.xy, sample_id, info, src.get_width(), src.get_height()));
}

fragment int4 expand_fragment_i(VertexOut in [[stage_in]],
                                uint sample_id [[sample_id]],
                                texture2d<int> src [[texture(0)]],
                                constant int4& info [[buffer(0)]]) {
    return src.read(msaa_pack_coord(in.position.xy, sample_id, info, src.get_width(), src.get_height()));
}

// Unpacks a multisampled texture into a larger single-sample texture laid out in Maxwell's
// spatial sample grid. info = (scale_x, scale_y, num_samples, 0).
static inline uint2 msaa_unpack_coord(float2 pos, uint4 info, uint w, uint h, thread uint& sid) {
    const uint2 p = uint2(pos);
    const uint2 scale = max(info.xy, uint2(1u));
    const uint2 src_p = min(p / scale, uint2(w - 1u, h - 1u));
    const uint2 sub = p - src_p * scale;
    sid = min(sub.y * scale.x + sub.x, max(info.z, 1u) - 1u);
    return src_p;
}

fragment float4 unpack_msaa_fragment(VertexOut in [[stage_in]],
                                     texture2d_ms<float> src [[texture(0)]],
                                     constant uint4& info [[buffer(0)]]) {
    uint sid = 0u;
    const uint2 src_p = msaa_unpack_coord(in.position.xy, info, src.get_width(), src.get_height(), sid);
    return src.read(src_p, sid);
}

fragment uint4 unpack_msaa_fragment_u(VertexOut in [[stage_in]],
                                      texture2d_ms<uint> src [[texture(0)]],
                                      constant uint4& info [[buffer(0)]]) {
    uint sid = 0u;
    const uint2 src_p = msaa_unpack_coord(in.position.xy, info, src.get_width(), src.get_height(), sid);
    return src.read(src_p, sid);
}

fragment int4 unpack_msaa_fragment_i(VertexOut in [[stage_in]],
                                     texture2d_ms<int> src [[texture(0)]],
                                     constant uint4& info [[buffer(0)]]) {
    uint sid = 0u;
    const uint2 src_p = msaa_unpack_coord(in.position.xy, info, src.get_width(), src.get_height(), sid);
    return src.read(src_p, sid);
}

// Depth<->colour reinterprets, and depth-to-depth rescaling, for
// BlitImageHelper::Convert. A blit encoder cannot express either, it rejects
// copyFromTexture across the depth/colour boundary and cannot rescale.
//
// depth2d<float> reads the depth aspect, which is also right for a packed
// Depth32Float_Stencil8 view. Stencil rides along untouched, Metal has no
// stencil export.
fragment float4 depth_to_color(VertexOut in [[stage_in]],
                               depth2d<float> src [[texture(0)]],
                               sampler src_sampler [[sampler(0)]]) {
    return float4(src.sample(src_sampler, in.uv), 0.0, 0.0, 1.0);
}

struct DepthOut {
    float depth [[depth(any)]];
};

fragment DepthOut color_to_depth(VertexOut in [[stage_in]],
                                 texture2d<float> src [[texture(0)]],
                                 sampler src_sampler [[sampler(0)]]) {
    return DepthOut{src.sample(src_sampler, in.uv).r};
}

fragment DepthOut depth_to_depth(VertexOut in [[stage_in]],
                                 depth2d<float> src [[texture(0)]],
                                 sampler src_sampler [[sampler(0)]]) {
    return DepthOut{src.sample(src_sampler, in.uv)};
}

fragment DepthOut expand_depth_fragment(VertexOut in [[stage_in]],
                                        uint sample_id [[sample_id]],
                                        depth2d<float> src [[texture(0)]],
                                        constant int4& info [[buffer(0)]]) {
    return DepthOut{src.read(msaa_pack_coord(in.position.xy, sample_id, info, src.get_width(), src.get_height()))};
}

fragment DepthOut unpack_msaa_depth_fragment(VertexOut in [[stage_in]],
                                             depth2d_ms<float> src [[texture(0)]],
                                             constant uint4& info [[buffer(0)]]) {
    uint sid = 0u;
    const uint2 src_p = msaa_unpack_coord(in.position.xy, info, src.get_width(), src.get_height(), sid);
    return DepthOut{src.read(src_p, sid)};
}

vertex float4 clear_vertex(uint vid [[vertex_id]],
                           constant float& depth [[buffer(1)]]) {
    const float2 positions[3] = { float2(-1.0, -3.0), float2(-1.0, 1.0), float2(3.0, 1.0) };
    return float4(positions[vid], depth, 1.0);
}

// One variant per attachment index. A fragment may only write colour outputs
// the pipeline has attachments for, so a single function declaring all eight
// fails to build on the ordinary one-attachment pass.
fragment float4 clear_fragment0(constant float4& colour [[buffer(0)]]) {
    return colour;
}
struct ClearOut1 { float4 c [[color(1)]]; };
fragment ClearOut1 clear_fragment1(constant float4& colour [[buffer(0)]]) {
    return ClearOut1{colour};
}
struct ClearOut2 { float4 c [[color(2)]]; };
fragment ClearOut2 clear_fragment2(constant float4& colour [[buffer(0)]]) {
    return ClearOut2{colour};
}
struct ClearOut3 { float4 c [[color(3)]]; };
fragment ClearOut3 clear_fragment3(constant float4& colour [[buffer(0)]]) {
    return ClearOut3{colour};
}
struct ClearOut4 { float4 c [[color(4)]]; };
fragment ClearOut4 clear_fragment4(constant float4& colour [[buffer(0)]]) {
    return ClearOut4{colour};
}
struct ClearOut5 { float4 c [[color(5)]]; };
fragment ClearOut5 clear_fragment5(constant float4& colour [[buffer(0)]]) {
    return ClearOut5{colour};
}
struct ClearOut6 { float4 c [[color(6)]]; };
fragment ClearOut6 clear_fragment6(constant float4& colour [[buffer(0)]]) {
    return ClearOut6{colour};
}
struct ClearOut7 { float4 c [[color(7)]]; };
fragment ClearOut7 clear_fragment7(constant float4& colour [[buffer(0)]]) {
    return ClearOut7{colour};
}

fragment uint4 clear_fragment_u0(constant uint4& colour [[buffer(0)]]) { return colour; }
struct ClearOutU1 { uint4 c [[color(1)]]; };
fragment ClearOutU1 clear_fragment_u1(constant uint4& colour [[buffer(0)]]) { return ClearOutU1{colour}; }
struct ClearOutU2 { uint4 c [[color(2)]]; };
fragment ClearOutU2 clear_fragment_u2(constant uint4& colour [[buffer(0)]]) { return ClearOutU2{colour}; }
struct ClearOutU3 { uint4 c [[color(3)]]; };
fragment ClearOutU3 clear_fragment_u3(constant uint4& colour [[buffer(0)]]) { return ClearOutU3{colour}; }
struct ClearOutU4 { uint4 c [[color(4)]]; };
fragment ClearOutU4 clear_fragment_u4(constant uint4& colour [[buffer(0)]]) { return ClearOutU4{colour}; }
struct ClearOutU5 { uint4 c [[color(5)]]; };
fragment ClearOutU5 clear_fragment_u5(constant uint4& colour [[buffer(0)]]) { return ClearOutU5{colour}; }
struct ClearOutU6 { uint4 c [[color(6)]]; };
fragment ClearOutU6 clear_fragment_u6(constant uint4& colour [[buffer(0)]]) { return ClearOutU6{colour}; }
struct ClearOutU7 { uint4 c [[color(7)]]; };
fragment ClearOutU7 clear_fragment_u7(constant uint4& colour [[buffer(0)]]) { return ClearOutU7{colour}; }

fragment int4 clear_fragment_i0(constant int4& colour [[buffer(0)]]) { return colour; }
struct ClearOutI1 { int4 c [[color(1)]]; };
fragment ClearOutI1 clear_fragment_i1(constant int4& colour [[buffer(0)]]) { return ClearOutI1{colour}; }
struct ClearOutI2 { int4 c [[color(2)]]; };
fragment ClearOutI2 clear_fragment_i2(constant int4& colour [[buffer(0)]]) { return ClearOutI2{colour}; }
struct ClearOutI3 { int4 c [[color(3)]]; };
fragment ClearOutI3 clear_fragment_i3(constant int4& colour [[buffer(0)]]) { return ClearOutI3{colour}; }
struct ClearOutI4 { int4 c [[color(4)]]; };
fragment ClearOutI4 clear_fragment_i4(constant int4& colour [[buffer(0)]]) { return ClearOutI4{colour}; }
struct ClearOutI5 { int4 c [[color(5)]]; };
fragment ClearOutI5 clear_fragment_i5(constant int4& colour [[buffer(0)]]) { return ClearOutI5{colour}; }
struct ClearOutI6 { int4 c [[color(6)]]; };
fragment ClearOutI6 clear_fragment_i6(constant int4& colour [[buffer(0)]]) { return ClearOutI6{colour}; }
struct ClearOutI7 { int4 c [[color(7)]]; };
fragment ClearOutI7 clear_fragment_i7(constant int4& colour [[buffer(0)]]) { return ClearOutI7{colour}; }

// Depth and stencil only. A clear that touches neither colour nor a colour
// attachment still needs a fragment stage.
fragment void clear_fragment_none() {
}

// Re-interleaves a depth plane (32-bit floats) and an optional stencil plane (bytes)
// into the guest's depth/stencil layout.
// info = (width, height, dst_row_texels, plane_row_texels); layout = DepthPackLayout.
kernel void pack_d24s8(device const float* depth [[buffer(0)]],
                       device const uchar* stencil [[buffer(1)]],
                       device uint* dst [[buffer(2)]],
                       constant uint4& info [[buffer(3)]],
                       constant uint& layout [[buffer(4)]],
                       uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= info.x || gid.y >= info.y) {
        return;
    }
    const uint plane_index = gid.y * info.w + gid.x;
    const uint dst_index = gid.y * info.z + gid.x;
    if (layout == 3u) {
        // D32_FLOAT_S8_UINT (Maxwell Z32_FLOAT_X24S8_UINT): [float32, u8 stencil + 3B pad].
        dst[dst_index * 2u] = as_type<uint>(depth[plane_index]);
        dst[dst_index * 2u + 1u] = uint(stencil[plane_index]);
        return;
    }
    // Saturated before scaling. A depth attachment can hold values outside
    // 0..1 and the guest's 24-bit field cannot, so wrapping would turn a
    // near-plane sliver into a far-plane one.
    const float d = clamp(depth[plane_index], 0.0f, 1.0f);
    const uint d24 = uint(d * 16777215.0 + 0.5) & 0xFFFFFFu;
    if (layout == 1u) {
        // D24_UNORM_S8_UINT (Maxwell S8Z24): depth in bits 0..23, stencil in 24..31.
        dst[dst_index] = d24 | (uint(stencil[plane_index]) << 24);
    } else if (layout == 2u) {
        // X8_D24_UNORM (Maxwell X8Z24): depth in bits 0..23, upper byte 0.
        dst[dst_index] = d24;
    } else {
        // S8_UINT_D24_UNORM (Maxwell Z24S8): depth in bits 8..31, stencil in 0..7.
        dst[dst_index] = (d24 << 8) | uint(stencil[plane_index]);
    }
}
)MSL";

enum class ColorFormatKind { Float, Uint, Sint };

bool IsDepthFormat(MTL::PixelFormat format) noexcept {
    switch (format) {
    case MTL::PixelFormatDepth16Unorm:
    case MTL::PixelFormatDepth32Float:
    case MTL::PixelFormatDepth24Unorm_Stencil8:
    case MTL::PixelFormatDepth32Float_Stencil8:
        return true;
    default:
        return false;
    }
}

bool IsStencilOnlyFormat(MTL::PixelFormat format) noexcept {
    switch (format) {
    case MTL::PixelFormatStencil8:
    case MTL::PixelFormatX32_Stencil8:
    case MTL::PixelFormatX24_Stencil8:
        return true;
    default:
        return false;
    }
}

bool HasStencilAspect(MTL::PixelFormat format) noexcept {
    switch (format) {
    case MTL::PixelFormatDepth24Unorm_Stencil8:
    case MTL::PixelFormatDepth32Float_Stencil8:
    case MTL::PixelFormatStencil8:
    case MTL::PixelFormatX32_Stencil8:
    case MTL::PixelFormatX24_Stencil8:
        return true;
    default:
        return false;
    }
}

ColorFormatKind ClassifyColorFormat(MTL::PixelFormat format) noexcept {
    switch (format) {
    case MTL::PixelFormatR8Uint:
    case MTL::PixelFormatRG8Uint:
    case MTL::PixelFormatRGBA8Uint:
    case MTL::PixelFormatR16Uint:
    case MTL::PixelFormatRG16Uint:
    case MTL::PixelFormatRGBA16Uint:
    case MTL::PixelFormatR32Uint:
    case MTL::PixelFormatRG32Uint:
    case MTL::PixelFormatRGBA32Uint:
    case MTL::PixelFormatRGB10A2Uint:
        return ColorFormatKind::Uint;
    case MTL::PixelFormatR8Sint:
    case MTL::PixelFormatRG8Sint:
    case MTL::PixelFormatRGBA8Sint:
    case MTL::PixelFormatR16Sint:
    case MTL::PixelFormatRG16Sint:
    case MTL::PixelFormatRGBA16Sint:
    case MTL::PixelFormatR32Sint:
    case MTL::PixelFormatRG32Sint:
    case MTL::PixelFormatRGBA32Sint:
        return ColorFormatKind::Sint;
    default:
        return ColorFormatKind::Float;
    }
}

Owned<MTL::SamplerState> MakeSampler(const Device& device, MTL::SamplerMinMagFilter filter) {
    ScopedAutoreleasePool pool;
    MTL::SamplerDescriptor* descriptor = MTL::SamplerDescriptor::alloc()->init();
    descriptor->setMinFilter(filter);
    descriptor->setMagFilter(filter);
    descriptor->setMipFilter(MTL::SamplerMipFilterNotMipmapped);
    descriptor->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
    descriptor->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
    Owned<MTL::SamplerState> sampler = Adopt(device.GetDevice()->newSamplerState(descriptor));
    descriptor->release();
    return sampler;
}

} // Anonymous namespace

BlitImageHelper::BlitImageHelper(const Device& device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_} {
    ScopedAutoreleasePool pool;

    NS::Error* error = nullptr;
    MTL::CompileOptions* options = MTL::CompileOptions::alloc()->init();
    library = Adopt(device.GetDevice()->newLibrary(ToNSString(BLIT_MSL), options, &error));
    options->release();

    if (!library) {
        LOG_ERROR(Render_Metal, "Failed to compile the blit shaders: {}", DescribeError(error));
        return;
    }
    library->setLabel(MTLSTR("nxemu blit"));

    sampler_linear = MakeSampler(device, MTL::SamplerMinMagFilterLinear);
    sampler_nearest = MakeSampler(device, MTL::SamplerMinMagFilterNearest);
}

BlitImageHelper::~BlitImageHelper() = default;

MTL::RenderPipelineState* BlitImageHelper::GetPipeline(MTL::PixelFormat format) {
    const u32 key = static_cast<u32>(format);
    if (const auto it = pipelines.find(key); it != pipelines.end()) {
        return it->second.get();
    }
    if (!library) {
        return nullptr;
    }

    ScopedAutoreleasePool pool;

    MTL4::RenderPipelineDescriptor* descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
    descriptor->setVertexFunctionDescriptor(MakeFunctionDescriptor(library.get(), "blit_vertex"));
    const ColorFormatKind kind = ClassifyColorFormat(format);
    const char* const frag_name = kind == ColorFormatKind::Uint   ? "blit_fragment_u"
                                  : kind == ColorFormatKind::Sint ? "blit_fragment_i"
                                                                  : "blit_fragment";
    descriptor->setFragmentFunctionDescriptor(MakeFunctionDescriptor(library.get(), frag_name));
    SetColorAttachment(descriptor, 0, format);

    NS::Error* error = nullptr;
    Owned<MTL::RenderPipelineState> pipeline =
        device.NewRenderPipelineState(descriptor, &error);
    descriptor->release();

    if (!pipeline) {
        LOG_ERROR(Render_Metal, "Failed to create a blit pipeline for format {}: {}",
                  static_cast<u32>(format), DescribeError(error));
        return nullptr;
    }
    LOG_DEBUG(Render_Metal, "Built blit pipeline for format {}", static_cast<u32>(format));
    return pipelines.emplace(key, std::move(pipeline)).first->second.get();
}


MTL::RenderPipelineState* BlitImageHelper::GetExpandPipeline(MTL::PixelFormat format,
                                                            u32 samples) {
    if (IsStencilOnlyFormat(format)) {
        // Metal fragment shaders have no stencil export.
        return nullptr;
    }
    const u32 key = static_cast<u32>(format) | (samples << 16);
    if (const auto it = expand_pipelines.find(key); it != expand_pipelines.end()) {
        return it->second.get();
    }
    if (!library) {
        return nullptr;
    }

    ScopedAutoreleasePool pool;
    MTL4::RenderPipelineDescriptor* descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
    descriptor->setVertexFunctionDescriptor(MakeFunctionDescriptor(library.get(), "blit_vertex"));
    if (IsDepthFormat(format)) {
        // Metal 4 depth-only render pipelines declare no color attachments; the render
        // pass supplies the depth/stencil format.
        descriptor->setFragmentFunctionDescriptor(
            MakeFunctionDescriptor(library.get(), "expand_depth_fragment"));
    } else {
        const ColorFormatKind kind = ClassifyColorFormat(format);
        const char* const frag_name = kind == ColorFormatKind::Uint   ? "expand_fragment_u"
                                      : kind == ColorFormatKind::Sint ? "expand_fragment_i"
                                                                      : "expand_fragment";
        descriptor->setFragmentFunctionDescriptor(MakeFunctionDescriptor(library.get(), frag_name));
        SetColorAttachment(descriptor, 0, format);
    }
    descriptor->setRasterSampleCount(samples);

    NS::Error* error = nullptr;
    Owned<MTL::RenderPipelineState> pipeline =
        device.NewRenderPipelineState(descriptor, &error);
    descriptor->release();
    if (!pipeline) {
        LOG_ERROR(Render_Metal, "Failed to create a {}x expand pipeline for format {}: {}", samples,
                  static_cast<u32>(format), DescribeError(error));
        return nullptr;
    }
    return expand_pipelines.emplace(key, std::move(pipeline)).first->second.get();
}

MTL::RenderPipelineState* BlitImageHelper::GetUnpackPipeline(MTL::PixelFormat format) {
    if (IsStencilOnlyFormat(format)) {
        return nullptr;
    }
    const u32 key = static_cast<u32>(format);
    if (const auto it = unpack_pipelines.find(key); it != unpack_pipelines.end()) {
        return it->second.get();
    }
    if (!library) {
        return nullptr;
    }

    ScopedAutoreleasePool pool;
    MTL4::RenderPipelineDescriptor* descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
    descriptor->setVertexFunctionDescriptor(MakeFunctionDescriptor(library.get(), "blit_vertex"));
    if (IsDepthFormat(format)) {
        descriptor->setFragmentFunctionDescriptor(
            MakeFunctionDescriptor(library.get(), "unpack_msaa_depth_fragment"));
    } else {
        const ColorFormatKind kind = ClassifyColorFormat(format);
        const char* const frag_name = kind == ColorFormatKind::Uint   ? "unpack_msaa_fragment_u"
                                      : kind == ColorFormatKind::Sint ? "unpack_msaa_fragment_i"
                                                                      : "unpack_msaa_fragment";
        descriptor->setFragmentFunctionDescriptor(MakeFunctionDescriptor(library.get(), frag_name));
        SetColorAttachment(descriptor, 0, format);
    }
    descriptor->setRasterSampleCount(1);

    NS::Error* error = nullptr;
    Owned<MTL::RenderPipelineState> pipeline =
        device.NewRenderPipelineState(descriptor, &error);
    descriptor->release();
    if (!pipeline) {
        LOG_ERROR(Render_Metal, "Failed to create an MSAA unpack pipeline for format {}: {}",
                  static_cast<u32>(format), DescribeError(error));
        return nullptr;
    }
    return unpack_pipelines.emplace(key, std::move(pipeline)).first->second.get();
}

void BlitImageHelper::UnpackFromMultisample(MTL::Texture* dst, MTL::Texture* src) {
    if (dst == nullptr || src == nullptr) {
        return;
    }
    const MTL::PixelFormat format = dst->pixelFormat();
    const bool is_depth = IsDepthFormat(format);
    MTL::RenderPipelineState* pipeline = GetUnpackPipeline(format);
    if (pipeline == nullptr) {
        return;
    }

    MTL::DepthStencilState* depth_state = nullptr;
    if (is_depth) {
        auto ds_it = convert_depth_states.find(1u);
        if (ds_it == convert_depth_states.end()) {
            ScopedAutoreleasePool ds_pool;
            MTL::DepthStencilDescriptor* ds = MTL::DepthStencilDescriptor::alloc()->init();
            ds->setDepthCompareFunction(MTL::CompareFunctionAlways);
            ds->setDepthWriteEnabled(true);
            Owned<MTL::DepthStencilState> state =
                Adopt(device.GetDevice()->newDepthStencilState(ds));
            ds->release();
            ds_it = convert_depth_states.emplace(1u, std::move(state)).first;
        }
        depth_state = ds_it->second.get();
    }

    const u32 src_w = (std::max)(1u, static_cast<u32>(src->width()));
    const u32 src_h = (std::max)(1u, static_cast<u32>(src->height()));
    const u32 dst_w = static_cast<u32>(dst->width());
    const u32 dst_h = static_cast<u32>(dst->height());
    const u32 scale_x = (std::max)(1u, dst_w / src_w);
    const u32 scale_y = (std::max)(1u, dst_h / src_h);
    const std::array<u32, 4> info{scale_x, scale_y, static_cast<u32>(src->sampleCount()), 0u};

    RenderPassState pass{};
    if (is_depth) {
        pass.depth_stencil.texture = dst;
        pass.depth_stencil.has_stencil = HasStencilAspect(format);
        pass.depth_stencil.depth_load = LoadAction::DontCare;
        pass.depth_stencil.depth_store = StoreAction::Store;
        pass.depth_stencil.stencil_load = LoadAction::Load;
        pass.depth_stencil.stencil_store = StoreAction::Store;
    } else {
        pass.num_color = 1;
        pass.color[0].texture = dst;
        pass.color[0].load_action = LoadAction::DontCare;
        pass.color[0].store_action = StoreAction::Store;
    }
    pass.render_area_width = dst_w;
    pass.render_area_height = dst_h;
    pass.sample_count = 1;

    scheduler.RequestRenderPass(pass);
    const std::array<f32, 4> rect{0.0f, 0.0f, 1.0f, 1.0f};
    src->retain();
    scheduler.Record([pipeline, depth_state, src, rect, info, width = dst_w,
                      height = dst_h](CommandContext& context) {
        RenderEncoder encoder = context.CurrentRender();
        if (encoder == nullptr) {
            src->release();
            return;
        }
        context.InvalidateRenderCache();
        MTL::Viewport viewport{};
        viewport.width = static_cast<double>(width);
        viewport.height = static_cast<double>(height);
        viewport.zfar = 1.0;
        encoder->setViewport(viewport);
        encoder->setRenderPipelineState(pipeline);
        if (depth_state != nullptr) {
            encoder->setDepthStencilState(depth_state);
        }
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setVertexBytes(rect.data(), rect.size() * sizeof(f32), 1);
        encoder->setFragmentBytes(info.data(), info.size() * sizeof(u32), 0);
        encoder->setFragmentTexture(src, 0);
        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        src->release();
    });
    scheduler.RequestOutsideRenderPassOperationContext();
}

void BlitImageHelper::ExpandToMultisample(MTL::Texture* dst, MTL::Texture* src) {
    if (dst == nullptr || src == nullptr) {
        return;
    }
    const MTL::PixelFormat format = dst->pixelFormat();
    const bool is_depth = IsDepthFormat(format);
    const u32 samples = static_cast<u32>(dst->sampleCount());
    MTL::RenderPipelineState* pipeline = GetExpandPipeline(format, samples);
    if (pipeline == nullptr) {
        return;
    }

    MTL::DepthStencilState* depth_state = nullptr;
    if (is_depth) {
        auto ds_it = convert_depth_states.find(1u);
        if (ds_it == convert_depth_states.end()) {
            ScopedAutoreleasePool ds_pool;
            MTL::DepthStencilDescriptor* ds = MTL::DepthStencilDescriptor::alloc()->init();
            ds->setDepthCompareFunction(MTL::CompareFunctionAlways);
            ds->setDepthWriteEnabled(true);
            Owned<MTL::DepthStencilState> state =
                Adopt(device.GetDevice()->newDepthStencilState(ds));
            ds->release();
            ds_it = convert_depth_states.emplace(1u, std::move(state)).first;
        }
        depth_state = ds_it->second.get();
    }

    const u32 dst_w = (std::max)(1u, static_cast<u32>(dst->width()));
    const u32 dst_h = (std::max)(1u, static_cast<u32>(dst->height()));
    const s32 scale_x = (std::max)(1, static_cast<s32>(src->width() / dst_w));
    const s32 scale_y = (std::max)(1, static_cast<s32>(src->height() / dst_h));
    const std::array<s32, 4> info{scale_x, scale_y, 0, 0};

    RenderPassState pass{};
    if (is_depth) {
        pass.depth_stencil.texture = dst;
        pass.depth_stencil.has_stencil = HasStencilAspect(format);
        pass.depth_stencil.depth_load = LoadAction::DontCare;
        pass.depth_stencil.depth_store = StoreAction::Store;
        pass.depth_stencil.stencil_load = LoadAction::Load;
        pass.depth_stencil.stencil_store = StoreAction::Store;
    } else {
        pass.num_color = 1;
        pass.color[0].texture = dst;
        // The triangle covers the whole attachment, so nothing needs loading.
        pass.color[0].load_action = LoadAction::DontCare;
        pass.color[0].store_action = StoreAction::Store;
    }
    pass.render_area_width = dst_w;
    pass.render_area_height = dst_h;
    pass.sample_count = samples;

    scheduler.RequestRenderPass(pass);
    const std::array<f32, 4> rect{0.0f, 0.0f, 1.0f, 1.0f};
    src->retain();
    scheduler.Record([pipeline, depth_state, src, rect, info, width = pass.render_area_width,
                      height = pass.render_area_height](CommandContext& context) {
        RenderEncoder encoder = context.CurrentRender();
        if (encoder == nullptr) {
            src->release();
            return;
        }
        // Binds straight onto the open encoder, bypassing RenderCache(), so the
        // draw path's idea of what is bound has to be dropped.
        context.InvalidateRenderCache();
        MTL::Viewport viewport{};
        viewport.width = static_cast<double>(width);
        viewport.height = static_cast<double>(height);
        viewport.zfar = 1.0;
        encoder->setViewport(viewport);
        encoder->setRenderPipelineState(pipeline);
        if (depth_state != nullptr) {
            encoder->setDepthStencilState(depth_state);
        }
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setVertexBytes(rect.data(), rect.size() * sizeof(f32), 1);
        encoder->setFragmentBytes(info.data(), info.size() * sizeof(s32), 0);
        encoder->setFragmentTexture(src, 0);
        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        src->release();
    });
    scheduler.RequestOutsideRenderPassOperationContext();
}

void BlitImageHelper::ExpandToMultisample(MTL::Texture* dst, MTL::Texture* src,
                                          std::span<const ExpandRegion> regions) {
    if (dst == nullptr || src == nullptr || regions.empty()) {
        return;
    }
    const MTL::PixelFormat format = dst->pixelFormat();
    const bool is_depth = IsDepthFormat(format);
    const u32 samples = static_cast<u32>(dst->sampleCount());
    MTL::RenderPipelineState* pipeline = GetExpandPipeline(format, samples);
    if (pipeline == nullptr) {
        return;
    }

    const float src_width = static_cast<float>(src->width());
    const float src_height = static_cast<float>(src->height());
    if (src_width <= 0.0f || src_height <= 0.0f) {
        return;
    }

    MTL::DepthStencilState* depth_state = nullptr;
    if (is_depth) {
        auto ds_it = convert_depth_states.find(1u);
        if (ds_it == convert_depth_states.end()) {
            ScopedAutoreleasePool ds_pool;
            MTL::DepthStencilDescriptor* ds = MTL::DepthStencilDescriptor::alloc()->init();
            ds->setDepthCompareFunction(MTL::CompareFunctionAlways);
            ds->setDepthWriteEnabled(true);
            Owned<MTL::DepthStencilState> state =
                Adopt(device.GetDevice()->newDepthStencilState(ds));
            ds->release();
            ds_it = convert_depth_states.emplace(1u, std::move(state)).first;
        }
        depth_state = ds_it->second.get();
    }

    RenderPassState pass{};
    if (is_depth) {
        pass.depth_stencil.texture = dst;
        pass.depth_stencil.has_stencil = HasStencilAspect(format);
        // Loaded, NOT discarded. Only part of the attachment is written and the
        // rest has to survive.
        pass.depth_stencil.depth_load = LoadAction::Load;
        pass.depth_stencil.depth_store = StoreAction::Store;
        pass.depth_stencil.stencil_load = LoadAction::Load;
        pass.depth_stencil.stencil_store = StoreAction::Store;
    } else {
        pass.num_color = 1;
        pass.color[0].texture = dst;
        // Loaded, NOT discarded. Only part of the attachment is written and the
        // rest has to survive.
        pass.color[0].load_action = LoadAction::Load;
        pass.color[0].store_action = StoreAction::Store;
    }
    pass.render_area_width = static_cast<u32>(dst->width());
    pass.render_area_height = static_cast<u32>(dst->height());
    pass.sample_count = samples;

    const u32 dst_w = (std::max)(1u, static_cast<u32>(dst->width()));
    const u32 dst_h = (std::max)(1u, static_cast<u32>(dst->height()));
    const s32 scale_x = (std::max)(1, static_cast<s32>(src->width() / dst_w));
    const s32 scale_y = (std::max)(1, static_cast<s32>(src->height() / dst_h));

    boost::container::small_vector<ExpandRegion, 8> owned(regions.begin(), regions.end());

    scheduler.RequestRenderPass(pass);
    src->retain();
    scheduler.Record([pipeline, depth_state, src, src_width, src_height, scale_x, scale_y,
                      regions = std::move(owned)](CommandContext& context) {
        RenderEncoder encoder = context.CurrentRender();
        if (encoder == nullptr) {
            src->release();
            return;
        }
        // Binds straight onto the open encoder, bypassing RenderCache().
        context.InvalidateRenderCache();
        encoder->setRenderPipelineState(pipeline);
        if (depth_state != nullptr) {
            encoder->setDepthStencilState(depth_state);
        }
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setFragmentTexture(src, 0);

        for (const ExpandRegion& region : regions) {
            if (region.width == 0 || region.height == 0) {
                continue;
            }
            // The triangle always covers clip space. The viewport is what puts
            // it over this region, and the scissor is what keeps the oversized
            // corners of it from reaching the rest of the attachment.
            MTL::Viewport viewport{};
            viewport.originX = static_cast<double>(region.dst_x);
            viewport.originY = static_cast<double>(region.dst_y);
            viewport.width = static_cast<double>(region.width);
            viewport.height = static_cast<double>(region.height);
            viewport.zfar = 1.0;
            encoder->setViewport(viewport);

            MTL::ScissorRect scissor{};
            scissor.x = region.dst_x;
            scissor.y = region.dst_y;
            scissor.width = region.width;
            scissor.height = region.height;
            encoder->setScissorRect(scissor);

            const std::array<f32, 4> rect{
                static_cast<f32>(region.src_x) / src_width,
                static_cast<f32>(region.src_y) / src_height,
                static_cast<f32>(region.src_x + region.width) / src_width,
                static_cast<f32>(region.src_y + region.height) / src_height,
            };
            const std::array<s32, 4> info{
                scale_x,
                scale_y,
                static_cast<s32>(region.src_x) - static_cast<s32>(region.dst_x) * scale_x,
                static_cast<s32>(region.src_y) - static_cast<s32>(region.dst_y) * scale_y,
            };
            encoder->setVertexBytes(rect.data(), rect.size() * sizeof(f32), 1);
            encoder->setFragmentBytes(info.data(), info.size() * sizeof(s32), 0);
            encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        }
        src->release();
    });
    scheduler.RequestOutsideRenderPassOperationContext();
}

MTL::ComputePipelineState* BlitImageHelper::GetPackDepthStencilPipeline() {
    if (pack_depth_stencil_pipeline) {
        return pack_depth_stencil_pipeline.get();
    }
    if (!library) {
        return nullptr;
    }
    pack_depth_stencil_pipeline =
        BuildComputePipeline(device, library.get(), "pack_d24s8", "nxemu pack D24S8");
    return pack_depth_stencil_pipeline.get();
}

void BlitImageHelper::PackDepthStencil(MTL::Buffer* dst, u64 dst_offset, u32 dst_row_texels,
                                       MTL::Buffer* planes, u64 depth_offset, u64 stencil_offset,
                                       u32 plane_row_texels, u32 width, u32 height,
                                       DepthPackLayout layout) {
    if (dst == nullptr || planes == nullptr || width == 0 || height == 0) {
        return;
    }
    MTL::ComputePipelineState* pipeline = GetPackDepthStencilPipeline();
    if (pipeline == nullptr) {
        return;
    }

    const std::array<u32, 4> info{width, height, dst_row_texels, plane_row_texels};
    const u32 layout_u32 = static_cast<u32>(layout);
    scheduler.Record([pipeline, dst, dst_offset, planes, depth_offset, stencil_offset, info,
                      layout_u32, width, height](CommandContext& context) {
        ComputeEncoder encoder = context.RequestCompute();
        if (encoder == nullptr) {
            return;
        }
        encoder->setComputePipelineState(pipeline);
        encoder->setBuffer(planes, depth_offset, 0);
        encoder->setBuffer(planes, stencil_offset, 1);
        encoder->setBuffer(dst, dst_offset, 2);
        encoder->setBytes(info.data(), info.size() * sizeof(u32), 3);
        encoder->setBytes(&layout_u32, sizeof(layout_u32), 4);

        // dispatchThreads rather than threadgroups. The kernel bounds-checks
        // against width/height, so a non-multiple extent needs no padding
        // arithmetic here.
        const NS::UInteger group_width =
            (std::min)(static_cast<NS::UInteger>(pipeline->threadExecutionWidth()),
                       static_cast<NS::UInteger>(width));
        const NS::UInteger group_height =
            (std::max)(NS::UInteger{1},
                       static_cast<NS::UInteger>(pipeline->maxTotalThreadsPerThreadgroup()) /
                           (std::max)(NS::UInteger{1}, group_width));
        encoder->dispatchThreads(MTL::Size::Make(width, height, 1),
                                 MTL::Size::Make(group_width, group_height, 1));
    });
}

MTL::RenderPipelineState* BlitImageHelper::GetClearPipeline(const RenderPassState& pass,
                                                            u32 color_target, bool clear_color,
                                                            u32 color_mask) {
    // Keyed on everything the pipeline bakes in, which attachment is written and
    // the formats of every attachment the pass declares.
    //
    // Hashed rather than bit-packed. Packing eight formats at a 6-bit stride
    // overlapped them, MTL::PixelFormat runs past 600 and needs ten bits, so two
    // passes with different attachment formats could share one pipeline.
    u64 key = 1469598103934665603ull;
    const auto mix = [&key](u64 value) { key = (key ^ value) * 1099511628211ull; };
    mix(color_target);
    mix(clear_color ? 1 : 0);
    mix(color_mask & 0xf);
    mix(pass.depth_stencil.texture != nullptr ? 1 : 0);
    mix(pass.sample_count);
    mix(pass.num_color);
    for (u32 i = 0; i < pass.num_color; ++i) {
        const MTL::PixelFormat format =
            pass.color[i].texture != nullptr ? pass.color[i].texture->pixelFormat()
                                             : MTL::PixelFormatInvalid;
        mix(static_cast<u64>(format));
    }
    if (const auto it = clear_pipelines.find(key); it != clear_pipelines.end()) {
        return it->second.get();
    }
    if (!library) {
        return nullptr;
    }

    ScopedAutoreleasePool pool;
    // Selected by which attachment is written and its scalar type (float/uint/sint).
    std::string fragment_name = "clear_fragment_none";
    if (clear_color) {
        const MTL::PixelFormat target_fmt =
            color_target < pass.num_color && pass.color[color_target].texture != nullptr
                ? pass.color[color_target].texture->pixelFormat()
                : MTL::PixelFormatInvalid;
        const ColorFormatKind kind = ClassifyColorFormat(target_fmt);
        const char* const prefix = kind == ColorFormatKind::Uint   ? "clear_fragment_u"
                                   : kind == ColorFormatKind::Sint ? "clear_fragment_i"
                                                                   : "clear_fragment";
        fragment_name = std::string{prefix} + std::to_string(color_target);
    }

    MTL4::RenderPipelineDescriptor* descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
    descriptor->setVertexFunctionDescriptor(MakeFunctionDescriptor(library.get(), "clear_vertex"));
    descriptor->setFragmentFunctionDescriptor(MakeFunctionDescriptor(library.get(), fragment_name));
    descriptor->setRasterSampleCount(pass.sample_count);

    for (u32 i = 0; i < pass.num_color; ++i) {
        if (pass.color[i].texture == nullptr) {
            continue;
        }
        MTL4::RenderPipelineColorAttachmentDescriptor* target =
            MTL4::RenderPipelineColorAttachmentDescriptor::alloc()->init();
        target->setPixelFormat(pass.color[i].texture->pixelFormat());
        // Only the attachment the guest asked for, and only the channels it
        // asked for. The rest are declared so the pipeline matches the pass,
        // but masked off entirely.
        target->setWriteMask(clear_color && i == color_target
                                 ? static_cast<MTL::ColorWriteMask>(color_mask)
                                 : MTL::ColorWriteMaskNone);
        descriptor->colorAttachments()->setObject(target, i);
        target->release();
    }
    // No depth or stencil format, Metal 4 pipelines do not declare one and the
    // render pass establishes it.

    NS::Error* error = nullptr;
    Owned<MTL::RenderPipelineState> pipeline =
        device.NewRenderPipelineState(descriptor, &error);
    descriptor->release();
    if (!pipeline) {
        LOG_ERROR(Render_Metal, "Failed to create a clear pipeline: {}", DescribeError(error));
        return nullptr;
    }
    return clear_pipelines.emplace(key, std::move(pipeline)).first->second.get();
}

void BlitImageHelper::ClearAttachments(const RenderPassState& pass, u32 color_target,
                                       bool clear_color, bool clear_depth, bool clear_stencil,
                                       const std::array<u32, 4>& raw_color, f32 depth, u32 stencil,
                                       s32 left, s32 top, s32 right, s32 bottom, u32 color_mask,
                                       u32 stencil_mask, MTL::Buffer* indirect_buffer,
                                       u32 indirect_offset) {
    if (right <= left || bottom <= top) {
        return;
    }
    MTL::RenderPipelineState* pipeline =
        GetClearPipeline(pass, color_target, clear_color, color_mask);
    if (pipeline == nullptr) {
        return;
    }
    // NOT calling scheduler.MarkDepthWritten() on purpose, here or in Convert.
    // It would let RequestFeedbackBarrier break the render pass on every clear,
    // and each break is a full tile store and reload on Apple GPUs, which
    // measures. The cost is that a shader sampling depth in a pass that cleared
    // it can read pre-clear contents.

    // Depth and stencil are written by the same draw. Always-compare with depth
    // writes on is what makes it a clear instead of a test.
    const u32 ds_key =
        (clear_depth ? 1u : 0u) | (clear_stencil ? 2u : 0u) | ((stencil_mask & 0xffu) << 2);
    auto ds_it = clear_depth_states.find(ds_key);
    if (ds_it == clear_depth_states.end()) {
        ScopedAutoreleasePool pool;
        MTL::DepthStencilDescriptor* ds = MTL::DepthStencilDescriptor::alloc()->init();
        ds->setDepthCompareFunction(MTL::CompareFunctionAlways);
        ds->setDepthWriteEnabled(clear_depth);
        if (clear_stencil) {
            MTL::StencilDescriptor* face = MTL::StencilDescriptor::alloc()->init();
            face->setStencilCompareFunction(MTL::CompareFunctionAlways);
            face->setDepthStencilPassOperation(MTL::StencilOperationReplace);
            // The guest's stencil write mask, so a masked clear leaves the
            // other bit planes alone.
            face->setWriteMask(stencil_mask);
            ds->setFrontFaceStencil(face);
            ds->setBackFaceStencil(face);
            face->release();
        }
        Owned<MTL::DepthStencilState> state = Adopt(device.GetDevice()->newDepthStencilState(ds));
        ds->release();
        ds_it = clear_depth_states.emplace(ds_key, std::move(state)).first;
    }
    MTL::DepthStencilState* depth_state = ds_it->second.get();

    const u32 width = pass.render_area_width;
    const u32 height = pass.render_area_height;

    scheduler.Record([pipeline, depth_state, raw_color, depth, stencil, left, top, right, bottom,
                      width, height, indirect_buffer, indirect_offset](CommandContext& context) {
        RenderEncoder encoder = context.CurrentRender();
        if (encoder == nullptr) {
            return;
        }
        // Binds straight onto the open encoder, bypassing RenderCache(), so the
        // draw path's idea of what is bound has to be dropped.
        context.InvalidateRenderCache();

        MTL::Viewport viewport{};
        viewport.originX = 0.0;
        viewport.originY = 0.0;
        viewport.width = static_cast<double>(width);
        viewport.height = static_cast<double>(height);
        viewport.znear = 0.0;
        viewport.zfar = 1.0;
        encoder->setViewport(viewport);

        MTL::ScissorRect scissor{};
        scissor.x = static_cast<NS::UInteger>(left);
        scissor.y = static_cast<NS::UInteger>(top);
        scissor.width = static_cast<NS::UInteger>(right - left);
        scissor.height = static_cast<NS::UInteger>(bottom - top);
        encoder->setScissorRect(scissor);

        // Never count the fullscreen clear triangle into an active occlusion query.
        encoder->setVisibilityResultMode(MTL::VisibilityResultModeDisabled, 0);
        encoder->setRenderPipelineState(pipeline);
        encoder->setDepthStencilState(depth_state);
        encoder->setStencilReferenceValue(stencil);
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setVertexBytes(&depth, sizeof(depth), 1);
        encoder->setFragmentBytes(raw_color.data(), raw_color.size() * sizeof(u32), 0);
        if (indirect_buffer != nullptr) {
            encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, indirect_buffer, indirect_offset);
        } else {
            encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger{0}, NS::UInteger{3});
        }
    });
    // This one leaves the pass open, unlike the two helpers above that follow
    // with RequestOutsideRenderPassOperationContext. Without this the attachment
    // has been written but pass_drew stays false, so a later feedback barrier or
    // colour feedback-loop break gets skipped and the guest samples a target it
    // just drew into. Only DrawImpl marked this.
    scheduler.MarkPassDrawn();
}


MTL::RenderPipelineState* BlitImageHelper::GetConvertPipeline(const RenderPassState& pass,
                                                              ConvertKind kind) {
    const MTL::PixelFormat color_format =
        pass.num_color > 0 && pass.color[0].texture != nullptr
            ? pass.color[0].texture->pixelFormat()
            : MTL::PixelFormatInvalid;
    const MTL::PixelFormat depth_format = pass.depth_stencil.texture != nullptr
                                              ? pass.depth_stencil.texture->pixelFormat()
                                              : MTL::PixelFormatInvalid;
    const u64 key = (static_cast<u64>(kind) << 48) ^ (static_cast<u64>(pass.sample_count) << 40) ^
                    (static_cast<u64>(color_format) << 20) ^ static_cast<u64>(depth_format);
    if (const auto it = convert_pipelines.find(key); it != convert_pipelines.end()) {
        return it->second.get();
    }
    if (!library) {
        return nullptr;
    }

    const char* fragment_name = kind == ConvertKind::DepthToColor  ? "depth_to_color"
                                : kind == ConvertKind::ColorToDepth ? "color_to_depth"
                                                                    : "depth_to_depth";

    ScopedAutoreleasePool pool;
    // The destination has to exist whichever direction the conversion runs.
    if (kind == ConvertKind::DepthToColor ? color_format == MTL::PixelFormatInvalid
                                          : depth_format == MTL::PixelFormatInvalid) {
        return nullptr;
    }

    MTL4::RenderPipelineDescriptor* descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
    descriptor->setVertexFunctionDescriptor(MakeFunctionDescriptor(library.get(), "blit_vertex"));
    descriptor->setFragmentFunctionDescriptor(MakeFunctionDescriptor(library.get(), fragment_name));
    descriptor->setRasterSampleCount(pass.sample_count);
    // Only the colour attachment is declared; Metal 4 pipelines do not carry a
    // depth or stencil format at all - the render pass establishes it.
    if (color_format != MTL::PixelFormatInvalid) {
        SetColorAttachment(descriptor, 0, color_format);
    }

    NS::Error* error = nullptr;
    Owned<MTL::RenderPipelineState> pipeline =
        device.NewRenderPipelineState(descriptor, &error);
    descriptor->release();
    if (!pipeline) {
        LOG_ERROR(Render_Metal, "Failed to create a conversion pipeline: {}",
                  DescribeError(error));
        return nullptr;
    }
    return convert_pipelines.emplace(key, std::move(pipeline)).first->second.get();
}

void BlitImageHelper::Convert(const RenderPassState& pass, MTL::Texture* src, ConvertKind kind,
                              const VideoCommon::Region2D& dst_region,
                              const VideoCommon::Region2D& src_region, bool linear) {
    if (src == nullptr) {
        return;
    }
    MTL::RenderPipelineState* pipeline = GetConvertPipeline(pass, kind);
    if (pipeline == nullptr) {
        return;
    }

    const f32 src_width = static_cast<f32>(src->width());
    const f32 src_height = static_cast<f32>(src->height());
    if (src_width <= 0.0f || src_height <= 0.0f) {
        return;
    }
    BlitRect rect{
        .left = static_cast<f32>(src_region.start.x) / src_width,
        .top = static_cast<f32>(src_region.start.y) / src_height,
        .right = static_cast<f32>(src_region.end.x) / src_width,
        .bottom = static_cast<f32>(src_region.end.y) / src_height,
    };
    if (dst_region.start.x > dst_region.end.x) {
        std::swap(rect.left, rect.right);
    }
    if (dst_region.start.y > dst_region.end.y) {
        std::swap(rect.top, rect.bottom);
    }

    // Writing depth needs a state that always passes and writes; the colour
    // direction must not touch depth at all. MarkDepthWritten is skipped here
    // for the reason given in ClearAttachments.
    const bool writes_depth = kind != ConvertKind::DepthToColor;
    const u32 ds_key = writes_depth ? 1u : 0u;
    auto ds_it = convert_depth_states.find(ds_key);
    if (ds_it == convert_depth_states.end()) {
        ScopedAutoreleasePool ds_pool;
        MTL::DepthStencilDescriptor* ds = MTL::DepthStencilDescriptor::alloc()->init();
        ds->setDepthCompareFunction(MTL::CompareFunctionAlways);
        ds->setDepthWriteEnabled(writes_depth);
        Owned<MTL::DepthStencilState> state = Adopt(device.GetDevice()->newDepthStencilState(ds));
        ds->release();
        ds_it = convert_depth_states.emplace(ds_key, std::move(state)).first;
    }
    MTL::DepthStencilState* depth_state = ds_it->second.get();

    MTL::SamplerState* sampler = linear ? sampler_linear.get() : sampler_nearest.get();

    const s32 dst_x = std::min(dst_region.start.x, dst_region.end.x);
    const s32 dst_y = std::min(dst_region.start.y, dst_region.end.y);
    const s32 dst_w = std::abs(dst_region.end.x - dst_region.start.x);
    const s32 dst_h = std::abs(dst_region.end.y - dst_region.start.y);
    if (dst_w <= 0 || dst_h <= 0) {
        return;
    }

    RenderPassState target = pass;
    // Loaded, never discarded: a conversion may cover only part of the
    // destination and the rest has to survive.
    for (u32 i = 0; i < target.num_color; ++i) {
        target.color[i].load_action = LoadAction::Load;
        target.color[i].store_action = StoreAction::Store;
    }
    target.depth_stencil.depth_load = LoadAction::Load;
    target.depth_stencil.depth_store = StoreAction::Store;
    target.depth_stencil.stencil_load = LoadAction::Load;
    target.depth_stencil.stencil_store = StoreAction::Store;

    const u32 area_width = target.render_area_width;
    const u32 area_height = target.render_area_height;
    scheduler.RequestRenderPass(target);
    scheduler.Record([pipeline, depth_state, src, sampler, rect, dst_x, dst_y, dst_w, dst_h,
                      area_width, area_height](CommandContext& context) {
        RenderEncoder encoder = context.CurrentRender();
        if (encoder == nullptr) {
            return;
        }
        // Binds straight onto the open encoder, bypassing RenderCache(), so the
        // draw path's idea of what is bound has to be dropped.
        context.InvalidateRenderCache();
        MTL::Viewport viewport{};
        viewport.originX = static_cast<double>(dst_x);
        viewport.originY = static_cast<double>(dst_y);
        viewport.width = static_cast<double>(dst_w);
        viewport.height = static_cast<double>(dst_h);
        viewport.znear = 0.0;
        viewport.zfar = 1.0;
        encoder->setViewport(viewport);

        const s64 max_w = static_cast<s64>(area_width);
        const s64 max_h = static_cast<s64>(area_height);
        const s64 left = std::clamp<s64>(dst_x, 0, max_w);
        const s64 top = std::clamp<s64>(dst_y, 0, max_h);
        const s64 right = std::clamp<s64>(static_cast<s64>(dst_x) + dst_w, left, max_w);
        const s64 bottom = std::clamp<s64>(static_cast<s64>(dst_y) + dst_h, top, max_h);
        if (right <= left || bottom <= top) {
            return;
        }
        MTL::ScissorRect scissor{};
        scissor.x = static_cast<NS::UInteger>(left);
        scissor.y = static_cast<NS::UInteger>(top);
        scissor.width = static_cast<NS::UInteger>(right - left);
        scissor.height = static_cast<NS::UInteger>(bottom - top);
        encoder->setScissorRect(scissor);

        encoder->setRenderPipelineState(pipeline);
        encoder->setDepthStencilState(depth_state);
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setVertexBytes(&rect, sizeof(rect), BLIT_UNIFORM_INDEX);
        encoder->setFragmentTexture(src, BLIT_TEXTURE_INDEX);
        encoder->setFragmentSamplerState(sampler, BLIT_SAMPLER_INDEX);
        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger{0}, NS::UInteger{3});
    });
    // Leaves the pass open; see the note in the depth-clear helper above.
    scheduler.MarkPassDrawn();
}

void BlitImageHelper::BlitColor(MTL::Texture* dst, MTL::Texture* src,
                                const VideoCommon::Region2D& dst_region,
                                const VideoCommon::Region2D& src_region,
                                Tegra::Engines::Fermi2D::Filter filter,
                                const std::optional<MTL::ScissorRect>& scissor_clip,
                                MTL::SamplerState* override_sampler) {
    if (dst == nullptr || src == nullptr) {
        return;
    }

    MTL::RenderPipelineState* pipeline = GetPipeline(dst->pixelFormat());
    if (pipeline == nullptr) {
        return;
    }

    const f32 src_width = static_cast<f32>(src->width());
    const f32 src_height = static_cast<f32>(src->height());
    if (src_width <= 0.0f || src_height <= 0.0f) {
        return;
    }

    // A start greater than an end on either region flips that axis; mix() in the
    // shader handles the reversed UV range.
    BlitRect rect{
        .left = static_cast<f32>(src_region.start.x) / src_width,
        .top = static_cast<f32>(src_region.start.y) / src_height,
        .right = static_cast<f32>(src_region.end.x) / src_width,
        .bottom = static_cast<f32>(src_region.end.y) / src_height,
    };
    if (dst_region.start.x > dst_region.end.x) {
        std::swap(rect.left, rect.right);
    }
    if (dst_region.start.y > dst_region.end.y) {
        std::swap(rect.top, rect.bottom);
    }

    // The destination region becomes the viewport, so the full-screen triangle
    // lands exactly on it without needing per-blit vertex positions.
    const s32 dst_x = std::min(dst_region.start.x, dst_region.end.x);
    const s32 dst_y = std::min(dst_region.start.y, dst_region.end.y);
    const s32 dst_w = std::abs(dst_region.end.x - dst_region.start.x);
    const s32 dst_h = std::abs(dst_region.end.y - dst_region.start.y);
    if (dst_w <= 0 || dst_h <= 0) {
        return;
    }

    const s64 area_width = static_cast<s64>(dst->width());
    const s64 area_height = static_cast<s64>(dst->height());
    s64 clip_left = std::clamp<s64>(dst_x, 0, area_width);
    s64 clip_top = std::clamp<s64>(dst_y, 0, area_height);
    s64 clip_right = std::clamp<s64>(static_cast<s64>(dst_x) + dst_w, clip_left, area_width);
    s64 clip_bottom = std::clamp<s64>(static_cast<s64>(dst_y) + dst_h, clip_top, area_height);
    if (scissor_clip) {
        const s64 sc_left = static_cast<s64>(scissor_clip->x);
        const s64 sc_top = static_cast<s64>(scissor_clip->y);
        const s64 sc_right = sc_left + static_cast<s64>(scissor_clip->width);
        const s64 sc_bottom = sc_top + static_cast<s64>(scissor_clip->height);
        clip_left = std::max(clip_left, sc_left);
        clip_top = std::max(clip_top, sc_top);
        clip_right = std::min(clip_right, sc_right);
        clip_bottom = std::min(clip_bottom, sc_bottom);
    }
    if (clip_right <= clip_left || clip_bottom <= clip_top) {
        return;
    }

    MTL::ScissorRect scissor{};
    scissor.x = static_cast<NS::UInteger>(clip_left);
    scissor.y = static_cast<NS::UInteger>(clip_top);
    scissor.width = static_cast<NS::UInteger>(clip_right - clip_left);
    scissor.height = static_cast<NS::UInteger>(clip_bottom - clip_top);

    MTL::SamplerState* sampler = override_sampler != nullptr
                                     ? override_sampler
                                     : (filter == Tegra::Engines::Fermi2D::Filter::Bilinear
                                            ? sampler_linear.get()
                                            : sampler_nearest.get());
    const bool bind_sampler = ClassifyColorFormat(dst->pixelFormat()) == ColorFormatKind::Float;

    RenderPassState pass{};
    pass.num_color = 1;
    pass.color[0].texture = dst;
    // Load, not DontCare: the blit may cover only part of the destination, and
    // discarding the rest would destroy content the guest still expects.
    pass.color[0].load_action = LoadAction::Load;
    pass.color[0].store_action = StoreAction::Store;
    pass.render_area_width = static_cast<u32>(dst->width());
    pass.render_area_height = static_cast<u32>(dst->height());

    // Opened on the worker rather than through Scheduler::RequestRenderPass, so
    // the recording-side mirror has to be told or every later request to end
    // the pass records nothing.
    scheduler.NotifyRenderPassOpened(pass);
    scheduler.Record([pipeline, src, sampler, bind_sampler, rect, dst_x, dst_y, dst_w, dst_h,
                      scissor, pass](CommandContext& context) {
        RenderEncoder encoder = context.RequestRender(pass);
        if (encoder == nullptr) {
            return;
        }
        context.InvalidateRenderCache();

        MTL::Viewport viewport{};
        viewport.originX = static_cast<double>(dst_x);
        viewport.originY = static_cast<double>(dst_y);
        viewport.width = static_cast<double>(dst_w);
        viewport.height = static_cast<double>(dst_h);
        viewport.znear = 0.0;
        viewport.zfar = 1.0;
        encoder->setViewport(viewport);
        encoder->setScissorRect(scissor);

        encoder->setVisibilityResultMode(MTL::VisibilityResultModeDisabled, 0);
        encoder->setRenderPipelineState(pipeline);
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setVertexBytes(&rect, sizeof(rect), BLIT_UNIFORM_INDEX);
        encoder->setFragmentTexture(src, BLIT_TEXTURE_INDEX);
        if (bind_sampler) {
            encoder->setFragmentSamplerState(sampler, BLIT_SAMPLER_INDEX);
        }
        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger{0}, NS::UInteger{3});
    });
    // Leaves the pass open; see the note in the depth-clear helper above.
    scheduler.MarkPassDrawn();
}

} // namespace Metal
