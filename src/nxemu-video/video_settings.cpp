#include "video_settings.h"
#include "video_enum_strings.h"
#include "video_settings_identifiers.h"
#include <algorithm>
#include <common/json.h>
#include <nxemu-module-spec/base.h>
#include <yuzu_common/yuzu_assert.h>

extern IModuleSettings * g_settings;

VideoSettings videoSettings{};

namespace Settings
{

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
    VideoSetting(const char * id, const char * section, const char * key, bool * val, bool defaultValue);
    VideoSetting(const char * id, const char * section, const char * key, int32_t * val, int32_t defaultValue);
    VideoSetting(const char * id, const char * section, const char * key, int32_t * val, int32_t defaultValue, int32_t minValue, int32_t maxValue);
    VideoSetting(const char * id, const char * section, const char * key, RendererBackend * val, RendererBackend defaultValue);
    VideoSetting(const char * id, const char * section, const char * key, ShaderBackend * val, ShaderBackend defaultValue);
    VideoSetting(const char * id, const char * section, const char * key, AstcDecodeMode * val, AstcDecodeMode defaultValue);
    VideoSetting(const char * id, const char * section, const char * key, VSyncMode * val, VSyncMode defaultValue);
    VideoSetting(const char * id, const char * section, const char * key, NvdecEmulation * val, NvdecEmulation defaultValue);
    VideoSetting(const char * id, const char * section, const char * key, FullscreenMode * val, FullscreenMode defaultValue);
    VideoSetting(const char * id, const char * section, const char * key, AspectRatio * val, AspectRatio defaultValue);
    VideoSetting(const char * id, const char * section, const char * key, ResolutionSetup * val, ResolutionSetup defaultValue);
    VideoSetting(const char * id, const char * section, const char * key, ScalingFilter * val, ScalingFilter defaultValue);
    VideoSetting(const char * id, const char * section, const char * key, AntiAliasing * val, AntiAliasing defaultValue);
    VideoSetting(const char * id, const char * section, const char * key, GpuAccuracy * val, GpuAccuracy defaultValue);
    VideoSetting(const char * id, const char * section, const char * key, AnisotropyMode * val, AnisotropyMode defaultValue);
    VideoSetting(const char * id, const char * section, const char * key, AstcRecompression * val, AstcRecompression defaultValue);
    VideoSetting(const char * id, const char * section, const char * key, VramUsageMode * val, VramUsageMode defaultValue);

    const char * identifier;
    const char * json_section;
    const char * json_key;
    SettingType settingType;
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
    {NXVideoSetting::UseDiskPipelineCache, "video", "use_disk_shader_cache", &videoSettings.use_disk_shader_cache, true},
    {NXVideoSetting::UseAsynchronousGPUEmulation, "video", "use_asynchronous_gpu_emulation", &videoSettings.use_asynchronous_gpu_emulation, true},
#ifdef ANDROID
    {NXVideoSetting::AstcDecodeMode, "video", "astc_decode_mode", &videoSettings.accelerate_astc, AstcDecodeMode::Cpu},
    {NXVideoSetting::EnableAsynchronousPresentation, "video", "async_presentation", &videoSettings.async_presentation, true},
    {NXVideoSetting::EnableReactiveFlushing, "video", "use_reactive_flushing", &videoSettings.use_reactive_flushing, false},
    {NXVideoSetting::AccuracyLevel, "video", "accuracy_level", &videoSettings.gpu_accuracy, GpuAccuracy::Normal},
    {NXVideoSetting::AnisotropicFiltering, "video", "anisotropic_filtering", &videoSettings.max_anisotropy, AnisotropyMode::Default},
#else
    {NXVideoSetting::AstcDecodeMode, "video", "astc_decode_mode", &videoSettings.accelerate_astc, AstcDecodeMode::Gpu},
    {NXVideoSetting::EnableAsynchronousPresentation, "video", "async_presentation", &videoSettings.async_presentation, false},
    {NXVideoSetting::EnableReactiveFlushing, "video", "use_reactive_flushing", &videoSettings.use_reactive_flushing, true},
    {NXVideoSetting::AccuracyLevel, "video", "accuracy_level", &videoSettings.gpu_accuracy, GpuAccuracy::High},
    {NXVideoSetting::AnisotropicFiltering, "video", "anisotropic_filtering", &videoSettings.max_anisotropy, AnisotropyMode::Automatic},
#endif
    {NXVideoSetting::VSyncMode, "video", "vsync_mode", &videoSettings.vsync_mode, VSyncMode::Fifo},
    {NXVideoSetting::NvdecEmulation, "video", "nvdec_emulation", &videoSettings.nvdec_emulation, NvdecEmulation::Gpu},
#ifdef _WIN32
    {NXVideoSetting::FullscreenMode, "video", "fullscreen_mode", &videoSettings.fullscreen_mode, FullscreenMode::Borderless},
#else
    {NXVideoSetting::FullscreenMode, "video", "fullscreen_mode", &videoSettings.fullscreen_mode, FullscreenMode::Exclusive},
#endif
    {NXVideoSetting::AspectRatio, "video", "aspect_ratio", &videoSettings.aspect_ratio, AspectRatio::R16_9},
    {NXVideoSetting::ResolutionSetup, "video", "resolution_setup", &videoSettings.resolution_setup, ResolutionSetup::Res1X},
    {NXVideoSetting::ScalingFilter, "video", "scaling_filter", &videoSettings.scaling_filter, ScalingFilter::Bilinear},
    {NXVideoSetting::AntiAliasing, "video", "anti_aliasing", &videoSettings.anti_aliasing, AntiAliasing::None},
    {NXVideoSetting::FSPSharpness, "video", "fsr_sharpening_slider", &videoSettings.fsr_sharpening_slider, 25, 0, 200},
    {NXVideoSetting::ForceMaximumClocks, "video", "renderer_force_max_clock", &videoSettings.renderer_force_max_clock, false},
    {NXVideoSetting::UseAsynchronousShaderBuilding, "video", "use_asynchronous_shaders", &videoSettings.use_asynchronous_shaders, false},
    {NXVideoSetting::FastGPUTime, "video", "use_fast_gpu_time", &videoSettings.use_fast_gpu_time, true},
    {NXVideoSetting::UseVulkanPipelineCache, "video", "use_vulkan_driver_pipeline_cache", &videoSettings.use_vulkan_driver_pipeline_cache, true},
    {NXVideoSetting::SyncToFramerateOfVideoPlayback, "video", "use_video_framerate", &videoSettings.use_video_framerate, false},
    {NXVideoSetting::BarrierFeedbackLoops, "video", "barrier_feedback_loops", &videoSettings.barrier_feedback_loops, true},
    {NXVideoSetting::ASTCRecompressionMethod, "video", "astc_recompression_method", &videoSettings.astc_recompression, AstcRecompression::Uncompressed},
    {NXVideoSetting::VRAMUsageMode, "video", "vram_usage_mode", &videoSettings.vram_usage_mode, VramUsageMode::Conservative},
};
} // namespace

namespace
{
void ApplyRangedInt(VideoSetting & videoSetting, int32_t value)
{
    *videoSetting.value.intValue = std::clamp(value, videoSetting.minValue, videoSetting.maxValue);
}

} // namespace

void VideoSettingChanged(const char * setting, void * /*userData*/)
{
    for (const VideoSetting & videoSetting : settings)
    {
        if (strcmp(videoSetting.identifier, setting) != 0)
        {
            continue;
        }
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
    Settings::UpdateRescalingInfo();
    Settings::UpdateGPUAccuracy();
}

void SetupVideoSetting(void)
{
    for (const VideoSetting & videoSetting : settings)
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

    JsonValue root;
    JsonReader reader;
    std::string json = g_settings->GetSectionSettings("nxemu-video");

    if (!json.empty() && reader.Parse(json.data(), json.data() + json.size(), root))
    {
        for (const VideoSetting & videoSetting : settings)
        {
            JsonValue section = root[videoSetting.json_section];
            if (!section.isObject())
            {
                continue;
            }
            JsonValue value = section[videoSetting.json_key];
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
    }

    for (const VideoSetting & videoSetting : settings)
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
        g_settings->RegisterCallback(videoSetting.identifier, VideoSettingChanged, nullptr);
    }
    Settings::UpdateRescalingInfo();
    Settings::UpdateGPUAccuracy();
}

void CleanupVideoSetting(void)
{
    for (const VideoSetting & videoSetting : settings)
    {
        g_settings->UnregisterCallback(videoSetting.identifier, VideoSettingChanged, nullptr);
    }
}

void SaveVideoSettings(void)
{
    typedef std::map<std::string, JsonValue> SectionMap;
    SectionMap sections;

    for (const VideoSetting & videoSetting : settings)
    {
        switch (videoSetting.settingType)
        {
        case SettingType::Boolean:
            if (*videoSetting.value.boolValue != videoSetting.defaults.boolValue)
            {
                sections[videoSetting.json_section][videoSetting.json_key] = *videoSetting.value.boolValue;
            }
            break;
        case SettingType::IntValue:
        case SettingType::IntValueRanged:
            if (*videoSetting.value.intValue != videoSetting.defaults.intValue)
            {
                sections[videoSetting.json_section][videoSetting.json_key] = *videoSetting.value.intValue;
            }
            break;
        case SettingType::RendererBackend:
            if (*videoSetting.value.rendererBackend != videoSetting.defaults.rendererBackend)
            {
                sections[videoSetting.json_section][videoSetting.json_key] = RendererBackendToString(*videoSetting.value.rendererBackend);
            }
            break;
        case SettingType::ShaderBackend:
            if (*videoSetting.value.shaderBackend != videoSetting.defaults.shaderBackend)
            {
                sections[videoSetting.json_section][videoSetting.json_key] = ShaderBackendToString(*videoSetting.value.shaderBackend);
            }
            break;
        case SettingType::AstcDecodeMode:
            if (*videoSetting.value.astcDecodeMode != videoSetting.defaults.astcDecodeMode)
            {
                sections[videoSetting.json_section][videoSetting.json_key] = AstcDecodeModeToString(*videoSetting.value.astcDecodeMode);
            }
            break;
        case SettingType::VSyncMode:
            if (*videoSetting.value.vSyncMode != videoSetting.defaults.vSyncMode)
            {
                sections[videoSetting.json_section][videoSetting.json_key] = VSyncModeToString(*videoSetting.value.vSyncMode);
            }
            break;
        case SettingType::NvdecEmulation:
            if (*videoSetting.value.nvdecEmulation != videoSetting.defaults.nvdecEmulation)
            {
                sections[videoSetting.json_section][videoSetting.json_key] = NvdecEmulationToString(*videoSetting.value.nvdecEmulation);
            }
            break;
        case SettingType::FullscreenMode:
            if (*videoSetting.value.fullscreenMode != videoSetting.defaults.fullscreenMode)
            {
                sections[videoSetting.json_section][videoSetting.json_key] = FullscreenModeToString(*videoSetting.value.fullscreenMode);
            }
            break;
        case SettingType::AspectRatio:
            if (*videoSetting.value.aspectRatio != videoSetting.defaults.aspectRatio)
            {
                sections[videoSetting.json_section][videoSetting.json_key] = AspectRatioToString(*videoSetting.value.aspectRatio);
            }
            break;
        case SettingType::ResolutionSetup:
            if (*videoSetting.value.resolutionSetup != videoSetting.defaults.resolutionSetup)
            {
                sections[videoSetting.json_section][videoSetting.json_key] = ResolutionSetupToString(*videoSetting.value.resolutionSetup);
            }
            break;
        case SettingType::ScalingFilter:
            if (*videoSetting.value.scalingFilter != videoSetting.defaults.scalingFilter)
            {
                sections[videoSetting.json_section][videoSetting.json_key] = ScalingFilterToString(*videoSetting.value.scalingFilter);
            }
            break;
        case SettingType::AntiAliasing:
            if (*videoSetting.value.antiAliasing != videoSetting.defaults.antiAliasing)
            {
                sections[videoSetting.json_section][videoSetting.json_key] = AntiAliasingToString(*videoSetting.value.antiAliasing);
            }
            break;
        case SettingType::GpuAccuracy:
            if (*videoSetting.value.gpuAccuracy != videoSetting.defaults.gpuAccuracy)
            {
                sections[videoSetting.json_section][videoSetting.json_key] = GpuAccuracyToString(*videoSetting.value.gpuAccuracy);
            }
            break;
        case SettingType::AnisotropyMode:
            if (*videoSetting.value.anisotropyMode != videoSetting.defaults.anisotropyMode)
            {
                sections[videoSetting.json_section][videoSetting.json_key] = AnisotropyModeToString(*videoSetting.value.anisotropyMode);
            }
            break;
        case SettingType::AstcRecompression:
            if (*videoSetting.value.astcRecompression != videoSetting.defaults.astcRecompression)
            {
                sections[videoSetting.json_section][videoSetting.json_key] = AstcRecompressionToString(*videoSetting.value.astcRecompression);
            }
            break;
        case SettingType::VramUsageMode:
            if (*videoSetting.value.vramUsageMode != videoSetting.defaults.vramUsageMode)
            {
                sections[videoSetting.json_section][videoSetting.json_key] = VramUsageModeToString(*videoSetting.value.vramUsageMode);
            }
            break;
        default:
            UNIMPLEMENTED();
        }
    }

    JsonValue json;
    for (SectionMap::const_iterator it = sections.begin(); it != sections.end(); ++it)
    {
        if (it->second.size() > 0)
        {
            json[it->first] = it->second;
        }
    }
    g_settings->SetSectionSettings("nxemu-video", json.isNull() ? "" : JsonStyledWriter().write(json));
}

namespace
{
VideoSetting::VideoSetting(const char * id, const char * section, const char * key, bool * val, bool defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::Boolean)
{
    defaults.boolValue = defaultValue;
    value.boolValue = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, int32_t * val, int32_t defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::IntValue)
{
    defaults.intValue = defaultValue;
    value.intValue = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, int32_t * val, int32_t defaultValue, int32_t minValue, int32_t maxValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::IntValueRanged),
    minValue(minValue),
    maxValue(maxValue)
{
    defaults.intValue = defaultValue;
    value.intValue = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, RendererBackend * val, RendererBackend defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::RendererBackend)
{
    defaults.rendererBackend = defaultValue;
    value.rendererBackend = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, ShaderBackend * val, ShaderBackend defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::ShaderBackend)
{
    defaults.shaderBackend = defaultValue;
    value.shaderBackend = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, AstcDecodeMode * val, AstcDecodeMode defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::AstcDecodeMode)
{
    defaults.astcDecodeMode = defaultValue;
    value.astcDecodeMode = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, VSyncMode * val, VSyncMode defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::VSyncMode)
{
    defaults.vSyncMode = defaultValue;
    value.vSyncMode = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, NvdecEmulation * val, NvdecEmulation defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::NvdecEmulation)
{
    defaults.nvdecEmulation = defaultValue;
    value.nvdecEmulation = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, FullscreenMode * val, FullscreenMode defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::FullscreenMode)
{
    defaults.fullscreenMode = defaultValue;
    value.fullscreenMode = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, AspectRatio * val, AspectRatio defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::AspectRatio)
{
    defaults.aspectRatio = defaultValue;
    value.aspectRatio = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, ResolutionSetup * val, ResolutionSetup defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::ResolutionSetup)
{
    defaults.resolutionSetup = defaultValue;
    value.resolutionSetup = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, ScalingFilter * val, ScalingFilter defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::ScalingFilter)
{
    defaults.scalingFilter = defaultValue;
    value.scalingFilter = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, AntiAliasing * val, AntiAliasing defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::AntiAliasing)
{
    defaults.antiAliasing = defaultValue;
    value.antiAliasing = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, GpuAccuracy * val, GpuAccuracy defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::GpuAccuracy)
{
    defaults.gpuAccuracy = defaultValue;
    value.gpuAccuracy = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, AnisotropyMode * val, AnisotropyMode defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::AnisotropyMode)
{
    defaults.anisotropyMode = defaultValue;
    value.anisotropyMode = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, AstcRecompression * val, AstcRecompression defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::AstcRecompression)
{
    defaults.astcRecompression = defaultValue;
    value.astcRecompression = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, VramUsageMode * val, VramUsageMode defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::VramUsageMode)
{
    defaults.vramUsageMode = defaultValue;
    value.vramUsageMode = val;
}
} // namespace