// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_video_core/fsr.h"
#include <bit>
#include "yuzu_video_core/area_sampling.h"
#include "yuzu_video_core/framebuffer_config.h"
#include "yuzu_video_core/present.h"
#include "yuzu_video_core/renderer_opengl/gl_blit_screen.h"
#include "yuzu_video_core/renderer_opengl/gl_rasterizer.h"
#include "yuzu_video_core/renderer_opengl/present/fsr.h"
#include "yuzu_video_core/renderer_opengl/present/fxaa.h"
#include "yuzu_video_core/renderer_opengl/present/layer.h"
#include "yuzu_video_core/renderer_opengl/present/present_uniforms.h"
#include "yuzu_video_core/renderer_opengl/present/smaa.h"
#include "yuzu_video_core/surface.h"
#include "yuzu_video_core/textures/decoders.h"
#include "video_settings.h"

namespace OpenGL {

Layer::Layer(RasterizerOpenGL& rasterizer_, Tegra::MaxwellDeviceMemoryManager& device_memory_,
             const PresentFilters& filters_)
    : rasterizer(rasterizer_), device_memory(device_memory_), filters(filters_) {
    // Allocate textures for the screen
    framebuffer_texture.resource.Create(GL_TEXTURE_2D);

    const GLuint texture = framebuffer_texture.resource.handle;
    glTextureStorage2D(texture, 1, GL_RGBA8, 1, 1);

    // Clear screen to black
    const u8 framebuffer_data[4] = {0, 0, 0, 0};
    glClearTexImage(framebuffer_texture.resource.handle, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                    framebuffer_data);
}

Layer::~Layer() = default;

GLuint Layer::ConfigureDraw(std::array<GLfloat, 3 * 2>& out_matrix,
                            std::array<ScreenRectVertex, 4>& out_vertices,
                            ProgramManager& program_manager,
                            const Tegra::FramebufferConfig& framebuffer,
                            const Layout::FramebufferLayout& layout, bool invert_y,
                            std::array<u32, 16>& easu_constants, ScalingFilter& effective_filter, ScalingFilter requested_filter) {
    easu_constants.fill(0);
    FramebufferTextureInfo info = PrepareRenderTarget(framebuffer);
    auto crop = Tegra::NormalizeCrop(framebuffer, info.width, info.height);
    VideoCore::NotePresentInput(info.scaled_width, info.scaled_height,
        static_cast<unsigned>(std::lround(std::abs(crop.right - crop.left) * info.scaled_width)),
        static_cast<unsigned>(std::lround(std::abs(crop.bottom - crop.top) * info.scaled_height)));
    GLuint texture = info.display_texture;

    auto anti_aliasing = filters.get_anti_aliasing();
    if (anti_aliasing != AntiAliasing::None) {
        glEnablei(GL_SCISSOR_TEST, 0);
        auto viewport_width = videoSettings.resolution_info.ScaleUp(framebuffer_texture.width);
        auto viewport_height = videoSettings.resolution_info.ScaleUp(framebuffer_texture.height);

        glScissorIndexed(0, 0, 0, viewport_width, viewport_height);
        glViewportIndexedf(0, 0.0f, 0.0f, static_cast<GLfloat>(viewport_width),
                           static_cast<GLfloat>(viewport_height));

        switch (anti_aliasing) {
        case AntiAliasing::Fxaa:
            CreateFXAA();
            texture = fxaa->Draw(program_manager, info.display_texture);
            break;
        case AntiAliasing::Cmaa2Intel:
            // Never misrepresent the OpenGL fallback as Intel CMAA2.
            break;
        case AntiAliasing::Smaa:
        case AntiAliasing::SmaaHigh:
        default:
            CreateSMAA(anti_aliasing != AntiAliasing::SmaaHigh);
            texture = smaa->Draw(program_manager, info.display_texture);
            break;
        }
    }

    glDisablei(GL_SCISSOR_TEST, 0);

    const auto selection = ResolvePresentScale(requested_filter,
        !filters.is_applet_capture && VideoCore::fsr_frame_state.downscale_filter == 1,
        info.scaled_width, info.scaled_height, crop, layout.screen.GetWidth(), layout.screen.GetHeight());
    const ScalingFilter scaling_filter = effective_filter = selection.filter;
    VideoCore::NotePresentSelection(static_cast<unsigned>(scaling_filter), static_cast<unsigned>(selection.mode));
    if (scaling_filter == ScalingFilter::Area) {
        const std::array<float, 4> area{
            std::abs(crop.right - crop.left) * info.scaled_width / layout.screen.GetWidth(),
            std::abs(crop.bottom - crop.top) * info.scaled_height / layout.screen.GetHeight(),
            1.0f / info.scaled_width, 1.0f / info.scaled_height};
        for (size_t i = 0; i < area.size(); ++i) easu_constants[i] = std::bit_cast<u32>(area[i]);
        easu_constants[4] = VideoCore::AreaSamplingGrid(info.scaled_width, info.scaled_height,
            crop.left, crop.top, crop.right, crop.bottom,
            layout.screen.GetWidth(), layout.screen.GetHeight());
    }
    const bool use_fsr = scaling_filter == ScalingFilter::Fsr ||
                         scaling_filter == ScalingFilter::FsrEasu;
    if (scaling_filter == ScalingFilter::CatmullRom || scaling_filter == ScalingFilter::CasCatmullRom ||
        scaling_filter == ScalingFilter::CatmullRomFast || scaling_filter == ScalingFilter::CasCatmullRomFast) {
        easu_constants[0] = CanUseNativeRcas(info.scaled_width, info.scaled_height, crop,
            layout.screen.GetWidth(), layout.screen.GetHeight()) ? 1U : 0U;
        if (scaling_filter == ScalingFilter::CatmullRomFast || scaling_filter == ScalingFilter::CasCatmullRomFast)
            easu_constants[0] |= 2U;
    }
    const bool skip_easu = !VideoCore::LegacyFsr() && use_fsr && CanUseNativeRcas(
        info.scaled_width, info.scaled_height, crop, layout.screen.GetWidth(),
        layout.screen.GetHeight());
    if (use_fsr) VideoCore::NoteFsrInput(info.scaled_width, info.scaled_height,
        VideoCore::LegacyFsr() ? 2U : (scaling_filter == ScalingFilter::Fsr && !skip_easu ? 1U : 0U));
    if (scaling_filter == ScalingFilter::Fsr && !skip_easu) {
        if (!fsr || fsr->NeedsRecreation(layout.screen, !VideoCore::LegacyFsr())) {
            fsr = std::make_unique<FSR>(layout.screen.GetWidth(), layout.screen.GetHeight(),
                                        !VideoCore::LegacyFsr());
        }
        texture = fsr->Draw(program_manager, texture, info.scaled_width, info.scaled_height, crop);
        crop = {0, 0, 1, 1};
    }

    if (scaling_filter == ScalingFilter::FsrEasu && !skip_easu) {
        const float input_width = static_cast<float>(info.scaled_width);
        const float input_height = static_cast<float>(info.scaled_height);
        ::FSR::FsrEasuConOffset(easu_constants.data(), easu_constants.data() + 4,
                                easu_constants.data() + 8, easu_constants.data() + 12,
                                (crop.right - crop.left) * input_width,
                                (crop.bottom - crop.top) * input_height,
                                input_width, input_height,
                                static_cast<float>(layout.screen.GetWidth()),
                                static_cast<float>(layout.screen.GetHeight()),
                                crop.left * input_width, crop.top * input_height);
    }

    out_matrix =
        MakeOrthographicMatrix(static_cast<float>(layout.width), static_cast<float>(layout.height));

    // Map the coordinates to the screen.
    const auto& screen = layout.screen;
    const auto x = screen.left;
    const auto y = screen.top;
    const auto w = screen.GetWidth();
    const auto h = screen.GetHeight();

    const auto left = crop.left;
    const auto right = crop.right;
    const auto top = invert_y ? crop.bottom : crop.top;
    const auto bottom = invert_y ? crop.top : crop.bottom;

    out_vertices[0] = ScreenRectVertex(x, y, left, top);
    out_vertices[1] = ScreenRectVertex(x + w, y, right, top);
    out_vertices[2] = ScreenRectVertex(x, y + h, left, bottom);
    out_vertices[3] = ScreenRectVertex(x + w, y + h, right, bottom);

    return texture;
}

FramebufferTextureInfo Layer::PrepareRenderTarget(const Tegra::FramebufferConfig& framebuffer) {
    // If framebuffer is provided, reload it from memory to a texture
    if (framebuffer_texture.width != static_cast<GLsizei>(framebuffer.width) ||
        framebuffer_texture.height != static_cast<GLsizei>(framebuffer.height) ||
        framebuffer_texture.pixel_format != framebuffer.pixel_format ||
        gl_framebuffer_data.empty()) {
        // Reallocate texture if the framebuffer size has changed.
        // This is expected to not happen very often and hence should not be a
        // performance problem.
        ConfigureFramebufferTexture(framebuffer);
    }

    // Load the framebuffer from memory if needed
    return LoadFBToScreenInfo(framebuffer);
}

FramebufferTextureInfo Layer::LoadFBToScreenInfo(const Tegra::FramebufferConfig& framebuffer) {
    const VAddr framebuffer_addr{framebuffer.address + framebuffer.offset};
    const auto accelerated_info =
        rasterizer.AccelerateDisplay(framebuffer, framebuffer_addr, framebuffer.stride);
    if (accelerated_info) {
        return *accelerated_info;
    }

    // Reset the screen info's display texture to its own permanent texture
    FramebufferTextureInfo info{};
    info.display_texture = framebuffer_texture.resource.handle;
    info.width = framebuffer.width;
    info.height = framebuffer.height;
    info.scaled_width = framebuffer.width;
    info.scaled_height = framebuffer.height;

    // TODO(Rodrigo): Read this from HLE
    constexpr u32 block_height_log2 = 4;
    const auto pixel_format{
        VideoCore::Surface::PixelFormatFromGPUPixelFormat(framebuffer.pixel_format)};
    const u32 bytes_per_pixel{VideoCore::Surface::BytesPerBlock(pixel_format)};
    const u64 size_in_bytes{Tegra::Texture::CalculateSize(
        true, bytes_per_pixel, framebuffer.stride, framebuffer.height, 1, block_height_log2, 0)};
    const u8* const host_ptr{device_memory.GetPointer<u8>(framebuffer_addr)};
    if (host_ptr) {
        const std::span<const u8> input_data(host_ptr, size_in_bytes);
        Tegra::Texture::UnswizzleTexture(gl_framebuffer_data, input_data, bytes_per_pixel,
                                         framebuffer.width, framebuffer.height, 1,
                                         block_height_log2, 0);
    }

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(framebuffer.stride));

    // Update existing texture
    // TODO: Test what happens on hardware when you change the framebuffer dimensions so that
    //       they differ from the LCD resolution.
    // TODO: Applications could theoretically crash yuzu here by specifying too large
    //       framebuffer sizes. We should make sure that this cannot happen.
    glTextureSubImage2D(framebuffer_texture.resource.handle, 0, 0, 0, framebuffer.width,
                        framebuffer.height, framebuffer_texture.gl_format,
                        framebuffer_texture.gl_type, gl_framebuffer_data.data());

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    return info;
}

void Layer::ConfigureFramebufferTexture(const Tegra::FramebufferConfig& framebuffer) {
    framebuffer_texture.width = framebuffer.width;
    framebuffer_texture.height = framebuffer.height;
    framebuffer_texture.pixel_format = framebuffer.pixel_format;

    const auto pixel_format{
        VideoCore::Surface::PixelFormatFromGPUPixelFormat(framebuffer.pixel_format)};
    const u32 bytes_per_pixel{VideoCore::Surface::BytesPerBlock(pixel_format)};
    gl_framebuffer_data.resize(framebuffer_texture.width * framebuffer_texture.height *
                               bytes_per_pixel);

    GLint internal_format;
    switch (framebuffer.pixel_format) {
    case Service::android::PixelFormat::Rgba8888:
        internal_format = GL_RGBA8;
        framebuffer_texture.gl_format = GL_RGBA;
        framebuffer_texture.gl_type = GL_UNSIGNED_INT_8_8_8_8_REV;
        break;
    case Service::android::PixelFormat::Rgb565:
        internal_format = GL_RGB565;
        framebuffer_texture.gl_format = GL_RGB;
        framebuffer_texture.gl_type = GL_UNSIGNED_SHORT_5_6_5;
        break;
    default:
        internal_format = GL_RGBA8;
        framebuffer_texture.gl_format = GL_RGBA;
        framebuffer_texture.gl_type = GL_UNSIGNED_INT_8_8_8_8_REV;
        // UNIMPLEMENTED_MSG("Unknown framebuffer pixel format: {}",
        //                   static_cast<u32>(framebuffer.pixel_format));
        break;
    }

    framebuffer_texture.resource.Release();
    framebuffer_texture.resource.Create(GL_TEXTURE_2D);
    glTextureStorage2D(framebuffer_texture.resource.handle, 1, internal_format,
                       framebuffer_texture.width, framebuffer_texture.height);

    fxaa.reset();
    smaa.reset();
}

void Layer::CreateFXAA() {
    smaa.reset();
    if (!fxaa) {
        fxaa = std::make_unique<FXAA>(
            videoSettings.resolution_info.ScaleUp(framebuffer_texture.width),
            videoSettings.resolution_info.ScaleUp(framebuffer_texture.height));
    }
}

void Layer::CreateSMAA(bool ultra) {
    fxaa.reset();
    if (!smaa || smaa_ultra != ultra) {
        smaa_ultra = ultra;
        smaa = std::make_unique<SMAA>(
            videoSettings.resolution_info.ScaleUp(framebuffer_texture.width),
            videoSettings.resolution_info.ScaleUp(framebuffer_texture.height), ultra);
    }
}

} // namespace OpenGL
