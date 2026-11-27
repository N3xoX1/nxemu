// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <algorithm>
#include <filesystem>
#include <system_error>
#include <chrono>
#include <atomic>
#include <set>
#include <mutex>
#include <string>
#include <vector>

#include "yuzu_common/fs/fs.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/settings.h"
#include "video_settings.h"
#include "frontend/emu_window.h"
#include "frontend/graphics_context.h"
#include "yuzu_video_core/capture.h"
#include "yuzu_video_core/framebuffer_config.h"
#include "yuzu_video_core/present.h"
#include "yuzu_video_core/host1x/gpu_device_memory_manager.h"
#include "yuzu_video_core/renderer_metal/maxwell_to_mtl.h"
#include "yuzu_video_core/textures/decoders.h"
#include "yuzu_video_core/renderer_metal/renderer_metal.h"

namespace Metal {
RendererMetal::RendererMetal(Core::Frontend::EmuWindow& emu_window,
                             Tegra::MaxwellDeviceMemoryManager& device_memory, Tegra::GPU& gpu,
                             std::unique_ptr<Core::Frontend::GraphicsContext> context_,
                             Vulkan::Display& display)
    : RendererBase(emu_window, std::move(context_)), m_gpu{gpu},
      m_device{static_cast<u32>(videoSettings.vulkan_device)}, m_memory_allocator{m_device},
      m_scheduler{m_device}, m_staging_pool{m_device, m_memory_allocator, m_scheduler},
      m_bridge{display, m_device, m_scheduler, emu_window},
      m_rasterizer{gpu,           device_memory, m_device,
                   m_memory_allocator, m_staging_pool, m_scheduler},
      m_blit_screen{m_device, m_scheduler}, m_device_memory{device_memory} {
    LOG_INFO(Render_Metal, "Metal renderer initialized");
}

RendererMetal::~RendererMetal() {
    m_scheduler.Finish();
    m_bridge.Release();
}

MTL::Texture* RendererMetal::UploadFramebuffer(const Tegra::FramebufferConfig& config) {
    const DAddr framebuffer_addr = config.address + config.offset;
    if (framebuffer_addr == 0) {
        return nullptr;
    }

    const auto pixel_format = VideoCore::Surface::PixelFormatFromGPUPixelFormat(config.pixel_format);
    const FormatInfo format_info = SurfaceFormat(m_device, pixel_format);
    if (format_info.support == FormatSupport::Unsupported) {
        LOG_DEBUG(Render_Metal, "Unsupported framebuffer format {}",
                  static_cast<u32>(config.pixel_format));
        return nullptr;
    }

    const u32 bytes_per_pixel = VideoCore::Surface::BytesPerBlock(pixel_format);

    // The guest buffer is block-linear, so its footprint is NOT stride * height *
    // bpp. Reading only the linear size truncates the source.
    constexpr u32 BLOCK_HEIGHT_LOG2 = 4;
    const u64 tiled_size = Tegra::Texture::CalculateSize(true, bytes_per_pixel, config.stride,
                                                         config.height, 1, BLOCK_HEIGHT_LOG2, 0);
    const u64 linear_size = static_cast<u64>(config.width) * config.height * bytes_per_pixel;
    if (tiled_size == 0 || linear_size == 0) {
        return nullptr;
    }

    // GetPointer, not ReadBlockUnsafe: the latter does not see the display
    // buffer's backing and returns all zeros for this address.
    const u8* const host_ptr = m_device_memory.GetPointer<u8>(framebuffer_addr);
    if (host_ptr == nullptr) {
        return nullptr;
    }

    if (!m_framebuffer_textures[0] || m_framebuffer_width != config.width ||
        m_framebuffer_height != config.height || m_framebuffer_format != format_info.format) {
        ScopedAutoreleasePool pool;

        for (Owned<MTL::Texture>& slot : m_framebuffer_textures) {
            if (slot) {
                m_scheduler.KeepAlive(slot.get(), slot.get());
            }
            MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
            descriptor->setTextureType(MTL::TextureType2D);
            descriptor->setPixelFormat(format_info.format);
            descriptor->setWidth(config.width);
            descriptor->setHeight(config.height);
            descriptor->setMipmapLevelCount(1);
            descriptor->setUsage(MTL::TextureUsageShaderRead);
            slot = m_memory_allocator.CreateTexture(descriptor, MemoryUsage::Upload);
            descriptor->release();

            if (!slot) {
                return nullptr;
            }
            slot->setLabel(MTLSTR("eden guest framebuffer"));
        }
        m_framebuffer_slot = 0;
        m_framebuffer_width = config.width;
        m_framebuffer_height = config.height;
        m_framebuffer_format = format_info.format;
        LOG_INFO(Render_Metal, "Guest framebuffer upload target: {}x{}", config.width,
                 config.height);
    }

    m_framebuffer_staging.resize_destructive(linear_size);
    Tegra::Texture::UnswizzleTexture(
        std::span<u8>(m_framebuffer_staging.data(), linear_size),
        std::span<const u8>(host_ptr, tiled_size), bytes_per_pixel, config.width, config.height, 1,
        BLOCK_HEIGHT_LOG2, 0);

    // Rotate rather than block: replaceRegion writes host memory the GPU may still
    // be reading, and a wait between acquire and present stops presentation.
    m_framebuffer_slot = (m_framebuffer_slot + 1) % FRAMEBUFFER_SLOTS;
    MTL::Texture* const target = m_framebuffer_textures[m_framebuffer_slot].get();

    target->replaceRegion(
        MTL::Region::Make2D(0, 0, config.width, config.height), 0,
        m_framebuffer_staging.data(), static_cast<NS::UInteger>(config.width) * bytes_per_pixel);

    return target;
}

namespace {
/// Runs at the end of a frame, presented or not: the caches age by frame tick,
/// so a stalled TickFrame means nothing is ever old enough to reclaim.
struct FrameEnd {
    Tegra::GPU& gpu;
    Core::Frontend::EmuWindow& window;
    RasterizerMetal& rasterizer;
    ~FrameEnd() {
        gpu.RendererFrameEndNotify();
        window.OnFrameDisplayed();
        rasterizer.TickFrame();
    }
};

} // Anonymous namespace

void RendererMetal::Composite(std::span<const Tegra::FramebufferConfig> framebuffers) {
    // Threads with no run loop have no autorelease pool, and without one the +0
    // drawable from nextDrawable() never returns to the layer's pool.
    ScopedAutoreleasePool pool;

    if (framebuffers.empty()) {
        return;
    }

    const FrameEnd frame_end{m_gpu, render_window, m_rasterizer};

    if (!render_window.IsShown()) {
        return;
    }

    if (HoldIncompleteFrame()) {
        m_scheduler.Record([](CommandContext& context) { context.EndEncoding(); });
        m_scheduler.Flush();
        return;
    }
    m_presented_once = true;

    const auto& layout = render_window.GetFramebufferLayout();
    if (layout.width == 0 || layout.height == 0) {
        return;
    }
    Vulkan::Frame* const frame = m_bridge.AcquireFrame(layout.width, layout.height);
    if (frame == nullptr) {
        return;
    }
    MTL::Texture* const target = m_bridge.Texture(frame);
    const u32 width = static_cast<u32>(target->width());
    const u32 height = static_cast<u32>(target->height());

    // Bottom layer first. Library applets (the software keyboard above all) are
    // composited by nvnflinger as their own layer on top of the game's.
    bool drew_any = false;
    // Feeds the status bar's resolution readout, as the Vulkan present path does.
    VideoCore::BeginFsrFrame(videoSettings.fsr_sharpening_slider, videoSettings.cas_sharpening_slider,
                             static_cast<unsigned>(videoSettings.scaling_filter), 0);
    const u32 screen_width = layout.screen.GetWidth();
    const u32 screen_height = layout.screen.GetHeight();
    for (const Tegra::FramebufferConfig& config : framebuffers) {
        u32 guest_width = 0;
        u32 guest_height = 0;
        MTL::Texture* source =
            m_rasterizer.GetFramebufferTexture(config, &guest_width, &guest_height);
        if (source == nullptr) {
            source = UploadFramebuffer(config);
        }
        if (source == nullptr || !m_blit_screen.IsValid()) {
            continue;
        }
        // The guest's crop_rect is the authority on what is on screen (it narrows
        // rather than shrinking the buffer across a docked/handheld switch) and it
        // carries the transform flags. Pass the guest extent, not the texture's.
        const auto crop = Tegra::NormalizeCrop(
            config, guest_width != 0 ? guest_width : static_cast<u32>(source->width()),
            guest_height != 0 ? guest_height : static_cast<u32>(source->height()));

        // The first layer that resolves owns the frame, whatever its index: if
        // layer 0's lookup missed, layer 1 still has to clear rather than blend
        // onto undefined contents. layout.screen is what letterboxes.
        if (!drew_any) {
            // The source texture is at the scaled resolution, the crop is normalized.
            const u32 source_width = static_cast<u32>(source->width());
            const u32 source_height = static_cast<u32>(source->height());
            VideoCore::NotePresentInput(source_width, source_height,
                static_cast<unsigned>(std::lround(std::abs(crop.right - crop.left) * static_cast<f32>(source_width))),
                static_cast<unsigned>(std::lround(std::abs(crop.bottom - crop.top) * static_cast<f32>(source_height))));
            const auto selection =
                ResolvePresentScale(GetScalingFilter(), GetDownscalingFilter() != 0, source_width,
                                    source_height, crop, screen_width, screen_height);
            VideoCore::NotePresentSelection(static_cast<unsigned>(selection.filter),
                                            static_cast<unsigned>(selection.mode));
        }
        m_blit_screen.Draw(source, target, width, height, crop.left, crop.top, crop.right,
                           crop.bottom, config.blending, !drew_any, layout.screen);
        drew_any = true;
    }
    VideoCore::PublishPresentationStatus(false, screen_width, screen_height,
                                         static_cast<unsigned>(framebuffers.size()));

    if (!drew_any) {
        RenderPassState pass{};
        pass.num_color = 1;
        pass.color[0].texture = target;
        pass.color[0].load_action = LoadAction::Clear;
        pass.color[0].store_action = StoreAction::Store;
        pass.color[0].clear_color = {0.0, 0.0, 0.0, 1.0};
        pass.render_area_width = width;
        pass.render_area_height = height;
        m_scheduler.RequestRenderPass(pass);
    }

    m_scheduler.Record([](CommandContext& context) { context.EndEncoding(); });
    frame->frame_interval_ns = framebuffers.front().frame_interval_ns;
    const u64 tick = m_scheduler.Flush();

    const auto window = render_window.GetWindowInfo();
    m_bridge.Present(frame, tick, window);

    // Only once asked for, this is a second full-screen render every frame.
    if (m_capture_requested.load(std::memory_order_relaxed)) {
        RenderCaptureLayer(framebuffers);
    }

    RenderScreenshot(framebuffers);
}

bool RendererMetal::HoldIncompleteFrame() {
    // A scene that just loaded compiles many pipelines at once, and its first frames miss many
    // draws. Those frames are not shown: the screen keeps the previous frame, often the
    // transition, while emulation goes on. During play, a few missing draws are better than a
    // frozen screen, so frames are held only right after a scene change, and never for long.
    constexpr std::chrono::milliseconds scene_change_window{1000};
    constexpr std::chrono::milliseconds max_hold{1500};
    constexpr u32 incomplete_percent = 10;
    const auto draws = m_rasterizer.TakeFrameDraws();
    const auto now = std::chrono::steady_clock::now();
    if (draws.new_render_targets) {
        m_last_new_render_target = now;
    }
    const bool incomplete =
        draws.skipped > 0 && draws.skipped * 100 >= draws.total * incomplete_percent;
    if (!incomplete) {
        m_incomplete_since.reset();
    } else if (!m_incomplete_since) {
        m_incomplete_since = now;
        m_hold_incomplete =
            m_last_new_render_target && now - *m_last_new_render_target < scene_change_window;
    }
    return incomplete && m_hold_incomplete && m_presented_once &&
           now - *m_incomplete_since < max_hold;
}

void RendererMetal::RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (!renderer_settings.screenshot_requested) {
        return;
    }
    const auto& layout = renderer_settings.screenshot_framebuffer_layout;
    const u32 width = layout.width;
    const u32 height = layout.height;
    // The callback's argument is invert_y, not success, so a failed capture is dropped.
    const auto fail = [this] {
        LOG_WARNING(Render_Metal, "Screenshot could not be captured");
        renderer_settings.screenshot_requested = false;
    };
    if (width == 0 || height == 0 || !m_blit_screen.IsValid()) {
        fail();
        return;
    }

    if (!m_screenshot_texture || m_screenshot_width != width || m_screenshot_height != height) {
        if (m_screenshot_texture) {
            m_scheduler.Finish();
            m_device.RemoveResidentAllocation(m_screenshot_texture.get());
            m_device.RemoveResidentAllocation(m_screenshot_readback.get());
        }
        MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
        descriptor->setTextureType(MTL::TextureType2D);
        // sRGB like the drawable, so the bytes read back are the ones that are displayed.
        descriptor->setPixelFormat(MTL::PixelFormatBGRA8Unorm_sRGB);
        descriptor->setWidth(width);
        descriptor->setHeight(height);
        descriptor->setMipmapLevelCount(1);
        descriptor->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        m_screenshot_texture = m_memory_allocator.CreateTexture(descriptor, MemoryUsage::DeviceLocal);
        descriptor->release();
        m_screenshot_readback =
            m_memory_allocator.CreateBuffer(u64{width} * height * 4, MemoryUsage::Download);
        if (!m_screenshot_texture || !m_screenshot_readback) {
            if (m_screenshot_texture) {
                m_device.RemoveResidentAllocation(m_screenshot_texture.get());
            }
            if (m_screenshot_readback) {
                m_device.RemoveResidentAllocation(m_screenshot_readback.get());
            }
            m_device.CommitResidency();
            m_screenshot_texture = {};
            m_screenshot_readback = {};
            fail();
            return;
        }
        m_device.CommitResidency();
        m_screenshot_width = width;
        m_screenshot_height = height;
    }

    bool drew_any = false;
    for (const Tegra::FramebufferConfig& config : framebuffers) {
        u32 guest_width = 0;
        u32 guest_height = 0;
        MTL::Texture* source =
            m_rasterizer.GetFramebufferTexture(config, &guest_width, &guest_height);
        if (source == nullptr) {
            source = UploadFramebuffer(config);
        }
        if (source == nullptr) {
            continue;
        }
        const auto crop = Tegra::NormalizeCrop(
            config, guest_width != 0 ? guest_width : static_cast<u32>(source->width()),
            guest_height != 0 ? guest_height : static_cast<u32>(source->height()));
        m_blit_screen.Draw(source, m_screenshot_texture.get(), width, height, crop.left, crop.top,
                           crop.right, crop.bottom, config.blending, !drew_any, layout.screen);
        drew_any = true;
    }
    if (!drew_any) {
        fail();
        return;
    }

    MTL::Texture* const src = m_screenshot_texture.get();
    MTL::Buffer* const dst = m_screenshot_readback.get();
    m_scheduler.Record([src, dst, width, height](CommandContext& context) {
        BlitEncoder encoder = context.RequestBlit();
        if (encoder == nullptr) {
            return;
        }
        encoder->copyFromTexture(src, 0, 0, MTL::Origin::Make(0, 0, 0),
                                 MTL::Size::Make(width, height, 1), dst, 0, size_t{width} * 4,
                                 size_t{width} * height * 4, MTL::BlitOptionNone);
    });
    m_scheduler.Finish();

    std::memcpy(renderer_settings.screenshot_bits, dst->contents(), size_t{width} * height * 4);
    renderer_settings.screenshot_complete_callback(false);
    renderer_settings.screenshot_requested = false;
}

void RendererMetal::RenderCaptureLayer(std::span<const Tegra::FramebufferConfig> framebuffers) {
    using namespace VideoCore::Capture;

    // Reached from the service thread as well as the GPU thread, see Composite.
    ScopedAutoreleasePool pool;
    std::scoped_lock capture_lock{m_capture_mutex};

    if (!m_capture_texture) {
        MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
        descriptor->setTextureType(MTL::TextureType2D);
        // VideoCore::Capture::Format is A8B8G8R8_UNORM (RGBA, sRGB-encoded bytes).
        // Rendering into RGBA8Unorm_sRGB keeps the display transfer curve intact.
        descriptor->setPixelFormat(MTL::PixelFormatRGBA8Unorm_sRGB);
        descriptor->setWidth(LinearWidth);
        descriptor->setHeight(LinearHeight);
        descriptor->setMipmapLevelCount(1);
        descriptor->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        m_capture_texture = m_memory_allocator.CreateTexture(descriptor, MemoryUsage::DeviceLocal);
        descriptor->release();
        if (!m_capture_texture) {
            return;
        }
        m_capture_texture->setLabel(MTLSTR("eden applet capture"));
    }

    bool drew_any = false;
    for (const Tegra::FramebufferConfig& config : framebuffers) {
        u32 guest_width = 0;
        u32 guest_height = 0;
        MTL::Texture* source =
            m_rasterizer.GetFramebufferTexture(config, &guest_width, &guest_height);
        if (source == nullptr) {
            source = UploadFramebuffer(config);
        }
        if (source == nullptr || !m_blit_screen.IsValid()) {
            continue;
        }
        const auto crop = Tegra::NormalizeCrop(
            config, guest_width != 0 ? guest_width : static_cast<u32>(source->width()),
            guest_height != 0 ? guest_height : static_cast<u32>(source->height()));
        m_blit_screen.Draw(source, m_capture_texture.get(), LinearWidth, LinearHeight, crop.left,
                           crop.top, crop.right, crop.bottom, config.blending, !drew_any, {},
                           /*apply_post_processing=*/false);
        drew_any = true;
    }
}

std::vector<u8> RendererMetal::GetAppletCaptureBuffer() {
    using namespace VideoCore::Capture;

    ScopedAutoreleasePool pool;

    m_capture_requested.store(true, std::memory_order_relaxed);

    // The guest expects the capture block-linear, hence readback plus swizzle.
    std::vector<u8> out(TiledSize);
    std::scoped_lock capture_lock{m_capture_mutex};
    if (!m_capture_texture) {
        return out;
    }

    const size_t linear_size = size_t{LinearWidth} * LinearHeight * BytesPerPixel;
    if (!m_capture_readback) {
        m_capture_readback = Adopt(
            m_device.GetDevice()->newBuffer(linear_size, MTL::ResourceStorageModeShared));
        if (m_capture_readback) {
            m_device.AddResidentAllocation(m_capture_readback.get());
            m_device.CommitResidency();
        }
        if (!m_capture_readback) {
            return out;
        }
    }

    MTL::Texture* const src = m_capture_texture.get();
    MTL::Buffer* const dst = m_capture_readback.get();
    m_scheduler.RequestOutsideRenderPassOperationContext();
    m_scheduler.Record([src, dst](CommandContext& context) {
        BlitEncoder encoder = context.RequestBlit();
        if (encoder == nullptr) {
            return;
        }
        encoder->copyFromTexture(src, 0, 0, MTL::Origin::Make(0, 0, 0),
                                 MTL::Size::Make(LinearWidth, LinearHeight, 1), dst, 0,
                                 LinearWidth * BytesPerPixel,
                                 size_t{LinearWidth} * LinearHeight * BytesPerPixel,
                                 MTL::BlitOptionNone);
    });
    m_scheduler.Finish();

    Tegra::Texture::SwizzleTexture(
        out, std::span<const u8>(static_cast<const u8*>(dst->contents()), linear_size),
        BytesPerPixel, LinearWidth, LinearHeight, LinearDepth, BlockHeight, BlockDepth);
    return out;
}

} // namespace Metal
