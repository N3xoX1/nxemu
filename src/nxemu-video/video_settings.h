#pragma once

#include <algorithm>
#include <nxemu-module-spec/video.h>
#include <yuzu_common/settings.h>

namespace Settings
{

struct ResolutionScalingInfo
{
    u32 up_scale{1};
    u32 down_shift{0};
    f32 up_factor{1.0f};
    f32 down_factor{1.0f};
    bool active{};
    bool downscale{};

    s32 ScaleUp(s32 value) const
    {
        if (value == 0)
        {
            return 0;
        }
        return std::max((value * static_cast<s32>(up_scale)) >> static_cast<s32>(down_shift), 1);
    }

    u32 ScaleUp(u32 value) const
    {
        if (value == 0U)
        {
            return 0U;
        }
        return std::max((value * up_scale) >> down_shift, 1U);
    }
};

void UpdateGPUAccuracy();
bool IsGPULevelExtreme();
bool IsGPULevelHigh();
void TranslateResolutionInfo(ResolutionSetup setup, ResolutionScalingInfo & info);
void UpdateRescalingInfo();

} // namespace Settings

struct VideoSettings
{
    Settings::Linkage linkage{};

    Settings::SwitchableSetting<RendererBackend, true> renderer_backend{linkage, RendererBackend::Vulkan, RendererBackend::OpenGL,RendererBackend::Null, "backend", Settings::Category::Renderer};
    Settings::SwitchableSetting<ShaderBackend, true> shader_backend{linkage, ShaderBackend::Glsl, ShaderBackend::Glsl,ShaderBackend::SpirV, "shader_backend", Settings::Category::Renderer,Settings::Specialization::RuntimeList};
    Settings::SwitchableSetting<int> vulkan_device{linkage, 0, "vulkan_device",Settings::Category::Renderer,Settings::Specialization::RuntimeList};

    Settings::SwitchableSetting<bool> use_disk_shader_cache{linkage, true, "use_disk_shader_cache", Settings::Category::Renderer};
    Settings::SwitchableSetting<bool> use_asynchronous_gpu_emulation{linkage, true, "use_asynchronous_gpu_emulation", Settings::Category::Renderer};
    Settings::SwitchableSetting<VSyncMode, true> vsync_mode{linkage, VSyncMode::Fifo, VSyncMode::Immediate,VSyncMode::FifoRelaxed, "use_vsync", Settings::Category::Renderer,Settings::Specialization::RuntimeList, true, true};
    Settings::SwitchableSetting<NvdecEmulation> nvdec_emulation{linkage, NvdecEmulation::Gpu, "nvdec_emulation", Settings::Category::Renderer};
#ifdef _WIN32
    Settings::SwitchableSetting<FullscreenMode, true> fullscreen_mode{linkage, FullscreenMode::Borderless, FullscreenMode::Borderless, FullscreenMode::Exclusive, "fullscreen_mode", Settings::Category::Renderer, Settings::Specialization::Default, true, true};
#else
    Settings::SwitchableSetting<FullscreenMode, true> fullscreen_mode{linkage, FullscreenMode::Exclusive, FullscreenMode::Borderless, FullscreenMode::Exclusive, "fullscreen_mode", Settings::Category::Renderer, Settings::Specialization::Default, true, true};
#endif
    Settings::SwitchableSetting<AspectRatio, true> aspect_ratio{linkage, AspectRatio::R16_9, AspectRatio::R16_9, AspectRatio::Stretch, "aspect_ratio", Settings::Category::Renderer, Settings::Specialization::Default, true, true};

    Settings::ResolutionScalingInfo resolution_info{};
    Settings::SwitchableSetting<ResolutionSetup> resolution_setup{linkage, ResolutionSetup::Res1X, "resolution_setup", Settings::Category::Renderer};
    Settings::SwitchableSetting<ScalingFilter> scaling_filter{linkage, ScalingFilter::Bilinear, "scaling_filter", Settings::Category::Renderer,Settings::Specialization::Default, true, true};
    Settings::SwitchableSetting<AntiAliasing> anti_aliasing{linkage, AntiAliasing::None, "anti_aliasing", Settings::Category::Renderer,Settings::Specialization::Default, true, true};
    Settings::SwitchableSetting<int, true> fsr_sharpening_slider{linkage, 25, 0, 200, "fsr_sharpening_slider", Settings::Category::Renderer,Settings::Specialization::Scalar | Settings::Specialization::Percentage, true, true};

    Settings::SwitchableSetting<u8, false> bg_red{linkage, 0, "bg_red", Settings::Category::Renderer, Settings::Specialization::Default,true, true};
    Settings::SwitchableSetting<u8, false> bg_green{linkage, 0, "bg_green", Settings::Category::Renderer, Settings::Specialization::Default,true, true};
    Settings::SwitchableSetting<u8, false> bg_blue{linkage, 0, "bg_blue", Settings::Category::Renderer, Settings::Specialization::Default,true, true};

    GpuAccuracy current_gpu_accuracy{GpuAccuracy::High};
    Settings::SwitchableSetting<AstcRecompression, true> astc_recompression{linkage, AstcRecompression::Uncompressed, AstcRecompression::Uncompressed, AstcRecompression::Bc3, "astc_recompression", Settings::Category::RendererAdvanced};
    Settings::SwitchableSetting<VramUsageMode, true> vram_usage_mode{linkage, VramUsageMode::Conservative, VramUsageMode::Conservative, VramUsageMode::Aggressive, "vram_usage_mode", Settings::Category::RendererAdvanced};
    Settings::SwitchableSetting<bool> renderer_force_max_clock{linkage, false, "force_max_clock", Settings::Category::RendererAdvanced};
#ifdef ANDROID
    Settings::SwitchableSetting<AstcDecodeMode, true> accelerate_astc{linkage, AstcDecodeMode::Cpu, AstcDecodeMode::Cpu, AstcDecodeMode::CpuAsynchronous, "accelerate_astc", Settings::Category::Renderer};
    Settings::SwitchableSetting<GpuAccuracy, true> gpu_accuracy{linkage, GpuAccuracy::Normal, GpuAccuracy::Normal, GpuAccuracy::Extreme, "gpu_accuracy", Settings::Category::RendererAdvanced, Settings::Specialization::Default, true, true};
    Settings::SwitchableSetting<AnisotropyMode, true> max_anisotropy{linkage, AnisotropyMode::Default, AnisotropyMode::Automatic, AnisotropyMode::X16, "max_anisotropy", Settings::Category::RendererAdvanced};
    Settings::SwitchableSetting<bool> async_presentation{linkage, true, "async_presentation", Settings::Category::RendererAdvanced};
    Settings::SwitchableSetting<bool> use_reactive_flushing{linkage, false, "use_reactive_flushing", Settings::Category::RendererAdvanced};
#else
    Settings::SwitchableSetting<AstcDecodeMode, true> accelerate_astc{linkage, AstcDecodeMode::Gpu, AstcDecodeMode::Cpu, AstcDecodeMode::CpuAsynchronous, "accelerate_astc", Settings::Category::Renderer};
    Settings::SwitchableSetting<GpuAccuracy, true> gpu_accuracy{linkage, GpuAccuracy::High, GpuAccuracy::Normal, GpuAccuracy::Extreme, "gpu_accuracy", Settings::Category::RendererAdvanced, Settings::Specialization::Default, true, true};
    Settings::SwitchableSetting<AnisotropyMode, true> max_anisotropy{linkage, AnisotropyMode::Automatic, AnisotropyMode::Automatic, AnisotropyMode::X16, "max_anisotropy", Settings::Category::RendererAdvanced};
    Settings::SwitchableSetting<bool> async_presentation{linkage, false, "async_presentation", Settings::Category::RendererAdvanced};
    Settings::SwitchableSetting<bool> use_reactive_flushing{linkage, true, "use_reactive_flushing", Settings::Category::RendererAdvanced};
#endif
    Settings::SwitchableSetting<bool> use_asynchronous_shaders{linkage, false, "use_asynchronous_shaders", Settings::Category::RendererAdvanced};
    Settings::SwitchableSetting<bool> use_fast_gpu_time{linkage, true, "use_fast_gpu_time", Settings::Category::RendererAdvanced, Settings::Specialization::Default, true, true};
    Settings::SwitchableSetting<bool> use_vulkan_driver_pipeline_cache{linkage, true, "use_vulkan_driver_pipeline_cache", Settings::Category::RendererAdvanced, Settings::Specialization::Default, true, true};
    Settings::SwitchableSetting<bool> enable_compute_pipelines{linkage, false, "enable_compute_pipelines", Settings::Category::RendererAdvanced};
    Settings::SwitchableSetting<bool> use_video_framerate{linkage, false, "use_video_framerate", Settings::Category::RendererAdvanced};
    Settings::SwitchableSetting<bool> barrier_feedback_loops{linkage, true, "barrier_feedback_loops", Settings::Category::RendererAdvanced};

    Settings::Setting<bool> renderer_debug{linkage, false, "debug", Settings::Category::RendererDebug};
    Settings::Setting<bool> renderer_shader_feedback{linkage, false, "shader_feedback", Settings::Category::RendererDebug};
    Settings::Setting<bool> enable_nsight_aftermath{linkage, false, "nsight_aftermath", Settings::Category::RendererDebug};
    Settings::Setting<bool> disable_shader_loop_safety_checks{linkage, false, "disable_shader_loop_safety_checks", Settings::Category::RendererDebug};
    Settings::Setting<bool> enable_renderdoc_hotkey{linkage, false, "renderdoc_hotkey", Settings::Category::RendererDebug};
    Settings::Setting<bool> disable_buffer_reorder{linkage, false, "disable_buffer_reorder", Settings::Category::RendererDebug};
};

extern VideoSettings videoSettings;

void SetupVideoSetting(void);
void SaveVideoSettings(void);
