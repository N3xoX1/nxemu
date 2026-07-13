#pragma once

#include <algorithm>
#include <nxemu-module-spec/video.h>
#include <yuzu_common/common_types.h>

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
    RendererBackend renderer_backend;
    ShaderBackend shader_backend;
    int32_t vulkan_device;

    bool use_disk_shader_cache;
    bool use_asynchronous_gpu_emulation;
    VSyncMode vsync_mode;
    NvdecEmulation nvdec_emulation;
    FullscreenMode fullscreen_mode;
    AspectRatio aspect_ratio;

    Settings::ResolutionScalingInfo resolution_info;
    ResolutionSetup resolution_setup;
    ScalingFilter scaling_filter;
    AntiAliasing anti_aliasing;
    int32_t fsr_sharpening_slider;

    u8 bg_red;
    u8 bg_green;
    u8 bg_blue;

    GpuAccuracy current_gpu_accuracy;
    AstcRecompression astc_recompression;
    VramUsageMode vram_usage_mode;
    bool renderer_force_max_clock;
    AstcDecodeMode accelerate_astc;
    GpuAccuracy gpu_accuracy;
    AnisotropyMode max_anisotropy;
    bool async_presentation;
    bool use_reactive_flushing;

    bool use_asynchronous_shaders;
    bool use_fast_gpu_time;
    bool use_vulkan_driver_pipeline_cache;
    bool enable_compute_pipelines;
    bool use_video_framerate;
    bool barrier_feedback_loops;

    bool renderer_debug;
    bool renderer_shader_feedback;
    bool enable_nsight_aftermath;
    bool disable_shader_loop_safety_checks;
    bool enable_renderdoc_hotkey;
    bool disable_buffer_reorder;
};

extern VideoSettings videoSettings;

void SetupVideoSetting(void);
void CleanupVideoSetting(void);
void SaveVideoSettings(void);
