// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <memory>

#include <glad/glad.h>

#include "frontend/emu_window.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/microprofile.h"
#include "yuzu_common/settings.h"
#include "video_settings.h"
#include "yuzu_common/stb.h"
#include "yuzu_common/yuzu_assert.h"
#include "yuzu_video_core/capture.h"
#include "yuzu_video_core/present.h"
#include "yuzu_video_core/renderer_opengl/gl_blit_screen.h"
#include "yuzu_video_core/renderer_opengl/gl_rasterizer.h"
#include "yuzu_video_core/renderer_opengl/gl_shader_manager.h"
#include "yuzu_video_core/renderer_opengl/gl_shader_util.h"
#include "yuzu_video_core/renderer_opengl/renderer_opengl.h"
#include "yuzu_video_core/textures/decoders.h"
#include "yuzu_video_core/watermark.h"

namespace OpenGL
{
namespace
{
const char * GetSource(GLenum source)
{
    switch (source)
    {
    case GL_DEBUG_SOURCE_API:
        return "API";
    case GL_DEBUG_SOURCE_WINDOW_SYSTEM:
        return "WINDOW_SYSTEM";
    case GL_DEBUG_SOURCE_SHADER_COMPILER:
        return "SHADER_COMPILER";
    case GL_DEBUG_SOURCE_THIRD_PARTY:
        return "THIRD_PARTY";
    case GL_DEBUG_SOURCE_APPLICATION:
        return "APPLICATION";
    case GL_DEBUG_SOURCE_OTHER:
        return "OTHER";
    default:
        ASSERT(false);
        return "Unknown source";
    }
}

const char * GetType(GLenum type)
{
    switch (type)
    {
    case GL_DEBUG_TYPE_ERROR:
        return "ERROR";
    case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
        return "DEPRECATED_BEHAVIOR";
    case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
        return "UNDEFINED_BEHAVIOR";
    case GL_DEBUG_TYPE_PORTABILITY:
        return "PORTABILITY";
    case GL_DEBUG_TYPE_PERFORMANCE:
        return "PERFORMANCE";
    case GL_DEBUG_TYPE_OTHER:
        return "OTHER";
    case GL_DEBUG_TYPE_MARKER:
        return "MARKER";
    default:
        ASSERT(false);
        return "Unknown type";
    }
}

void APIENTRY DebugHandler(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar * message, const void * user_param)
{
    const char format[] = "{} {} {}: {}";
    const char * const str_source = GetSource(source);
    const char * const str_type = GetType(type);

    switch (severity)
    {
    case GL_DEBUG_SEVERITY_HIGH:
        LOG_CRITICAL(Render_OpenGL, format, str_source, str_type, id, message);
        break;
    case GL_DEBUG_SEVERITY_MEDIUM:
        LOG_WARNING(Render_OpenGL, format, str_source, str_type, id, message);
        break;
    case GL_DEBUG_SEVERITY_NOTIFICATION:
    case GL_DEBUG_SEVERITY_LOW:
        LOG_DEBUG(Render_OpenGL, format, str_source, str_type, id, message);
        break;
    }
}
} // Anonymous namespace

RendererOpenGL::RendererOpenGL(Core::Frontend::EmuWindow & emu_window_, Tegra::MaxwellDeviceMemoryManager & device_memory_, Tegra::GPU & gpu_, std::unique_ptr<Core::Frontend::GraphicsContext> context_) :
    RendererBase{emu_window_, std::move(context_)},
    emu_window{emu_window_}, device_memory{device_memory_}, gpu{gpu_}, device{emu_window_},
    state_tracker{}, program_manager{device},
    rasterizer(emu_window, gpu, device_memory, device, program_manager, state_tracker),
    watermark_texture(0), watermark_vao(0), watermark_vbo(0), watermark_shader(0), watermark_width(0), watermark_height(0),
    watermark_timer_started(false), watermark_show(true)
{
    if (videoSettings.renderer_debug && GLAD_GL_KHR_debug)
    {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(DebugHandler, nullptr);
    }
    AddTelemetryFields();

    // Initialize default attributes to match hardware's disabled attributes
    GLint max_attribs{};
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &max_attribs);
    for (GLint attrib = 0; attrib < max_attribs; ++attrib)
    {
        glVertexAttrib4f(attrib, 0.0f, 0.0f, 0.0f, 1.0f);
    }
    // Enable seamless cubemaps when per texture parameters are not available
    if (!GLAD_GL_ARB_seamless_cubemap_per_texture && !GLAD_GL_AMD_seamless_cubemap_per_texture)
    {
        glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    }

    // Enable unified vertex attributes when the driver supports it
    if (device.HasVertexBufferUnifiedMemory())
    {
        glEnableClientState(GL_VERTEX_ATTRIB_ARRAY_UNIFIED_NV);
        glEnableClientState(GL_ELEMENT_ARRAY_UNIFIED_NV);
    }
    blit_screen = std::make_unique<BlitScreen>(rasterizer, device_memory, state_tracker, program_manager, device, PresentFiltersForDisplay);
    blit_applet = std::make_unique<BlitScreen>(rasterizer, device_memory, state_tracker, program_manager, device, PresentFiltersForAppletCapture);
    capture_framebuffer.Create();
    capture_renderbuffer.Create();
    glBindRenderbuffer(GL_RENDERBUFFER, capture_renderbuffer.handle);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_SRGB8, VideoCore::Capture::LinearWidth, VideoCore::Capture::LinearHeight);

    InitializeWatermark();
}

RendererOpenGL::~RendererOpenGL()
{
    CleanupWatermark();
}

void RendererOpenGL::Composite(std::span<const Tegra::FramebufferConfig> framebuffers)
{
    if (framebuffers.empty())
    {
        return;
    }

    RenderAppletCaptureLayer(framebuffers);
    RenderScreenshot(framebuffers);

    state_tracker.BindFramebuffer(0);
    blit_screen->DrawScreen(framebuffers, emu_window.GetFramebufferLayout(), false);
    if (watermark_show)
    {
        RenderWatermark();
    }

    ++m_current_frame;

    gpu.RendererFrameEndNotify();
    rasterizer.TickFrame();

    context->SwapBuffers();

    render_window.OnFrameDisplayed();
}

void RendererOpenGL::AddTelemetryFields()
{
    const char * const gl_version{reinterpret_cast<char const *>(glGetString(GL_VERSION))};
    const char * const gpu_vendor{reinterpret_cast<char const *>(glGetString(GL_VENDOR))};
    const char * const gpu_model{reinterpret_cast<char const *>(glGetString(GL_RENDERER))};

    LOG_INFO(Render_OpenGL, "GL_VERSION: {}", gl_version);
    LOG_INFO(Render_OpenGL, "GL_VENDOR: {}", gpu_vendor);
    LOG_INFO(Render_OpenGL, "GL_RENDERER: {}", gpu_model);
}

void RendererOpenGL::InitializeWatermark()
{
    while (glGetError() != GL_NO_ERROR)
    {
    }

    int channels;
    unsigned char * image_data = stbi_load_from_memory(watermark_png, watermark_png_len, &watermark_width, &watermark_height, &channels, 4);
    if (!image_data)
    {
        LOG_ERROR(Render_OpenGL, "Failed to load watermark image: {}", stbi_failure_reason());
        return;
    }

    glGenTextures(1, &watermark_texture);
    glBindTexture(GL_TEXTURE_2D, watermark_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, watermark_width, watermark_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data);
    stbi_image_free(image_data);

    glGenVertexArrays(1, &watermark_vao);
    glGenBuffers(1, &watermark_vbo);

    glBindVertexArray(watermark_vao);
    glBindBuffer(GL_ARRAY_BUFFER, watermark_vbo);

    const size_t vertex_data_size = 6 * 4 * sizeof(float);
    glBufferData(GL_ARRAY_BUFFER, vertex_data_size, nullptr, GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    const std::string vertex_shader_source = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aTexCoord;

out vec2 TexCoord;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

    const std::string fragment_shader_source = R"(
#version 330 core
in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D watermarkTexture;
uniform float alpha;

void main() {
    vec4 texColor = texture(watermarkTexture, TexCoord);
    FragColor = vec4(texColor.rgb, texColor.a * alpha);
}
)";

    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    const char * vertex_src = vertex_shader_source.c_str();
    glShaderSource(vertex_shader, 1, &vertex_src, nullptr);
    glCompileShader(vertex_shader);

    GLint success;
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        char info_log[512];
        glGetShaderInfoLog(vertex_shader, 512, nullptr, info_log);
        LOG_ERROR(Render_OpenGL, "Vertex shader compilation failed: {}", info_log);
        return;
    }

    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    const char * fragment_src = fragment_shader_source.c_str();
    glShaderSource(fragment_shader, 1, &fragment_src, nullptr);
    glCompileShader(fragment_shader);

    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        char info_log[512];
        glGetShaderInfoLog(fragment_shader, 512, nullptr, info_log);
        LOG_ERROR(Render_OpenGL, "Fragment shader compilation failed: {}", info_log);
        glDeleteShader(vertex_shader);
        return;
    }

    watermark_shader = glCreateProgram();
    glAttachShader(watermark_shader, vertex_shader);
    glAttachShader(watermark_shader, fragment_shader);
    glLinkProgram(watermark_shader);

    glGetProgramiv(watermark_shader, GL_LINK_STATUS, &success);
    if (!success)
    {
        char info_log[512];
        glGetProgramInfoLog(watermark_shader, 512, nullptr, info_log);
        LOG_ERROR(Render_OpenGL, "Shader program linking failed: {}", info_log);
        watermark_shader = 0;
    }
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void RendererOpenGL::RenderWatermark()
{
    if (!watermark_texture || !watermark_shader || !watermark_vao || !watermark_vbo)
    {
        return;
    }

    if (!watermark_timer_started)
    {
        watermark_start_time = std::chrono::steady_clock::now();
        watermark_timer_started = true;
    }

    std::chrono::steady_clock::time_point current_time = std::chrono::steady_clock::now();
    const long long elapsed_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(current_time - watermark_start_time).count();
    const float elapsed = static_cast<float>(elapsed_seconds);

    float fade_alpha;
    const float fade_duration = 300.0f;
    if (elapsed >= fade_duration)
    {
        fade_alpha = 0.0f;
        watermark_show = false;
    }
    else
    {
        float progress = elapsed / fade_duration;
        float eased = 0.5f * (1.0f + cosf(progress * 3.14159f));
        fade_alpha = 0.8f * eased;
    }

    GLint current_program, current_vao, current_buffer, current_texture, current_active_texture;
    glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &current_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &current_buffer);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &current_texture);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &current_active_texture);

    glUseProgram(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    const Layout::FramebufferLayout & layout = emu_window.GetFramebufferLayout();
    const float layout_w = static_cast<float>(layout.width);
    const float layout_h = static_cast<float>(layout.height);
    float watermark_scale = 0.14f;
    float aspect_ratio = static_cast<float>(watermark_height) / static_cast<float>(watermark_width);
    float watermark_screen_width = layout_w * watermark_scale;
    float watermark_screen_height = watermark_screen_width * aspect_ratio;

    float padding = 18.0f;
    float x = layout_w - watermark_screen_width - padding;
    float y = padding;

    float ndc_left = (x / layout_w) * 2.0f - 1.0f;
    float ndc_right = ((x + watermark_screen_width) / layout_w) * 2.0f - 1.0f;
    float ndc_bottom = (y / layout_h) * 2.0f - 1.0f;
    float ndc_top = ((y + watermark_screen_height) / layout_h) * 2.0f - 1.0f;

    float vertices[] = {
        ndc_left, ndc_bottom, 0.0f, 1.0f,  // Bottom-left
        ndc_right, ndc_bottom, 1.0f, 1.0f, // Bottom-right
        ndc_left, ndc_top, 0.0f, 0.0f,     // Top-left

        ndc_right, ndc_bottom, 1.0f, 1.0f, // Bottom-right
        ndc_right, ndc_top, 1.0f, 0.0f,    // Top-right
        ndc_left, ndc_top, 0.0f, 0.0f      // Top-left
    };

    glUseProgram(watermark_shader);
    glBindVertexArray(watermark_vao);
    glBindBuffer(GL_ARRAY_BUFFER, watermark_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, watermark_texture);

    GLint tex_uniform = glGetUniformLocation(watermark_shader, "watermarkTexture");
    GLint alpha_uniform = glGetUniformLocation(watermark_shader, "alpha");
    if (tex_uniform != -1)
    {
        glUniform1i(tex_uniform, 0);
    }

    if (alpha_uniform != -1)
    {
        glUniform1f(alpha_uniform, fade_alpha);
    }

    while (glGetError() != GL_NO_ERROR)
    {
    }

    glDrawArrays(GL_TRIANGLES, 0, 6);
    glUseProgram(current_program);
    glBindVertexArray(current_vao);
    glBindBuffer(GL_ARRAY_BUFFER, current_buffer);
    glActiveTexture(current_active_texture);
    glBindTexture(GL_TEXTURE_2D, current_texture);
}

void RendererOpenGL::CleanupWatermark()
{
    if (watermark_texture)
    {
        glDeleteTextures(1, &watermark_texture);
        watermark_texture = 0;
    }

    if (watermark_shader)
    {
        glDeleteProgram(watermark_shader);
        watermark_shader = 0;
    }

    if (watermark_vao)
    {
        glDeleteVertexArrays(1, &watermark_vao);
        watermark_vao = 0;
    }

    if (watermark_vbo)
    {
        glDeleteBuffers(1, &watermark_vbo);
        watermark_vbo = 0;
    }
}

void RendererOpenGL::RenderToBuffer(std::span<const Tegra::FramebufferConfig> framebuffers, const Layout::FramebufferLayout & layout, void * dst)
{
    GLint old_read_fb;
    GLint old_draw_fb;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_fb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw_fb);

    // Draw the current frame to the screenshot framebuffer
    screenshot_framebuffer.Create();
    glBindFramebuffer(GL_FRAMEBUFFER, screenshot_framebuffer.handle);

    GLuint renderbuffer;
    glGenRenderbuffers(1, &renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_SRGB8, layout.width, layout.height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);

    blit_screen->DrawScreen(framebuffers, layout, false);

    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(0, 0, layout.width, layout.height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, dst);

    screenshot_framebuffer.Release();
    glDeleteRenderbuffers(1, &renderbuffer);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw_fb);
}

void RendererOpenGL::RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers)
{
    if (!renderer_settings.screenshot_requested)
    {
        return;
    }

    RenderToBuffer(framebuffers, renderer_settings.screenshot_framebuffer_layout, renderer_settings.screenshot_bits);

    renderer_settings.screenshot_complete_callback(true);
    renderer_settings.screenshot_requested = false;
}

void RendererOpenGL::RenderAppletCaptureLayer(std::span<const Tegra::FramebufferConfig> framebuffers)
{
    GLint old_read_fb;
    GLint old_draw_fb;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_fb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw_fb);

    glBindFramebuffer(GL_FRAMEBUFFER, capture_framebuffer.handle);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,capture_renderbuffer.handle);

    blit_applet->DrawScreen(framebuffers, VideoCore::Capture::Layout, true);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw_fb);
}

std::vector<u8> RendererOpenGL::GetAppletCaptureBuffer()
{
    using namespace VideoCore::Capture;

    std::vector<u8> linear(TiledSize);
    std::vector<u8> out(TiledSize);

    GLint old_read_fb;
    GLint old_draw_fb;
    GLint old_pixel_pack_buffer;
    GLint old_pack_row_length;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_fb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw_fb);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pixel_pack_buffer);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &old_pack_row_length);

    glBindFramebuffer(GL_FRAMEBUFFER, capture_framebuffer.handle);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, capture_renderbuffer.handle);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(0, 0, LinearWidth, LinearHeight, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, linear.data());

    glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw_fb);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, old_pixel_pack_buffer);
    glPixelStorei(GL_PACK_ROW_LENGTH, old_pack_row_length);

    Tegra::Texture::SwizzleTexture(out, linear, BytesPerPixel, LinearWidth, LinearHeight, LinearDepth, BlockHeight, BlockDepth);

    return out;
}

} // namespace OpenGL
