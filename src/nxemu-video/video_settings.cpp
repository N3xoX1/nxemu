#include "video_settings.h"
#include "presentation_settings.h"
#include "video_enum_strings.h"
#include "video_settings_identifiers.h"
#include <algorithm>
#include <atomic>
#include <common/json.h>
#include <cstring>
#include <cstdlib>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/module_setting_table.h>
#include <nxemu-module-spec/base.h>
#include <nxemu-module-spec/fsr_sharpness.h>
#include <yuzu_common/yuzu_assert.h>

extern IModuleSettings * g_settings;

VideoSettings videoSettings{};

namespace
{
// Configuration storage stays on the settings thread; the GPU reads these snapshots.
std::atomic<int32_t> runtime_dma_accuracy{0};
std::atomic<bool> runtime_gpu_command_synchronization{false};
std::atomic<bool> runtime_fast_gpu_readback{true};
std::atomic<bool> runtime_fsr_legacy{false};
}

namespace Settings
{

bool IsLegacyFsrEnabled()
{
    return runtime_fsr_legacy.load(std::memory_order_relaxed);
}

void UpdateGPUAccuracy()
{
    videoSettings.current_gpu_accuracy = videoSettings.gpu_accuracy;
}

bool IsGPULevelExtreme()
{
    return videoSettings.current_gpu_accuracy == GpuAccuracy::Extreme;
}

bool IsGPULevelHigh()
{
    return videoSettings.current_gpu_accuracy == GpuAccuracy::Extreme ||
           videoSettings.current_gpu_accuracy == GpuAccuracy::High;
}

bool IsDMALevelDefault()
{
    return runtime_dma_accuracy.load(std::memory_order_relaxed) == 0;
}

bool IsDMALevelSafe()
{
    return runtime_dma_accuracy.load(std::memory_order_relaxed) == 2;
}

bool UseSafeDMAReads()
{
    const int32_t accuracy = runtime_dma_accuracy.load(std::memory_order_relaxed);
    return accuracy == 0 ? IsGPULevelHigh() : accuracy == 2;
}

bool IsGpuCommandSynchronizationEnabled()
{
    return runtime_gpu_command_synchronization.load(std::memory_order_relaxed);
}

// High accuracy waits for host GPU completion before publishing guest fences.
// Fast readback permits immediate ordinary fences only without pending readbacks.
// GPU-backed query results always retire safely.
bool IsGpuReadbackExact()
{
    return IsGPULevelHigh() && !runtime_fast_gpu_readback.load(std::memory_order_relaxed);
}

void TranslateResolutionInfo(ResolutionSetup setup, ResolutionScalingInfo & info)
{
    info.downscale = false;
    switch (setup)
    {
    case ResolutionSetup::Res1_2X:
        info.up_scale = 1;
        info.down_shift = 1;
        info.downscale = true;
        break;
    case ResolutionSetup::Res3_4X:
        info.up_scale = 3;
        info.down_shift = 2;
        info.downscale = true;
        break;
    case ResolutionSetup::Res1X:
        info.up_scale = 1;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res3_2X:
        info.up_scale = 3;
        info.down_shift = 1;
        break;
    case ResolutionSetup::Res2X:
        info.up_scale = 2;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res3X:
        info.up_scale = 3;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res4X:
        info.up_scale = 4;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res5X:
        info.up_scale = 5;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res6X:
        info.up_scale = 6;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res7X:
        info.up_scale = 7;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res8X:
        info.up_scale = 8;
        info.down_shift = 0;
        break;
    default:
        ASSERT(false);
        info.up_scale = 1;
        info.down_shift = 0;
        break;
    }
    info.up_factor = static_cast<f32>(info.up_scale) / (1U << info.down_shift);
    info.down_factor = static_cast<f32>(1U << info.down_shift) / info.up_scale;
    info.active = info.up_scale != 1 || info.down_shift != 0;
}

void UpdateRescalingInfo()
{
    const auto setup = videoSettings.resolution_setup;
    auto & info = videoSettings.resolution_info;
    TranslateResolutionInfo(setup, info);

    g_settings->SetDefaultFloat(NXVideoSetting::ResolutionUpFactor, 1.0f);
    g_settings->SetFloat(NXVideoSetting::ResolutionUpFactor, info.up_factor);
}

} // namespace Settings

namespace
{
enum class SettingType
{
    Boolean,
    IntValue,
    IntValueRanged,
    RendererBackend,
    ShaderBackend,
    AstcDecodeMode,
    VSyncMode,
    NvdecEmulation,
    FullscreenMode,
    AspectRatio,
    ResolutionSetup,
    ScalingFilter,
    AntiAliasing,
    GpuAccuracy,
    AnisotropyMode,
    AstcRecompression,
    VramUsageMode,
};

class VideoSetting
{
public:
    VideoSetting(const char * id, const char * section, const char * key, bool * val, bool defaultValue, bool debugSetting = false);
    VideoSetting(const char * id, const char * section, const char * key, int32_t * val, int32_t defaultValue, bool debugSetting = false);
    VideoSetting(const char * id, const char * section, const char * key, int32_t * val, int32_t defaultValue, int32_t minValue, int32_t maxValue, bool debugSetting = false);
    VideoSetting(const char * id, const char * section, const char * key, RendererBackend * val, RendererBackend defaultValue, bool debugSetting = false);
    VideoSetting(const char * id, const char * section, const char * key, ShaderBackend * val, ShaderBackend defaultValue, bool debugSetting = false);
    VideoSetting(const char * id, const char * section, const char * key, AstcDecodeMode * val, AstcDecodeMode defaultValue, bool debugSetting = false);
    VideoSetting(const char * id, const char * section, const char * key, VSyncMode * val, VSyncMode defaultValue, bool debugSetting = false);
    VideoSetting(const char * id, const char * section, const char * key, NvdecEmulation * val, NvdecEmulation defaultValue, bool debugSetting = false);
    VideoSetting(const char * id, const char * section, const char * key, FullscreenMode * val, FullscreenMode defaultValue, bool debugSetting = false);
    VideoSetting(const char * id, const char * section, const char * key, AspectRatio * val, AspectRatio defaultValue, bool debugSetting = false);
    VideoSetting(const char * id, const char * section, const char * key, ResolutionSetup * val, ResolutionSetup defaultValue, bool debugSetting = false);
    VideoSetting(const char * id, const char * section, const char * key, ScalingFilter * val, ScalingFilter defaultValue, bool debugSetting = false);
    VideoSetting(const char * id, const char * section, const char * key, AntiAliasing * val, AntiAliasing defaultValue, bool debugSetting = false);
    VideoSetting(const char * id, const char * section, const char * key, GpuAccuracy * val, GpuAccuracy defaultValue, bool debugSetting = false);
    VideoSetting(const char * id, const char * section, const char * key, AnisotropyMode * val, AnisotropyMode defaultValue, bool debugSetting = false);
    VideoSetting(const char * id, const char * section, const char * key, AstcRecompression * val, AstcRecompression defaultValue, bool debugSetting = false);
    VideoSetting(const char * id, const char * section, const char * key, VramUsageMode * val, VramUsageMode defaultValue, bool debugSetting = false);
    VideoSetting(const char * id, const char * section, const char * key, bool * val, const char * fallbackId);
    VideoSetting(const char * id, const char * section, const char * key, int32_t * val, const char * fallbackId);
    VideoSetting(const char * id, const char * section, const char * key, AstcDecodeMode * val, const char * fallbackId);
    VideoSetting(const char * id, const char * section, const char * key, VSyncMode * val, const char * fallbackId);
    VideoSetting(const char * id, const char * section, const char * key, NvdecEmulation * val, const char * fallbackId);
    VideoSetting(const char * id, const char * section, const char * key, AspectRatio * val, const char * fallbackId);
    VideoSetting(const char * id, const char * section, const char * key, ResolutionSetup * val, const char * fallbackId);
    VideoSetting(const char * id, const char * section, const char * key, ScalingFilter * val, const char * fallbackId);
    VideoSetting(const char * id, const char * section, const char * key, AntiAliasing * val, const char * fallbackId);
    VideoSetting(const char * id, const char * section, const char * key, GpuAccuracy * val, const char * fallbackId);
    VideoSetting(const char * id, const char * section, const char * key, AnisotropyMode * val, const char * fallbackId);
    VideoSetting(const char * id, const char * section, const char * key, AstcRecompression * val, const char * fallbackId);
    VideoSetting(const char * id, const char * section, const char * key, VramUsageMode * val, const char * fallbackId);

    const char * identifier;
    const char * json_section;
    const char * json_key;
    SettingType settingType;
    bool debugSetting;
    const char * fallback{nullptr};
    bool gameValue{false};
    int32_t minValue{};
    int32_t maxValue{};
    union
    {
        bool boolValue;
        int32_t intValue;
        RendererBackend rendererBackend;
        ShaderBackend shaderBackend;
        AstcDecodeMode astcDecodeMode;
        VSyncMode vSyncMode;
        NvdecEmulation nvdecEmulation;
        FullscreenMode fullscreenMode;
        AspectRatio aspectRatio;
        ResolutionSetup resolutionSetup;
        ScalingFilter scalingFilter;
        AntiAliasing antiAliasing;
        GpuAccuracy gpuAccuracy;
        AnisotropyMode anisotropyMode;
        AstcRecompression astcRecompression;
        VramUsageMode vramUsageMode;
    } defaults;
    union
    {
        bool * boolValue;
        int32_t * intValue;
        RendererBackend * rendererBackend;
        ShaderBackend * shaderBackend;
        AstcDecodeMode * astcDecodeMode;
        VSyncMode * vSyncMode;
        NvdecEmulation * nvdecEmulation;
        FullscreenMode * fullscreenMode;
        AspectRatio * aspectRatio;
        ResolutionSetup * resolutionSetup;
        ScalingFilter * scalingFilter;
        AntiAliasing * antiAliasing;
        GpuAccuracy * gpuAccuracy;
        AnisotropyMode * anisotropyMode;
        AstcRecompression * astcRecompression;
        VramUsageMode * vramUsageMode;
    } value;
};

static VideoSetting settings[] = {
    {NXVideoSetting::GraphicsAPI, "video", "renderer_backend", &videoSettings.renderer_backend, RendererBackend::Vulkan},
    {NXVideoSetting::ShaderBackend, "video", "shader_backend", &videoSettings.shader_backend, ShaderBackend::Glsl},
    {NXVideoSetting::VulkanDevice, "video", "vulkan_device", &videoSettings.vulkan_device, 0},
    {NXVideoSetting::SystemUseDiskPipelineCache, "video", "use_disk_shader_cache", &videoSettings.global_use_disk_shader_cache, true},
    {NXVideoSetting::RecordShaderRecipes, "video", "record_shader_recipes", &videoSettings.record_shader_recipes, true},
    {NXVideoSetting::SystemUseAsynchronousGPUEmulation, "video", "use_asynchronous_gpu_emulation", &videoSettings.global_use_asynchronous_gpu_emulation, true, true},
#ifdef ANDROID
    {NXVideoSetting::SystemAstcDecodeMode, "video", "astc_decode_mode", &videoSettings.global_accelerate_astc, AstcDecodeMode::Cpu},
    {NXVideoSetting::SystemEnableAsynchronousPresentation, "video", "async_presentation", &videoSettings.global_async_presentation, true, true},
    {NXVideoSetting::SystemEnableReactiveFlushing, "video", "use_reactive_flushing", &videoSettings.global_use_reactive_flushing, false},
    {NXVideoSetting::SystemAccuracyLevel, "video", "accuracy_level", &videoSettings.global_gpu_accuracy, GpuAccuracy::Normal},
    {NXVideoSetting::SystemAnisotropicFiltering, "video", "anisotropic_filtering", &videoSettings.global_max_anisotropy, AnisotropyMode::Default},
#else
    {NXVideoSetting::SystemAstcDecodeMode, "video", "astc_decode_mode", &videoSettings.global_accelerate_astc, AstcDecodeMode::Gpu},
    {NXVideoSetting::SystemEnableAsynchronousPresentation, "video", "async_presentation", &videoSettings.global_async_presentation, true, true},
    {NXVideoSetting::SystemEnableReactiveFlushing, "video", "use_reactive_flushing", &videoSettings.global_use_reactive_flushing, true, true},
    {NXVideoSetting::SystemAccuracyLevel, "video", "accuracy_level", &videoSettings.global_gpu_accuracy, GpuAccuracy::High},
    {NXVideoSetting::SystemAnisotropicFiltering, "video", "anisotropic_filtering", &videoSettings.global_max_anisotropy, AnisotropyMode::Automatic},
#endif
    {NXVideoSetting::SystemDMAAccuracy, "video", "dma_accuracy", &videoSettings.global_dma_accuracy, 0, 0, 2, true},
    {NXVideoSetting::SystemGpuCommandSynchronization, "video", "gpu_command_synchronization", &videoSettings.global_gpu_command_synchronization, false, true},
    {NXVideoSetting::FastGpuReadback, "video", "fast_gpu_readback", &videoSettings.fast_gpu_readback, true, true},
    {NXVideoSetting::SystemVSyncMode, "video", "vsync_mode", &videoSettings.global_vsync_mode, VSyncMode::Fifo},
    {NXVideoSetting::SystemNvdecEmulation, "video", "nvdec_emulation", &videoSettings.global_nvdec_emulation, NvdecEmulation::Gpu, true},
#ifdef _WIN32
    {NXVideoSetting::FullscreenMode, "video", "fullscreen_mode", &videoSettings.fullscreen_mode, FullscreenMode::Borderless},
#else
    {NXVideoSetting::FullscreenMode, "video", "fullscreen_mode", &videoSettings.fullscreen_mode, FullscreenMode::Exclusive},
#endif
    {NXVideoSetting::SystemAspectRatio, "video", "aspect_ratio", &videoSettings.global_aspect_ratio, AspectRatio::R16_9},
    {NXVideoSetting::SystemResolutionSetup, "video", "resolution_setup", &videoSettings.global_resolution_setup, ResolutionSetup::Res1X},
    {NXVideoSetting::SystemScalingFilter, "video", "scaling_filter", &videoSettings.global_scaling_filter, ScalingFilter::Bilinear},
    {NXVideoSetting::CustomScalingFilter, "video", "custom_scaling_filter", &videoSettings.custom_scaling_filter, static_cast<int32_t>(ScalingFilter::Bilinear), 0, static_cast<int32_t>(ScalingFilter::ScaleForceFast)},
    {NXVideoSetting::SystemAntiAliasing, "video", "anti_aliasing", &videoSettings.global_anti_aliasing, AntiAliasing::None},
    {NXVideoSetting::SystemFSPSharpness, "video", "fsr_sharpening_slider", &videoSettings.global_fsr_sharpening_slider, FsrSharpness::ToStored(80), 0, FsrSharpness::StoredMaximum},
    {NXVideoSetting::DownscalingFilter, "video", "downscaling_filter", &videoSettings.downscaling_filter, 0, 0, 1},
    {NXVideoSetting::CASSharpness, "video", "cas_sharpening_slider", &videoSettings.cas_sharpening_slider, 80, 0, 100},
    {NXVideoSetting::FsrLegacyBenchmark, "video", "fsr_legacy_benchmark", &videoSettings.fsr_legacy_benchmark, false},
    {NXVideoSetting::SystemForceMaximumClocks, "video", "renderer_force_max_clock", &videoSettings.global_renderer_force_max_clock, false},
    {NXVideoSetting::SystemUseAsynchronousShaderBuilding, "video", "use_asynchronous_shaders", &videoSettings.global_use_asynchronous_shaders, true},
    {NXVideoSetting::NXShaderWarmupMode, "video", "nxshader_warmup_mode", &videoSettings.nxshader_warmup_mode, 0, 0, 1},
    {NXVideoSetting::NXShaderInitialMinimum, "video", "nxshader_initial_minimum", &videoSettings.nxshader_initial_minimum, 512, 0, 8192, true},
    {NXVideoSetting::NXShaderInitialSeconds, "video", "nxshader_initial_seconds", &videoSettings.nxshader_initial_seconds, 60, 0, 600, true},
    {NXVideoSetting::SystemFastGPUTime, "video", "use_fast_gpu_time", &videoSettings.global_use_fast_gpu_time, true},
    {NXVideoSetting::SystemUseVulkanPipelineCache, "video", "use_vulkan_driver_pipeline_cache", &videoSettings.global_use_vulkan_driver_pipeline_cache, true, true},
    {NXVideoSetting::SystemSyncToFramerateOfVideoPlayback, "video", "use_video_framerate", &videoSettings.global_use_video_framerate, false},
    {NXVideoSetting::SystemBarrierFeedbackLoops, "video", "barrier_feedback_loops", &videoSettings.global_barrier_feedback_loops, true, true},
    {NXVideoSetting::SystemASTCRecompressionMethod, "video", "astc_recompression_method", &videoSettings.global_astc_recompression, AstcRecompression::Uncompressed},
    {NXVideoSetting::SystemVRAMUsageMode, "video", "vram_usage_mode", &videoSettings.global_vram_usage_mode, VramUsageMode::Conservative},
    {NXVideoSetting::GameUseDiskPipelineCache, "video", "use_disk_shader_cache", &videoSettings.use_disk_shader_cache, NXVideoSetting::SystemUseDiskPipelineCache},
    {NXVideoSetting::GameUseAsynchronousGPUEmulation, "video", "use_asynchronous_gpu_emulation", &videoSettings.use_asynchronous_gpu_emulation, NXVideoSetting::SystemUseAsynchronousGPUEmulation},
    {NXVideoSetting::GameAstcDecodeMode, "video", "astc_decode_mode", &videoSettings.accelerate_astc, NXVideoSetting::SystemAstcDecodeMode},
    {NXVideoSetting::GameEnableAsynchronousPresentation, "video", "async_presentation", &videoSettings.async_presentation, NXVideoSetting::SystemEnableAsynchronousPresentation},
    {NXVideoSetting::GameEnableReactiveFlushing, "video", "use_reactive_flushing", &videoSettings.use_reactive_flushing, NXVideoSetting::SystemEnableReactiveFlushing},
    {NXVideoSetting::GameAccuracyLevel, "video", "accuracy_level", &videoSettings.gpu_accuracy, NXVideoSetting::SystemAccuracyLevel},
    {NXVideoSetting::GameAnisotropicFiltering, "video", "anisotropic_filtering", &videoSettings.max_anisotropy, NXVideoSetting::SystemAnisotropicFiltering},
    {NXVideoSetting::GameDMAAccuracy, "video", "dma_accuracy", &videoSettings.dma_accuracy, NXVideoSetting::SystemDMAAccuracy},
    {NXVideoSetting::GameGpuCommandSynchronization, "video", "gpu_command_synchronization", &videoSettings.gpu_command_synchronization, NXVideoSetting::SystemGpuCommandSynchronization},
    {NXVideoSetting::GameVSyncMode, "video", "vsync_mode", &videoSettings.vsync_mode, NXVideoSetting::SystemVSyncMode},
    {NXVideoSetting::GameNvdecEmulation, "video", "nvdec_emulation", &videoSettings.nvdec_emulation, NXVideoSetting::SystemNvdecEmulation},
    {NXVideoSetting::GameAspectRatio, "video", "aspect_ratio", &videoSettings.aspect_ratio, NXVideoSetting::SystemAspectRatio},
    {NXVideoSetting::GameResolutionSetup, "video", "resolution_setup", &videoSettings.resolution_setup, NXVideoSetting::SystemResolutionSetup},
    {NXVideoSetting::GameScalingFilter, "video", "scaling_filter", &videoSettings.scaling_filter, NXVideoSetting::SystemScalingFilter},
    {NXVideoSetting::GameAntiAliasing, "video", "anti_aliasing", &videoSettings.anti_aliasing, NXVideoSetting::SystemAntiAliasing},
    {NXVideoSetting::GameFSPSharpness, "video", "fsr_sharpening_slider", &videoSettings.fsr_sharpening_slider, NXVideoSetting::SystemFSPSharpness},
    {NXVideoSetting::GameForceMaximumClocks, "video", "renderer_force_max_clock", &videoSettings.renderer_force_max_clock, NXVideoSetting::SystemForceMaximumClocks},
    {NXVideoSetting::GameUseAsynchronousShaderBuilding, "video", "use_asynchronous_shaders", &videoSettings.use_asynchronous_shaders, NXVideoSetting::SystemUseAsynchronousShaderBuilding},
    {NXVideoSetting::GameFastGPUTime, "video", "use_fast_gpu_time", &videoSettings.use_fast_gpu_time, NXVideoSetting::SystemFastGPUTime},
    {NXVideoSetting::GameUseVulkanPipelineCache, "video", "use_vulkan_driver_pipeline_cache", &videoSettings.use_vulkan_driver_pipeline_cache, NXVideoSetting::SystemUseVulkanPipelineCache},
    {NXVideoSetting::GameSyncToFramerateOfVideoPlayback, "video", "use_video_framerate", &videoSettings.use_video_framerate, NXVideoSetting::SystemSyncToFramerateOfVideoPlayback},
    {NXVideoSetting::GameBarrierFeedbackLoops, "video", "barrier_feedback_loops", &videoSettings.barrier_feedback_loops, NXVideoSetting::SystemBarrierFeedbackLoops},
    {NXVideoSetting::GameASTCRecompressionMethod, "video", "astc_recompression_method", &videoSettings.astc_recompression, NXVideoSetting::SystemASTCRecompressionMethod},
    {NXVideoSetting::GameVRAMUsageMode, "video", "vram_usage_mode", &videoSettings.vram_usage_mode, NXVideoSetting::SystemVRAMUsageMode},
};
} // namespace

namespace
{
StoreKind StoreKindOf(const VideoSetting & videoSetting)
{
    return videoSetting.settingType == SettingType::Boolean ? StoreKind::Bool : StoreKind::Int;
}

bool IsPersisted(const VideoSetting & /*videoSetting*/)
{
    return true;
}

JsonValue JsonSettingValue(const VideoSetting & videoSetting, const JsonValue & root)
{
    const JsonValue & section = root[videoSetting.json_section];
    return section.isObject() ? section[videoSetting.json_key] : JsonValue();
}

void SetJsonSettingValue(const VideoSetting & videoSetting, JsonValue & root, const JsonValue & value)
{
    root[videoSetting.json_section][videoSetting.json_key] = value;
}

void InheritFallback(VideoSetting & videoSetting, const VideoSetting & fallbackSetting)
{
    videoSetting.defaults = fallbackSetting.defaults;
    videoSetting.debugSetting = fallbackSetting.debugSetting;
    videoSetting.minValue = fallbackSetting.minValue;
    videoSetting.maxValue = fallbackSetting.maxValue;
}

void ApplyRangedInt(VideoSetting & videoSetting, int32_t value)
{
    if (value == static_cast<int32_t>(RetiredMetalFxFilter) &&
        strcmp(videoSetting.identifier, NXVideoSetting::CustomScalingFilter) == 0)
    {
        value = static_cast<int32_t>(ScalingFilter::Fsr);
    }
    *videoSetting.value.intValue = std::clamp(value, videoSetting.minValue, videoSetting.maxValue);
}

void LoadStoreSetting(const VideoSetting & videoSetting, const char * setting)
{
    switch (videoSetting.settingType)
    {
    case SettingType::Boolean:
        *videoSetting.value.boolValue = g_settings->GetBool(setting);
        break;
    case SettingType::IntValue:
        *videoSetting.value.intValue = g_settings->GetInt(setting);
        break;
    case SettingType::IntValueRanged:
        ApplyRangedInt(const_cast<VideoSetting &>(videoSetting), g_settings->GetInt(setting));
        break;
    case SettingType::RendererBackend:
        *videoSetting.value.rendererBackend = (RendererBackend)g_settings->GetInt(setting);
        break;
    case SettingType::ShaderBackend:
        *videoSetting.value.shaderBackend = (ShaderBackend)g_settings->GetInt(setting);
        break;
    case SettingType::AstcDecodeMode:
        *videoSetting.value.astcDecodeMode = (AstcDecodeMode)g_settings->GetInt(setting);
        break;
    case SettingType::VSyncMode:
        *videoSetting.value.vSyncMode = (VSyncMode)g_settings->GetInt(setting);
        break;
    case SettingType::NvdecEmulation:
        *videoSetting.value.nvdecEmulation = (NvdecEmulation)g_settings->GetInt(setting);
        break;
    case SettingType::FullscreenMode:
        *videoSetting.value.fullscreenMode = (FullscreenMode)g_settings->GetInt(setting);
        break;
    case SettingType::AspectRatio:
        *videoSetting.value.aspectRatio = (AspectRatio)g_settings->GetInt(setting);
        break;
    case SettingType::ResolutionSetup:
        *videoSetting.value.resolutionSetup = (ResolutionSetup)g_settings->GetInt(setting);
        break;
    case SettingType::ScalingFilter:
        *videoSetting.value.scalingFilter = (ScalingFilter)g_settings->GetInt(setting);
        break;
    case SettingType::AntiAliasing:
        *videoSetting.value.antiAliasing = (AntiAliasing)g_settings->GetInt(setting);
        break;
    case SettingType::GpuAccuracy:
        *videoSetting.value.gpuAccuracy = (GpuAccuracy)g_settings->GetInt(setting);
        break;
    case SettingType::AnisotropyMode:
        *videoSetting.value.anisotropyMode = (AnisotropyMode)g_settings->GetInt(setting);
        break;
    case SettingType::AstcRecompression:
        *videoSetting.value.astcRecompression = (AstcRecompression)g_settings->GetInt(setting);
        break;
    case SettingType::VramUsageMode:
        *videoSetting.value.vramUsageMode = (VramUsageMode)g_settings->GetInt(setting);
        break;
    default:
        UNIMPLEMENTED();
    }
}

} // namespace

void VideoSettingChanged(const char * setting, void * /*userData*/)
{
    ModuleSettingTable::SettingChanged(settings, setting);
    if (strcmp(setting, NXVideoSetting::GameDMAAccuracy) == 0)
    {
        runtime_dma_accuracy.store(videoSettings.dma_accuracy, std::memory_order_relaxed);
        return;
    }
    if (strcmp(setting, NXVideoSetting::FsrLegacyBenchmark) == 0)
    {
        runtime_fsr_legacy.store(videoSettings.fsr_legacy_benchmark, std::memory_order_relaxed);
        return;
    }
    if (strcmp(setting, NXVideoSetting::GameGpuCommandSynchronization) == 0)
    {
        runtime_gpu_command_synchronization.store(videoSettings.gpu_command_synchronization, std::memory_order_relaxed);
        return;
    }
    if (strcmp(setting, NXVideoSetting::FastGpuReadback) == 0)
    {
        runtime_fast_gpu_readback.store(videoSettings.fast_gpu_readback, std::memory_order_relaxed);
        return;
    }
    Settings::UpdateRescalingInfo();
    Settings::UpdateGPUAccuracy();
}

namespace
{
void ApplyDefault(const VideoSetting & videoSetting)
{
    switch (videoSetting.settingType)
    {
    case SettingType::Boolean:
        *videoSetting.value.boolValue = videoSetting.defaults.boolValue;
        break;
    case SettingType::IntValue:
    case SettingType::IntValueRanged:
        *videoSetting.value.intValue = videoSetting.defaults.intValue;
        break;
    case SettingType::RendererBackend:
        *videoSetting.value.rendererBackend = videoSetting.defaults.rendererBackend;
        break;
    case SettingType::ShaderBackend:
        *videoSetting.value.shaderBackend = videoSetting.defaults.shaderBackend;
        break;
    case SettingType::AstcDecodeMode:
        *videoSetting.value.astcDecodeMode = videoSetting.defaults.astcDecodeMode;
        break;
    case SettingType::VSyncMode:
        *videoSetting.value.vSyncMode = videoSetting.defaults.vSyncMode;
        break;
    case SettingType::NvdecEmulation:
        *videoSetting.value.nvdecEmulation = videoSetting.defaults.nvdecEmulation;
        break;
    case SettingType::FullscreenMode:
        *videoSetting.value.fullscreenMode = videoSetting.defaults.fullscreenMode;
        break;
    case SettingType::AspectRatio:
        *videoSetting.value.aspectRatio = videoSetting.defaults.aspectRatio;
        break;
    case SettingType::ResolutionSetup:
        *videoSetting.value.resolutionSetup = videoSetting.defaults.resolutionSetup;
        break;
    case SettingType::ScalingFilter:
        *videoSetting.value.scalingFilter = videoSetting.defaults.scalingFilter;
        break;
    case SettingType::AntiAliasing:
        *videoSetting.value.antiAliasing = videoSetting.defaults.antiAliasing;
        break;
    case SettingType::GpuAccuracy:
        *videoSetting.value.gpuAccuracy = videoSetting.defaults.gpuAccuracy;
        break;
    case SettingType::AnisotropyMode:
        *videoSetting.value.anisotropyMode = videoSetting.defaults.anisotropyMode;
        break;
    case SettingType::AstcRecompression:
        *videoSetting.value.astcRecompression = videoSetting.defaults.astcRecompression;
        break;
    case SettingType::VramUsageMode:
        *videoSetting.value.vramUsageMode = videoSetting.defaults.vramUsageMode;
        break;
    default:
        UNIMPLEMENTED();
    }
}

void ReadJsonSetting(const VideoSetting & videoSetting, const JsonValue & root)
{
    const JsonValue value = JsonSettingValue(videoSetting, root);
    switch (videoSetting.settingType)
    {
    case SettingType::Boolean:
        if (value.isBool())
        {
            *videoSetting.value.boolValue = value.asBool();
        }
        break;
    case SettingType::IntValue:
        if (value.isInt())
        {
            *videoSetting.value.intValue = (int32_t)value.asInt64();
        }
        break;
    case SettingType::IntValueRanged:
        if (value.isInt())
        {
            ApplyRangedInt(const_cast<VideoSetting &>(videoSetting), (int32_t)value.asInt64());
        }
        break;
    case SettingType::RendererBackend:
        if (value.isString())
        {
            *videoSetting.value.rendererBackend = RendererBackendFromString(value.asString());
        }
        break;
    case SettingType::ShaderBackend:
        if (value.isString())
        {
            *videoSetting.value.shaderBackend = ShaderBackendFromString(value.asString());
        }
        break;
    case SettingType::AstcDecodeMode:
        if (value.isString())
        {
            *videoSetting.value.astcDecodeMode = AstcDecodeModeFromString(value.asString());
        }
        break;
    case SettingType::VSyncMode:
        if (value.isString())
        {
            *videoSetting.value.vSyncMode = VSyncModeFromString(value.asString());
        }
        break;
    case SettingType::NvdecEmulation:
        if (value.isString())
        {
            *videoSetting.value.nvdecEmulation = NvdecEmulationFromString(value.asString());
        }
        break;
    case SettingType::FullscreenMode:
        if (value.isString())
        {
            *videoSetting.value.fullscreenMode = FullscreenModeFromString(value.asString());
        }
        break;
    case SettingType::AspectRatio:
        if (value.isString())
        {
            *videoSetting.value.aspectRatio = AspectRatioFromString(value.asString());
        }
        break;
    case SettingType::ResolutionSetup:
        if (value.isString())
        {
            *videoSetting.value.resolutionSetup = ResolutionSetupFromString(value.asString());
        }
        break;
    case SettingType::ScalingFilter:
        if (value.isString())
        {
            *videoSetting.value.scalingFilter = ScalingFilterFromString(value.asString());
        }
        break;
    case SettingType::AntiAliasing:
        if (value.isString())
        {
            *videoSetting.value.antiAliasing = AntiAliasingFromString(value.asString());
        }
        break;
    case SettingType::GpuAccuracy:
        if (value.isString())
        {
            *videoSetting.value.gpuAccuracy = GpuAccuracyFromString(value.asString());
        }
        break;
    case SettingType::AnisotropyMode:
        if (value.isString())
        {
            *videoSetting.value.anisotropyMode = AnisotropyModeFromString(value.asString());
        }
        break;
    case SettingType::AstcRecompression:
        if (value.isString())
        {
            *videoSetting.value.astcRecompression = AstcRecompressionFromString(value.asString());
        }
        break;
    case SettingType::VramUsageMode:
        if (value.isString())
        {
            *videoSetting.value.vramUsageMode = VramUsageModeFromString(value.asString());
        }
        break;
    default:
        UNIMPLEMENTED();
    }
}

void StoreSetting(const VideoSetting & videoSetting)
{
    switch (videoSetting.settingType)
    {
    case SettingType::Boolean:
        g_settings->SetDefaultBool(videoSetting.identifier, videoSetting.defaults.boolValue);
        g_settings->SetBool(videoSetting.identifier, *videoSetting.value.boolValue);
        break;
    case SettingType::IntValue:
        g_settings->SetDefaultInt(videoSetting.identifier, videoSetting.defaults.intValue);
        g_settings->SetInt(videoSetting.identifier, *videoSetting.value.intValue);
        break;
    case SettingType::IntValueRanged:
        g_settings->SetDefaultInt(videoSetting.identifier, videoSetting.defaults.intValue);
        ApplyRangedInt(const_cast<VideoSetting &>(videoSetting), *videoSetting.value.intValue);
        g_settings->SetInt(videoSetting.identifier, *videoSetting.value.intValue);
        break;
    case SettingType::RendererBackend:
        g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.defaults.rendererBackend);
        g_settings->SetInt(videoSetting.identifier, (int32_t)*videoSetting.value.rendererBackend);
        break;
    case SettingType::ShaderBackend:
        g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.defaults.shaderBackend);
        g_settings->SetInt(videoSetting.identifier, (int32_t)*videoSetting.value.shaderBackend);
        break;
    case SettingType::AstcDecodeMode:
        g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.defaults.astcDecodeMode);
        g_settings->SetInt(videoSetting.identifier, (int32_t)*videoSetting.value.astcDecodeMode);
        break;
    case SettingType::VSyncMode:
        g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.defaults.vSyncMode);
        g_settings->SetInt(videoSetting.identifier, (int32_t)*videoSetting.value.vSyncMode);
        break;
    case SettingType::NvdecEmulation:
        g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.defaults.nvdecEmulation);
        g_settings->SetInt(videoSetting.identifier, (int32_t)*videoSetting.value.nvdecEmulation);
        break;
    case SettingType::FullscreenMode:
        g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.defaults.fullscreenMode);
        g_settings->SetInt(videoSetting.identifier, (int32_t)*videoSetting.value.fullscreenMode);
        break;
    case SettingType::AspectRatio:
        g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.defaults.aspectRatio);
        g_settings->SetInt(videoSetting.identifier, (int32_t)*videoSetting.value.aspectRatio);
        break;
    case SettingType::ResolutionSetup:
        g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.defaults.resolutionSetup);
        g_settings->SetInt(videoSetting.identifier, (int32_t)*videoSetting.value.resolutionSetup);
        break;
    case SettingType::ScalingFilter:
        g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.defaults.scalingFilter);
        g_settings->SetInt(videoSetting.identifier, (int32_t)*videoSetting.value.scalingFilter);
        break;
    case SettingType::AntiAliasing:
        g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.defaults.antiAliasing);
        g_settings->SetInt(videoSetting.identifier, (int32_t)*videoSetting.value.antiAliasing);
        break;
    case SettingType::GpuAccuracy:
        g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.defaults.gpuAccuracy);
        g_settings->SetInt(videoSetting.identifier, (int32_t)*videoSetting.value.gpuAccuracy);
        break;
    case SettingType::AnisotropyMode:
        g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.defaults.anisotropyMode);
        g_settings->SetInt(videoSetting.identifier, (int32_t)*videoSetting.value.anisotropyMode);
        break;
    case SettingType::AstcRecompression:
        g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.defaults.astcRecompression);
        g_settings->SetInt(videoSetting.identifier, (int32_t)*videoSetting.value.astcRecompression);
        break;
    case SettingType::VramUsageMode:
        g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.defaults.vramUsageMode);
        g_settings->SetInt(videoSetting.identifier, (int32_t)*videoSetting.value.vramUsageMode);
        break;
    default:
        UNIMPLEMENTED();
    }
}

void WriteJsonSetting(const VideoSetting & videoSetting, JsonValue & root, bool always)
{
    switch (videoSetting.settingType)
    {
    case SettingType::Boolean:
        if (always || *videoSetting.value.boolValue != videoSetting.defaults.boolValue)
        {
            SetJsonSettingValue(videoSetting, root, *videoSetting.value.boolValue);
        }
        break;
    case SettingType::IntValue:
    case SettingType::IntValueRanged:
        if (always || *videoSetting.value.intValue != videoSetting.defaults.intValue)
        {
            SetJsonSettingValue(videoSetting, root, *videoSetting.value.intValue);
        }
        break;
    case SettingType::RendererBackend:
        if (always || *videoSetting.value.rendererBackend != videoSetting.defaults.rendererBackend)
        {
            SetJsonSettingValue(videoSetting, root, RendererBackendToString(*videoSetting.value.rendererBackend));
        }
        break;
    case SettingType::ShaderBackend:
        if (always || *videoSetting.value.shaderBackend != videoSetting.defaults.shaderBackend)
        {
            SetJsonSettingValue(videoSetting, root, ShaderBackendToString(*videoSetting.value.shaderBackend));
        }
        break;
    case SettingType::AstcDecodeMode:
        if (always || *videoSetting.value.astcDecodeMode != videoSetting.defaults.astcDecodeMode)
        {
            SetJsonSettingValue(videoSetting, root, AstcDecodeModeToString(*videoSetting.value.astcDecodeMode));
        }
        break;
    case SettingType::VSyncMode:
        if (always || *videoSetting.value.vSyncMode != videoSetting.defaults.vSyncMode)
        {
            SetJsonSettingValue(videoSetting, root, VSyncModeToString(*videoSetting.value.vSyncMode));
        }
        break;
    case SettingType::NvdecEmulation:
        if (always || *videoSetting.value.nvdecEmulation != videoSetting.defaults.nvdecEmulation)
        {
            SetJsonSettingValue(videoSetting, root, NvdecEmulationToString(*videoSetting.value.nvdecEmulation));
        }
        break;
    case SettingType::FullscreenMode:
        if (always || *videoSetting.value.fullscreenMode != videoSetting.defaults.fullscreenMode)
        {
            SetJsonSettingValue(videoSetting, root, FullscreenModeToString(*videoSetting.value.fullscreenMode));
        }
        break;
    case SettingType::AspectRatio:
        if (always || *videoSetting.value.aspectRatio != videoSetting.defaults.aspectRatio)
        {
            SetJsonSettingValue(videoSetting, root, AspectRatioToString(*videoSetting.value.aspectRatio));
        }
        break;
    case SettingType::ResolutionSetup:
        if (always || *videoSetting.value.resolutionSetup != videoSetting.defaults.resolutionSetup)
        {
            SetJsonSettingValue(videoSetting, root, ResolutionSetupToString(*videoSetting.value.resolutionSetup));
        }
        break;
    case SettingType::ScalingFilter:
        if (always || *videoSetting.value.scalingFilter != videoSetting.defaults.scalingFilter)
        {
            SetJsonSettingValue(videoSetting, root, ScalingFilterToString(*videoSetting.value.scalingFilter));
        }
        break;
    case SettingType::AntiAliasing:
        if (always || *videoSetting.value.antiAliasing != videoSetting.defaults.antiAliasing)
        {
            SetJsonSettingValue(videoSetting, root, AntiAliasingToString(*videoSetting.value.antiAliasing));
        }
        break;
    case SettingType::GpuAccuracy:
        if (always || *videoSetting.value.gpuAccuracy != videoSetting.defaults.gpuAccuracy)
        {
            SetJsonSettingValue(videoSetting, root, GpuAccuracyToString(*videoSetting.value.gpuAccuracy));
        }
        break;
    case SettingType::AnisotropyMode:
        if (always || *videoSetting.value.anisotropyMode != videoSetting.defaults.anisotropyMode)
        {
            SetJsonSettingValue(videoSetting, root, AnisotropyModeToString(*videoSetting.value.anisotropyMode));
        }
        break;
    case SettingType::AstcRecompression:
        if (always || *videoSetting.value.astcRecompression != videoSetting.defaults.astcRecompression)
        {
            SetJsonSettingValue(videoSetting, root, AstcRecompressionToString(*videoSetting.value.astcRecompression));
        }
        break;
    case SettingType::VramUsageMode:
        if (always || *videoSetting.value.vramUsageMode != videoSetting.defaults.vramUsageMode)
        {
            SetJsonSettingValue(videoSetting, root, VramUsageModeToString(*videoSetting.value.vramUsageMode));
        }
        break;
    default:
        UNIMPLEMENTED();
    }
}

void GameSettingsChanged(const char * /*setting*/, void * /*userData*/)
{
    ModuleSettingTable::LoadGameSettings(settings, "nxemu-video");
}

} // namespace

void SetupVideoSetting(void)
{
    ModuleSettingTable::SetupSettings(settings, "nxemu-video", VideoSettingChanged, GameSettingsChanged);
    runtime_dma_accuracy.store(videoSettings.dma_accuracy, std::memory_order_relaxed);
    runtime_gpu_command_synchronization.store(videoSettings.gpu_command_synchronization, std::memory_order_relaxed);
    runtime_fast_gpu_readback.store(videoSettings.fast_gpu_readback, std::memory_order_relaxed);
    Settings::UpdateRescalingInfo();
    Settings::UpdateGPUAccuracy();
}

void CleanupVideoSetting(void)
{
    ModuleSettingTable::CleanupSettings(settings, VideoSettingChanged, GameSettingsChanged);
}

void SaveVideoSettings(void)
{
    ModuleSettingTable::SaveSettings(settings, "nxemu-video");
}

namespace
{
VideoSetting::VideoSetting(const char * id, const char * section, const char * key, bool * val, bool defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::Boolean),
    debugSetting(debugSetting)
{
    defaults.boolValue = defaultValue;
    value.boolValue = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, int32_t * val, int32_t defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::IntValue),
    debugSetting(debugSetting)
{
    defaults.intValue = defaultValue;
    value.intValue = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, int32_t * val, int32_t defaultValue, int32_t minValue, int32_t maxValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::IntValueRanged),
    debugSetting(debugSetting),
    minValue(minValue),
    maxValue(maxValue)
{
    defaults.intValue = defaultValue;
    value.intValue = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, RendererBackend * val, RendererBackend defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::RendererBackend),
    debugSetting(debugSetting)
{
    defaults.rendererBackend = defaultValue;
    value.rendererBackend = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, ShaderBackend * val, ShaderBackend defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::ShaderBackend),
    debugSetting(debugSetting)
{
    defaults.shaderBackend = defaultValue;
    value.shaderBackend = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, AstcDecodeMode * val, AstcDecodeMode defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::AstcDecodeMode),
    debugSetting(debugSetting)
{
    defaults.astcDecodeMode = defaultValue;
    value.astcDecodeMode = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, VSyncMode * val, VSyncMode defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::VSyncMode),
    debugSetting(debugSetting)
{
    defaults.vSyncMode = defaultValue;
    value.vSyncMode = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, NvdecEmulation * val, NvdecEmulation defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::NvdecEmulation),
    debugSetting(debugSetting)
{
    defaults.nvdecEmulation = defaultValue;
    value.nvdecEmulation = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, FullscreenMode * val, FullscreenMode defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::FullscreenMode),
    debugSetting(debugSetting)
{
    defaults.fullscreenMode = defaultValue;
    value.fullscreenMode = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, AspectRatio * val, AspectRatio defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::AspectRatio),
    debugSetting(debugSetting)
{
    defaults.aspectRatio = defaultValue;
    value.aspectRatio = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, ResolutionSetup * val, ResolutionSetup defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::ResolutionSetup),
    debugSetting(debugSetting)
{
    defaults.resolutionSetup = defaultValue;
    value.resolutionSetup = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, ScalingFilter * val, ScalingFilter defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::ScalingFilter),
    debugSetting(debugSetting)
{
    defaults.scalingFilter = defaultValue;
    value.scalingFilter = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, AntiAliasing * val, AntiAliasing defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::AntiAliasing),
    debugSetting(debugSetting)
{
    defaults.antiAliasing = defaultValue;
    value.antiAliasing = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, GpuAccuracy * val, GpuAccuracy defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::GpuAccuracy),
    debugSetting(debugSetting)
{
    defaults.gpuAccuracy = defaultValue;
    value.gpuAccuracy = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, AnisotropyMode * val, AnisotropyMode defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::AnisotropyMode),
    debugSetting(debugSetting)
{
    defaults.anisotropyMode = defaultValue;
    value.anisotropyMode = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, AstcRecompression * val, AstcRecompression defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::AstcRecompression),
    debugSetting(debugSetting)
{
    defaults.astcRecompression = defaultValue;
    value.astcRecompression = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, VramUsageMode * val, VramUsageMode defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::VramUsageMode),
    debugSetting(debugSetting)
{
    defaults.vramUsageMode = defaultValue;
    value.vramUsageMode = val;
}
VideoSetting::VideoSetting(const char * id, const char * section, const char * key, bool * val, const char * fallbackId) :
    VideoSetting(id, section, key, val, false)
{
    fallback = fallbackId;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, int32_t * val, const char * fallbackId) :
    VideoSetting(id, section, key, val, 0, 0, 0)
{
    fallback = fallbackId;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, AstcDecodeMode * val, const char * fallbackId) :
    VideoSetting(id, section, key, val, AstcDecodeMode{})
{
    fallback = fallbackId;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, VSyncMode * val, const char * fallbackId) :
    VideoSetting(id, section, key, val, VSyncMode{})
{
    fallback = fallbackId;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, NvdecEmulation * val, const char * fallbackId) :
    VideoSetting(id, section, key, val, NvdecEmulation{})
{
    fallback = fallbackId;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, AspectRatio * val, const char * fallbackId) :
    VideoSetting(id, section, key, val, AspectRatio{})
{
    fallback = fallbackId;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, ResolutionSetup * val, const char * fallbackId) :
    VideoSetting(id, section, key, val, ResolutionSetup{})
{
    fallback = fallbackId;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, ScalingFilter * val, const char * fallbackId) :
    VideoSetting(id, section, key, val, ScalingFilter{})
{
    fallback = fallbackId;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, AntiAliasing * val, const char * fallbackId) :
    VideoSetting(id, section, key, val, AntiAliasing{})
{
    fallback = fallbackId;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, GpuAccuracy * val, const char * fallbackId) :
    VideoSetting(id, section, key, val, GpuAccuracy{})
{
    fallback = fallbackId;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, AnisotropyMode * val, const char * fallbackId) :
    VideoSetting(id, section, key, val, AnisotropyMode{})
{
    fallback = fallbackId;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, AstcRecompression * val, const char * fallbackId) :
    VideoSetting(id, section, key, val, AstcRecompression{})
{
    fallback = fallbackId;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, VramUsageMode * val, const char * fallbackId) :
    VideoSetting(id, section, key, val, VramUsageMode{})
{
    fallback = fallbackId;
}
} // namespace
