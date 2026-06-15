#include "video_settings.h"
#include "video_enum_strings.h"
#include "video_settings_identifiers.h"
#include <common/json.h>
#include <nxemu-module-spec/base.h>
#include <yuzu_common/settings_enums.h>
#include <yuzu_common/settings_setting.h>
#include <yuzu_common/yuzu_assert.h>

extern IModuleSettings * g_settings;

VideoSettings videoSettings;

namespace Settings
{

#define VIDEO_SETTING_ENUM_METADATA(EnumType, translation_type, ...)                             \
    template <>                                                                                  \
    std::vector<std::pair<std::string, EnumType>> EnumMetadata<EnumType>::Canonicalizations() {  \
        return {__VA_ARGS__};                                                                    \
    }                                                                                            \
    template <>                                                                                  \
    u32 EnumMetadata<EnumType>::Index() {                                                        \
        return static_cast<u32>(translation_type);                                               \
    }

VIDEO_SETTING_ENUM_METADATA(
    AnisotropyMode, VideoSettingTranslationType::AnisotropyMode,
    {AnisotropyModeToString(AnisotropyMode::Automatic), AnisotropyMode::Automatic},
    {AnisotropyModeToString(AnisotropyMode::Default), AnisotropyMode::Default},
    {AnisotropyModeToString(AnisotropyMode::X2), AnisotropyMode::X2},
    {AnisotropyModeToString(AnisotropyMode::X4), AnisotropyMode::X4},
    {AnisotropyModeToString(AnisotropyMode::X8), AnisotropyMode::X8},
    {AnisotropyModeToString(AnisotropyMode::X16), AnisotropyMode::X16});

VIDEO_SETTING_ENUM_METADATA(
    AntiAliasing, VideoSettingTranslationType::AntiAliasing,
    {AntiAliasingToString(AntiAliasing::None), AntiAliasing::None},
    {AntiAliasingToString(AntiAliasing::Fxaa), AntiAliasing::Fxaa},
    {AntiAliasingToString(AntiAliasing::Smaa), AntiAliasing::Smaa},
    {AntiAliasingToString(AntiAliasing::MaxEnum), AntiAliasing::MaxEnum});

VIDEO_SETTING_ENUM_METADATA(
    AspectRatio, VideoSettingTranslationType::AspectRatio,
    {AspectRatioToString(AspectRatio::R16_9), AspectRatio::R16_9},
    {AspectRatioToString(AspectRatio::R4_3), AspectRatio::R4_3},
    {AspectRatioToString(AspectRatio::R21_9), AspectRatio::R21_9},
    {AspectRatioToString(AspectRatio::R16_10), AspectRatio::R16_10},
    {AspectRatioToString(AspectRatio::Stretch), AspectRatio::Stretch});

VIDEO_SETTING_ENUM_METADATA(
    AstcDecodeMode, VideoSettingTranslationType::AstcDecodeMode,
    {AstcDecodeModeToString(AstcDecodeMode::Cpu), AstcDecodeMode::Cpu},
    {AstcDecodeModeToString(AstcDecodeMode::Gpu), AstcDecodeMode::Gpu},
    {AstcDecodeModeToString(AstcDecodeMode::CpuAsynchronous), AstcDecodeMode::CpuAsynchronous});

VIDEO_SETTING_ENUM_METADATA(
    AstcRecompression, VideoSettingTranslationType::AstcRecompression,
    {AstcRecompressionToString(AstcRecompression::Uncompressed), AstcRecompression::Uncompressed},
    {AstcRecompressionToString(AstcRecompression::Bc1), AstcRecompression::Bc1},
    {AstcRecompressionToString(AstcRecompression::Bc3), AstcRecompression::Bc3});

VIDEO_SETTING_ENUM_METADATA(
    FullscreenMode, VideoSettingTranslationType::FullscreenMode,
    {FullscreenModeToString(FullscreenMode::Borderless), FullscreenMode::Borderless},
    {FullscreenModeToString(FullscreenMode::Exclusive), FullscreenMode::Exclusive});

VIDEO_SETTING_ENUM_METADATA(
    GpuAccuracy, VideoSettingTranslationType::GpuAccuracy,
    {GpuAccuracyToString(GpuAccuracy::Normal), GpuAccuracy::Normal},
    {GpuAccuracyToString(GpuAccuracy::High), GpuAccuracy::High},
    {GpuAccuracyToString(GpuAccuracy::Extreme), GpuAccuracy::Extreme});

VIDEO_SETTING_ENUM_METADATA(
    NvdecEmulation, VideoSettingTranslationType::NvdecEmulation,
    {NvdecEmulationToString(NvdecEmulation::Off), NvdecEmulation::Off},
    {NvdecEmulationToString(NvdecEmulation::Cpu), NvdecEmulation::Cpu},
    {NvdecEmulationToString(NvdecEmulation::Gpu), NvdecEmulation::Gpu});

VIDEO_SETTING_ENUM_METADATA(
    RendererBackend, VideoSettingTranslationType::RendererBackend,
    {RendererBackendToString(RendererBackend::OpenGL), RendererBackend::OpenGL},
    {RendererBackendToString(RendererBackend::Vulkan), RendererBackend::Vulkan},
    {RendererBackendToString(RendererBackend::Null), RendererBackend::Null});

VIDEO_SETTING_ENUM_METADATA(
    ResolutionSetup, VideoSettingTranslationType::ResolutionSetup,
    {ResolutionSetupToString(ResolutionSetup::Res1_2X), ResolutionSetup::Res1_2X},
    {ResolutionSetupToString(ResolutionSetup::Res3_4X), ResolutionSetup::Res3_4X},
    {ResolutionSetupToString(ResolutionSetup::Res1X), ResolutionSetup::Res1X},
    {ResolutionSetupToString(ResolutionSetup::Res3_2X), ResolutionSetup::Res3_2X},
    {ResolutionSetupToString(ResolutionSetup::Res2X), ResolutionSetup::Res2X},
    {ResolutionSetupToString(ResolutionSetup::Res3X), ResolutionSetup::Res3X},
    {ResolutionSetupToString(ResolutionSetup::Res4X), ResolutionSetup::Res4X},
    {ResolutionSetupToString(ResolutionSetup::Res5X), ResolutionSetup::Res5X},
    {ResolutionSetupToString(ResolutionSetup::Res6X), ResolutionSetup::Res6X},
    {ResolutionSetupToString(ResolutionSetup::Res7X), ResolutionSetup::Res7X},
    {ResolutionSetupToString(ResolutionSetup::Res8X), ResolutionSetup::Res8X});

VIDEO_SETTING_ENUM_METADATA(
    ScalingFilter, VideoSettingTranslationType::ScalingFilter,
    {ScalingFilterToString(ScalingFilter::NearestNeighbor), ScalingFilter::NearestNeighbor},
    {ScalingFilterToString(ScalingFilter::Bilinear), ScalingFilter::Bilinear},
    {ScalingFilterToString(ScalingFilter::Bicubic), ScalingFilter::Bicubic},
    {ScalingFilterToString(ScalingFilter::Gaussian), ScalingFilter::Gaussian},
    {ScalingFilterToString(ScalingFilter::ScaleForce), ScalingFilter::ScaleForce},
    {ScalingFilterToString(ScalingFilter::Fsr), ScalingFilter::Fsr},
    {ScalingFilterToString(ScalingFilter::MaxEnum), ScalingFilter::MaxEnum});

VIDEO_SETTING_ENUM_METADATA(
    ShaderBackend, VideoSettingTranslationType::ShaderBackend,
    {ShaderBackendToString(ShaderBackend::Glsl), ShaderBackend::Glsl},
    {ShaderBackendToString(ShaderBackend::Glasm), ShaderBackend::Glasm},
    {ShaderBackendToString(ShaderBackend::SpirV), ShaderBackend::SpirV});

VIDEO_SETTING_ENUM_METADATA(
    VSyncMode, VideoSettingTranslationType::VSyncMode,
    {VSyncModeToString(VSyncMode::Immediate), VSyncMode::Immediate},
    {VSyncModeToString(VSyncMode::Mailbox), VSyncMode::Mailbox},
    {VSyncModeToString(VSyncMode::Fifo), VSyncMode::Fifo},
    {VSyncModeToString(VSyncMode::FifoRelaxed), VSyncMode::FifoRelaxed});

VIDEO_SETTING_ENUM_METADATA(
    VramUsageMode, VideoSettingTranslationType::VramUsageMode,
    {VramUsageModeToString(VramUsageMode::Conservative), VramUsageMode::Conservative},
    {VramUsageModeToString(VramUsageMode::Aggressive), VramUsageMode::Aggressive});

#undef VIDEO_SETTING_ENUM_METADATA

#ifndef CANNOT_EXPLICITLY_INSTANTIATE
#define SETTING(TYPE, RANGED) template class Setting<TYPE, RANGED>
#define SWITCHABLE(TYPE, RANGED) template class SwitchableSetting<TYPE, RANGED>

SWITCHABLE(AnisotropyMode, true);
SWITCHABLE(AntiAliasing, false);
SWITCHABLE(AspectRatio, true);
SWITCHABLE(AstcDecodeMode, true);
SWITCHABLE(AstcRecompression, true);
SWITCHABLE(FullscreenMode, true);
SWITCHABLE(GpuAccuracy, true);
SWITCHABLE(NvdecEmulation, false);
SWITCHABLE(RendererBackend, true);
SWITCHABLE(ResolutionSetup, false);
SWITCHABLE(ScalingFilter, false);
SWITCHABLE(ShaderBackend, true);
SWITCHABLE(VramUsageMode, true);
SETTING(VSyncMode, true);
SWITCHABLE(bool, false);
SWITCHABLE(int, false);
SWITCHABLE(int, true);
SWITCHABLE(u8, false);

#undef SETTING
#undef SWITCHABLE
#endif

void UpdateGPUAccuracy()
{
    videoSettings.current_gpu_accuracy = videoSettings.gpu_accuracy.GetValue();
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
    const auto setup = videoSettings.resolution_setup.GetValue();
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
    VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<bool> * val);
    VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<int> * val);
    VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<int, true> * val);
    VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<RendererBackend, true> * val);
    VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<ShaderBackend, true> * val);
    VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<AstcDecodeMode, true> * val);
    VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<VSyncMode, true> * val);
    VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<NvdecEmulation> * val);
    VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<FullscreenMode, true> * val);
    VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<AspectRatio, true> * val);
    VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<ResolutionSetup> * val);
    VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<ScalingFilter> * val);
    VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<AntiAliasing> * val);
    VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<GpuAccuracy, true> * val);
    VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<AnisotropyMode, true> * val);
    VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<AstcRecompression, true> * val);
    VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<VramUsageMode, true> * val);

    const char * identifier;
    const char * json_section;
    const char * json_key;
    SettingType settingType;
    union
    {
        Settings::SwitchableSetting<bool> * boolValue;
        Settings::SwitchableSetting<int> * intValue;
        Settings::SwitchableSetting<int, true> * intValueRanged;
        Settings::SwitchableSetting<RendererBackend, true> * rendererBackend;
        Settings::SwitchableSetting<ShaderBackend, true> * shaderBackend;
        Settings::SwitchableSetting<AstcDecodeMode, true> * astcDecodeMode;
        Settings::SwitchableSetting<VSyncMode, true> * vSyncMode;
        Settings::SwitchableSetting<NvdecEmulation> * nvdecEmulation;
        Settings::SwitchableSetting<FullscreenMode, true> * fullscreenMode;
        Settings::SwitchableSetting<AspectRatio, true> * aspectRatio;
        Settings::SwitchableSetting<ResolutionSetup> * resolutionSetup;
        Settings::SwitchableSetting<ScalingFilter> * scalingFilter;
        Settings::SwitchableSetting<AntiAliasing> * antiAliasing;
        Settings::SwitchableSetting<GpuAccuracy, true> * gpuAccuracy;
        Settings::SwitchableSetting<AnisotropyMode, true> * anisotropyMode;
        Settings::SwitchableSetting<AstcRecompression, true> * astcRecompression;
        Settings::SwitchableSetting<VramUsageMode, true> * vramUsageMode;
    } setting;
};

static VideoSetting settings[] = {
    {NXVideoSetting::GraphicsAPI, "video", "renderer_backend", &videoSettings.renderer_backend},
    {NXVideoSetting::ShaderBackend, "video", "shader_backend", &videoSettings.shader_backend},
    {NXVideoSetting::VulkanDevice, "video", "vulkan_device", &videoSettings.vulkan_device},
    {NXVideoSetting::UseDiskPipelineCache, "video", "use_disk_shader_cache", &videoSettings.use_disk_shader_cache},
    {NXVideoSetting::UseAsynchronousGPUEmulation, "video", "use_asynchronous_gpu_emulation", &videoSettings.use_asynchronous_gpu_emulation},
    {NXVideoSetting::AstcDecodeMode, "video", "astc_decode_mode", &videoSettings.accelerate_astc},
    {NXVideoSetting::VSyncMode, "video", "vsync_mode", &videoSettings.vsync_mode},
    {NXVideoSetting::NvdecEmulation, "video", "nvdec_emulation", &videoSettings.nvdec_emulation},
    {NXVideoSetting::FullscreenMode, "video", "fullscreen_mode", &videoSettings.fullscreen_mode},
    {NXVideoSetting::AspectRatio, "video", "aspect_ratio", &videoSettings.aspect_ratio},
    {NXVideoSetting::ResolutionSetup, "video", "resolution_setup", &videoSettings.resolution_setup},
    {NXVideoSetting::ScalingFilter, "video", "scaling_filter", &videoSettings.scaling_filter},
    {NXVideoSetting::AntiAliasing, "video", "anti_aliasing", &videoSettings.anti_aliasing},
    {NXVideoSetting::FSPSharpness, "video", "fsr_sharpening_slider", &videoSettings.fsr_sharpening_slider},
    {NXVideoSetting::EnableAsynchronousPresentation, "video", "async_presentation", &videoSettings.async_presentation},
    {NXVideoSetting::ForceMaximumClocks, "video", "renderer_force_max_clock", &videoSettings.renderer_force_max_clock},
    {NXVideoSetting::EnableReactiveFlushing, "video", "use_reactive_flushing", &videoSettings.use_reactive_flushing},
    {NXVideoSetting::UseAsynchronousShaderBuilding, "video", "use_asynchronous_shaders", &videoSettings.use_asynchronous_shaders},
    {NXVideoSetting::FastGPUTime, "video", "use_fast_gpu_time", &videoSettings.use_fast_gpu_time},
    {NXVideoSetting::UseVulkanPipelineCache, "video", "use_vulkan_driver_pipeline_cache", &videoSettings.use_vulkan_driver_pipeline_cache},
    {NXVideoSetting::SyncToFramerateOfVideoPlayback, "video", "use_video_framerate", &videoSettings.use_video_framerate},
    {NXVideoSetting::BarrierFeedbackLoops, "video", "barrier_feedback_loops", &videoSettings.barrier_feedback_loops},
    {NXVideoSetting::AccuracyLevel, "video", "accuracy_level", &videoSettings.gpu_accuracy},
    {NXVideoSetting::AnisotropicFiltering, "video", "anisotropic_filtering", &videoSettings.max_anisotropy},
    {NXVideoSetting::ASTCRecompressionMethod, "video", "astc_recompression_method", &videoSettings.astc_recompression},
    {NXVideoSetting::VRAMUsageMode, "video", "vram_usage_mode", &videoSettings.vram_usage_mode},
};
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
            videoSetting.setting.boolValue->SetValue(g_settings->GetBool(setting));
            break;
        case SettingType::IntValue:
            videoSetting.setting.intValue->SetValue(g_settings->GetInt(setting));
            break;
        case SettingType::IntValueRanged:
            videoSetting.setting.intValueRanged->SetValue(g_settings->GetInt(setting));
            break;
        case SettingType::RendererBackend:
            videoSetting.setting.rendererBackend->SetValue((RendererBackend)g_settings->GetInt(setting));
            break;
        case SettingType::ShaderBackend:
            videoSetting.setting.shaderBackend->SetValue((ShaderBackend)g_settings->GetInt(setting));
            break;
        case SettingType::AstcDecodeMode:
            videoSetting.setting.astcDecodeMode->SetValue((AstcDecodeMode)g_settings->GetInt(setting));
            break;
        case SettingType::VSyncMode:
            videoSetting.setting.vSyncMode->SetValue((VSyncMode)g_settings->GetInt(setting));
            break;
        case SettingType::NvdecEmulation:
            videoSetting.setting.nvdecEmulation->SetValue((NvdecEmulation)g_settings->GetInt(setting));
            break;
        case SettingType::FullscreenMode:
            videoSetting.setting.fullscreenMode->SetValue((FullscreenMode)g_settings->GetInt(setting));
            break;
        case SettingType::AspectRatio:
            videoSetting.setting.aspectRatio->SetValue((AspectRatio)g_settings->GetInt(setting));
            break;
        case SettingType::ResolutionSetup:
            videoSetting.setting.resolutionSetup->SetValue((ResolutionSetup)g_settings->GetInt(setting));
            break;
        case SettingType::ScalingFilter:
            videoSetting.setting.scalingFilter->SetValue((ScalingFilter)g_settings->GetInt(setting));
            break;
        case SettingType::AntiAliasing:
            videoSetting.setting.antiAliasing->SetValue((AntiAliasing)g_settings->GetInt(setting));
            break;
        case SettingType::GpuAccuracy:
            videoSetting.setting.gpuAccuracy->SetValue((GpuAccuracy)g_settings->GetInt(setting));
            break;
        case SettingType::AnisotropyMode:
            videoSetting.setting.anisotropyMode->SetValue((AnisotropyMode)g_settings->GetInt(setting));
            break;
        case SettingType::AstcRecompression:
            videoSetting.setting.astcRecompression->SetValue((AstcRecompression)g_settings->GetInt(setting));
            break;
        case SettingType::VramUsageMode:
            videoSetting.setting.vramUsageMode->SetValue((VramUsageMode)g_settings->GetInt(setting));
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
            videoSetting.setting.boolValue->SetValue(videoSetting.setting.boolValue->GetDefault());
            break;
        case SettingType::IntValue:
            videoSetting.setting.intValue->SetValue(videoSetting.setting.intValue->GetDefault());
            break;
        case SettingType::IntValueRanged:
            videoSetting.setting.intValueRanged->SetValue(videoSetting.setting.intValueRanged->GetDefault());
            break;
        case SettingType::RendererBackend:
            videoSetting.setting.rendererBackend->SetValue(videoSetting.setting.rendererBackend->GetDefault());
            break;
        case SettingType::ShaderBackend:
            videoSetting.setting.shaderBackend->SetValue(videoSetting.setting.shaderBackend->GetDefault());
            break;
        case SettingType::AstcDecodeMode:
            videoSetting.setting.astcDecodeMode->SetValue(videoSetting.setting.astcDecodeMode->GetDefault());
            break;
        case SettingType::VSyncMode:
            videoSetting.setting.vSyncMode->SetValue(videoSetting.setting.vSyncMode->GetDefault());
            break;
        case SettingType::NvdecEmulation:
            videoSetting.setting.nvdecEmulation->SetValue(videoSetting.setting.nvdecEmulation->GetDefault());
            break;
        case SettingType::FullscreenMode:
            videoSetting.setting.fullscreenMode->SetValue(videoSetting.setting.fullscreenMode->GetDefault());
            break;
        case SettingType::AspectRatio:
            videoSetting.setting.aspectRatio->SetValue(videoSetting.setting.aspectRatio->GetDefault());
            break;
        case SettingType::ResolutionSetup:
            videoSetting.setting.resolutionSetup->SetValue(videoSetting.setting.resolutionSetup->GetDefault());
            break;
        case SettingType::ScalingFilter:
            videoSetting.setting.scalingFilter->SetValue(videoSetting.setting.scalingFilter->GetDefault());
            break;
        case SettingType::AntiAliasing:
            videoSetting.setting.antiAliasing->SetValue(videoSetting.setting.antiAliasing->GetDefault());
            break;
        case SettingType::GpuAccuracy:
            videoSetting.setting.gpuAccuracy->SetValue(videoSetting.setting.gpuAccuracy->GetDefault());
            break;
        case SettingType::AnisotropyMode:
            videoSetting.setting.anisotropyMode->SetValue(videoSetting.setting.anisotropyMode->GetDefault());
            break;
        case SettingType::AstcRecompression:
            videoSetting.setting.astcRecompression->SetValue(videoSetting.setting.astcRecompression->GetDefault());
            break;
        case SettingType::VramUsageMode:
            videoSetting.setting.vramUsageMode->SetValue(videoSetting.setting.vramUsageMode->GetDefault());
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
                    videoSetting.setting.boolValue->SetValue(value.asBool());
                }
                break;
            case SettingType::IntValue:
                if (value.isInt())
                {
                    videoSetting.setting.intValue->SetValue((int32_t)value.asInt64());
                }
                break;
            case SettingType::IntValueRanged:
                if (value.isInt())
                {
                    videoSetting.setting.intValueRanged->SetValue((int32_t)value.asInt64());
                }
                break;
            case SettingType::RendererBackend:
                if (value.isString())
                {
                    videoSetting.setting.rendererBackend->SetValue(RendererBackendFromString(value.asString()));
                }
                break;
            case SettingType::ShaderBackend:
                if (value.isString())
                {
                    videoSetting.setting.shaderBackend->SetValue(ShaderBackendFromString(value.asString()));
                }
                break;
            case SettingType::AstcDecodeMode:
                if (value.isString())
                {
                    videoSetting.setting.astcDecodeMode->SetValue(AstcDecodeModeFromString(value.asString()));
                }
                break;
            case SettingType::VSyncMode:
                if (value.isString())
                {
                    videoSetting.setting.vSyncMode->SetValue(VSyncModeFromString(value.asString()));
                }
                break;
            case SettingType::NvdecEmulation:
                if (value.isString())
                {
                    videoSetting.setting.nvdecEmulation->SetValue(NvdecEmulationFromString(value.asString()));
                }
                break;
            case SettingType::FullscreenMode:
                if (value.isString())
                {
                    videoSetting.setting.fullscreenMode->SetValue(FullscreenModeFromString(value.asString()));
                }
                break;
            case SettingType::AspectRatio:
                if (value.isString())
                {
                    videoSetting.setting.aspectRatio->SetValue(AspectRatioFromString(value.asString()));
                }
                break;
            case SettingType::ResolutionSetup:
                if (value.isString())
                {
                    videoSetting.setting.resolutionSetup->SetValue(ResolutionSetupFromString(value.asString()));
                }
                break;
            case SettingType::ScalingFilter:
                if (value.isString())
                {
                    videoSetting.setting.scalingFilter->SetValue(ScalingFilterFromString(value.asString()));
                }
                break;
            case SettingType::AntiAliasing:
                if (value.isString())
                {
                    videoSetting.setting.antiAliasing->SetValue(AntiAliasingFromString(value.asString()));
                }
                break;
            case SettingType::GpuAccuracy:
                if (value.isString())
                {
                    videoSetting.setting.gpuAccuracy->SetValue(GpuAccuracyFromString(value.asString()));
                }
                break;
            case SettingType::AnisotropyMode:
                if (value.isString())
                {
                    videoSetting.setting.anisotropyMode->SetValue(AnisotropyModeFromString(value.asString()));
                }
                break;
            case SettingType::AstcRecompression:
                if (value.isString())
                {
                    videoSetting.setting.astcRecompression->SetValue(AstcRecompressionFromString(value.asString()));
                }
                break;
            case SettingType::VramUsageMode:
                if (value.isString())
                {
                    videoSetting.setting.vramUsageMode->SetValue(VramUsageModeFromString(value.asString()));
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
            g_settings->SetDefaultBool(videoSetting.identifier, videoSetting.setting.boolValue->GetDefault());
            g_settings->SetBool(videoSetting.identifier, videoSetting.setting.boolValue->GetValue());
            break;
        case SettingType::IntValue:
            g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.setting.intValue->GetDefault());
            g_settings->SetInt(videoSetting.identifier, (int32_t)videoSetting.setting.intValue->GetValue());
            break;
        case SettingType::IntValueRanged:
            g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.setting.intValueRanged->GetDefault());
            g_settings->SetInt(videoSetting.identifier, (int32_t)videoSetting.setting.intValueRanged->GetValue());
            break;
        case SettingType::RendererBackend:
            g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.setting.rendererBackend->GetDefault());
            g_settings->SetInt(videoSetting.identifier, (int32_t)videoSetting.setting.rendererBackend->GetValue());
            break;
        case SettingType::ShaderBackend:
            g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.setting.shaderBackend->GetDefault());
            g_settings->SetInt(videoSetting.identifier, (int32_t)videoSetting.setting.shaderBackend->GetValue());
            break;
        case SettingType::AstcDecodeMode:
            g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.setting.astcDecodeMode->GetDefault());
            g_settings->SetInt(videoSetting.identifier, (int32_t)videoSetting.setting.astcDecodeMode->GetValue());
            break;
        case SettingType::VSyncMode:
            g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.setting.vSyncMode->GetDefault());
            g_settings->SetInt(videoSetting.identifier, (int32_t)videoSetting.setting.vSyncMode->GetValue());
            break;
        case SettingType::NvdecEmulation:
            g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.setting.nvdecEmulation->GetDefault());
            g_settings->SetInt(videoSetting.identifier, (int32_t)videoSetting.setting.nvdecEmulation->GetValue());
            break;
        case SettingType::FullscreenMode:
            g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.setting.fullscreenMode->GetDefault());
            g_settings->SetInt(videoSetting.identifier, (int32_t)videoSetting.setting.fullscreenMode->GetValue());
            break;
        case SettingType::AspectRatio:
            g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.setting.aspectRatio->GetDefault());
            g_settings->SetInt(videoSetting.identifier, (int32_t)videoSetting.setting.aspectRatio->GetValue());
            break;
        case SettingType::ResolutionSetup:
            g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.setting.resolutionSetup->GetDefault());
            g_settings->SetInt(videoSetting.identifier, (int32_t)videoSetting.setting.resolutionSetup->GetValue());
            break;
        case SettingType::ScalingFilter:
            g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.setting.scalingFilter->GetDefault());
            g_settings->SetInt(videoSetting.identifier, (int32_t)videoSetting.setting.scalingFilter->GetValue());
            break;
        case SettingType::AntiAliasing:
            g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.setting.antiAliasing->GetDefault());
            g_settings->SetInt(videoSetting.identifier, (int32_t)videoSetting.setting.antiAliasing->GetValue());
            break;
        case SettingType::GpuAccuracy:
            g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.setting.gpuAccuracy->GetDefault());
            g_settings->SetInt(videoSetting.identifier, (int32_t)videoSetting.setting.gpuAccuracy->GetValue());
            break;
        case SettingType::AnisotropyMode:
            g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.setting.anisotropyMode->GetDefault());
            g_settings->SetInt(videoSetting.identifier, (int32_t)videoSetting.setting.anisotropyMode->GetValue());
            break;
        case SettingType::AstcRecompression:
            g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.setting.astcRecompression->GetDefault());
            g_settings->SetInt(videoSetting.identifier, (int32_t)videoSetting.setting.astcRecompression->GetValue());
            break;
        case SettingType::VramUsageMode:
            g_settings->SetDefaultInt(videoSetting.identifier, (int32_t)videoSetting.setting.vramUsageMode->GetDefault());
            g_settings->SetInt(videoSetting.identifier, (int32_t)videoSetting.setting.vramUsageMode->GetValue());
            break;
        default:
            UNIMPLEMENTED();
        }
        g_settings->RegisterCallback(videoSetting.identifier, VideoSettingChanged, nullptr);
    }
    Settings::UpdateRescalingInfo();
    Settings::UpdateGPUAccuracy();
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
            if (videoSetting.setting.boolValue->GetValue() != videoSetting.setting.boolValue->GetDefault())
            {
                sections[videoSetting.json_section][videoSetting.json_key] = videoSetting.setting.boolValue->GetValue();
            }
            break;
        case SettingType::IntValue:
            if (videoSetting.setting.intValue->GetValue() != videoSetting.setting.intValue->GetDefault())
            {
                sections[videoSetting.json_section][videoSetting.json_key] = videoSetting.setting.intValue->GetValue();
            }
            break;
        case SettingType::IntValueRanged:
            if (videoSetting.setting.intValueRanged->GetValue() != videoSetting.setting.intValueRanged->GetDefault())
            {
                sections[videoSetting.json_section][videoSetting.json_key] = videoSetting.setting.intValueRanged->GetValue();
            }
            break;
        case SettingType::RendererBackend:
            if (videoSetting.setting.rendererBackend->GetValue() != videoSetting.setting.rendererBackend->GetDefault())
            {
                sections[videoSetting.json_section][videoSetting.json_key] = RendererBackendToString(videoSetting.setting.rendererBackend->GetValue());
            }
            break;
        case SettingType::ShaderBackend:
            if (videoSetting.setting.shaderBackend->GetValue() != videoSetting.setting.shaderBackend->GetDefault())
            {
                sections[videoSetting.json_section][videoSetting.json_key] = ShaderBackendToString(videoSetting.setting.shaderBackend->GetValue());
            }
            break;
        case SettingType::AstcDecodeMode:
            if (videoSetting.setting.astcDecodeMode->GetValue() != videoSetting.setting.astcDecodeMode->GetDefault())
            {
                sections[videoSetting.json_section][videoSetting.json_key] = AstcDecodeModeToString(videoSetting.setting.astcDecodeMode->GetValue());
            }
            break;
        case SettingType::VSyncMode:
            if (videoSetting.setting.vSyncMode->GetValue() != videoSetting.setting.vSyncMode->GetDefault())
            {
                sections[videoSetting.json_section][videoSetting.json_key] = VSyncModeToString(videoSetting.setting.vSyncMode->GetValue());
            }
            break;
        case SettingType::NvdecEmulation:
            if (videoSetting.setting.nvdecEmulation->GetValue() != videoSetting.setting.nvdecEmulation->GetDefault())
            {
                sections[videoSetting.json_section][videoSetting.json_key] = NvdecEmulationToString(videoSetting.setting.nvdecEmulation->GetValue());
            }
            break;
        case SettingType::FullscreenMode:
            if (videoSetting.setting.fullscreenMode->GetValue() != videoSetting.setting.fullscreenMode->GetDefault())
            {
                sections[videoSetting.json_section][videoSetting.json_key] = FullscreenModeToString(videoSetting.setting.fullscreenMode->GetValue());
            }
            break;
        case SettingType::AspectRatio:
            if (videoSetting.setting.aspectRatio->GetValue() != videoSetting.setting.aspectRatio->GetDefault())
            {
                sections[videoSetting.json_section][videoSetting.json_key] = AspectRatioToString(videoSetting.setting.aspectRatio->GetValue());
            }
            break;
        case SettingType::ResolutionSetup:
            if (videoSetting.setting.resolutionSetup->GetValue() != videoSetting.setting.resolutionSetup->GetDefault())
            {
                sections[videoSetting.json_section][videoSetting.json_key] = ResolutionSetupToString(videoSetting.setting.resolutionSetup->GetValue());
            }
            break;
        case SettingType::ScalingFilter:
            if (videoSetting.setting.scalingFilter->GetValue() != videoSetting.setting.scalingFilter->GetDefault())
            {
                sections[videoSetting.json_section][videoSetting.json_key] = ScalingFilterToString(videoSetting.setting.scalingFilter->GetValue());
            }
            break;
        case SettingType::AntiAliasing:
            if (videoSetting.setting.antiAliasing->GetValue() != videoSetting.setting.antiAliasing->GetDefault())
            {
                sections[videoSetting.json_section][videoSetting.json_key] = AntiAliasingToString(videoSetting.setting.antiAliasing->GetValue());
            }
            break;
        case SettingType::GpuAccuracy:
            if (videoSetting.setting.gpuAccuracy->GetValue() != videoSetting.setting.gpuAccuracy->GetDefault())
            {
                sections[videoSetting.json_section][videoSetting.json_key] = GpuAccuracyToString(videoSetting.setting.gpuAccuracy->GetValue());
            }
            break;
        case SettingType::AnisotropyMode:
            if (videoSetting.setting.anisotropyMode->GetValue() != videoSetting.setting.anisotropyMode->GetDefault())
            {
                sections[videoSetting.json_section][videoSetting.json_key] = AnisotropyModeToString(videoSetting.setting.anisotropyMode->GetValue());
            }
            break;
        case SettingType::AstcRecompression:
            if (videoSetting.setting.astcRecompression->GetValue() != videoSetting.setting.astcRecompression->GetDefault())
            {
                sections[videoSetting.json_section][videoSetting.json_key] = AstcRecompressionToString(videoSetting.setting.astcRecompression->GetValue());
            }
            break;
        case SettingType::VramUsageMode:
            if (videoSetting.setting.vramUsageMode->GetValue() != videoSetting.setting.vramUsageMode->GetDefault())
            {
                sections[videoSetting.json_section][videoSetting.json_key] = VramUsageModeToString(videoSetting.setting.vramUsageMode->GetValue());
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
VideoSetting::VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<bool> * val) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::Boolean)
{
    setting.boolValue = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<int> * val) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::IntValue)
{
    setting.intValue = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<int, true> * val) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::IntValueRanged)
{
    setting.intValueRanged = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<RendererBackend, true> * val) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::RendererBackend)
{
    setting.rendererBackend = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<ShaderBackend, true> * val) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::ShaderBackend)
{
    setting.shaderBackend = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<AstcDecodeMode, true> * val) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::AstcDecodeMode)
{
    setting.astcDecodeMode = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<VSyncMode, true> * val) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::VSyncMode)
{
    setting.vSyncMode = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<NvdecEmulation> * val) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::NvdecEmulation)
{
    setting.nvdecEmulation = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<FullscreenMode, true> * val) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::FullscreenMode)
{
    setting.fullscreenMode = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<AspectRatio, true> * val) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::AspectRatio)
{
    setting.aspectRatio = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<ResolutionSetup> * val) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::ResolutionSetup)
{
    setting.resolutionSetup = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<ScalingFilter> * val) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::ScalingFilter)
{
    setting.scalingFilter = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<AntiAliasing> * val) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::AntiAliasing)
{
    setting.antiAliasing = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<GpuAccuracy, true> * val) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::GpuAccuracy)
{
    setting.gpuAccuracy = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<AnisotropyMode, true> * val) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::AnisotropyMode)
{
    setting.anisotropyMode = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<AstcRecompression, true> * val) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::AstcRecompression)
{
    setting.astcRecompression = val;
}

VideoSetting::VideoSetting(const char * id, const char * section, const char * key, Settings::SwitchableSetting<VramUsageMode, true> * val) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::VramUsageMode)
{
    setting.vramUsageMode = val;
}
} // namespace