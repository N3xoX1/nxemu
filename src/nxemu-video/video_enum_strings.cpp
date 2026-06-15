#include "video_enum_strings.h"

const char* AnisotropyModeToString(AnisotropyMode value)
{
    switch (value) {
    case AnisotropyMode::Automatic: return "Automatic";
    case AnisotropyMode::Default: return "Default";
    case AnisotropyMode::X2: return "X2";
    case AnisotropyMode::X4: return "X4";
    case AnisotropyMode::X8: return "X8";
    case AnisotropyMode::X16: return "X16";
    }
    return "Automatic";
}

AnisotropyMode AnisotropyModeFromString(std::string_view str)
{
    if (str == "Automatic") return AnisotropyMode::Automatic;
    if (str == "Default") return AnisotropyMode::Default;
    if (str == "X2") return AnisotropyMode::X2;
    if (str == "X4") return AnisotropyMode::X4;
    if (str == "X8") return AnisotropyMode::X8;
    if (str == "X16") return AnisotropyMode::X16;
    return AnisotropyMode::Automatic;
}

const char* AstcDecodeModeToString(AstcDecodeMode value)
{
    switch (value) {
    case AstcDecodeMode::Cpu: return "Cpu";
    case AstcDecodeMode::Gpu: return "Gpu";
    case AstcDecodeMode::CpuAsynchronous: return "CpuAsynchronous";
    }
    return "Cpu";
}

AstcDecodeMode AstcDecodeModeFromString(std::string_view str)
{
    if (str == "Cpu") return AstcDecodeMode::Cpu;
    if (str == "Gpu") return AstcDecodeMode::Gpu;
    if (str == "CpuAsynchronous") return AstcDecodeMode::CpuAsynchronous;
    return AstcDecodeMode::Cpu;
}

const char* AstcRecompressionToString(AstcRecompression value)
{
    switch (value) {
    case AstcRecompression::Uncompressed: return "Uncompressed";
    case AstcRecompression::Bc1: return "Bc1";
    case AstcRecompression::Bc3: return "Bc3";
    }
    return "Uncompressed";
}

AstcRecompression AstcRecompressionFromString(std::string_view str)
{
    if (str == "Uncompressed") return AstcRecompression::Uncompressed;
    if (str == "Bc1") return AstcRecompression::Bc1;
    if (str == "Bc3") return AstcRecompression::Bc3;
    return AstcRecompression::Uncompressed;
}

const char* VSyncModeToString(VSyncMode value)
{
    switch (value) {
    case VSyncMode::Immediate: return "Immediate";
    case VSyncMode::Mailbox: return "Mailbox";
    case VSyncMode::Fifo: return "Fifo";
    case VSyncMode::FifoRelaxed: return "FifoRelaxed";
    }
    return "Immediate";
}

VSyncMode VSyncModeFromString(std::string_view str)
{
    if (str == "Immediate") return VSyncMode::Immediate;
    if (str == "Mailbox") return VSyncMode::Mailbox;
    if (str == "Fifo") return VSyncMode::Fifo;
    if (str == "FifoRelaxed") return VSyncMode::FifoRelaxed;
    return VSyncMode::Immediate;
}

const char* VramUsageModeToString(VramUsageMode value)
{
    switch (value) {
    case VramUsageMode::Conservative: return "Conservative";
    case VramUsageMode::Aggressive: return "Aggressive";
    }
    return "Conservative";
}

VramUsageMode VramUsageModeFromString(std::string_view str)
{
    if (str == "Conservative") return VramUsageMode::Conservative;
    if (str == "Aggressive") return VramUsageMode::Aggressive;
    return VramUsageMode::Conservative;
}

const char* RendererBackendToString(RendererBackend value)
{
    switch (value) {
    case RendererBackend::OpenGL: return "OpenGL";
    case RendererBackend::Vulkan: return "Vulkan";
    case RendererBackend::Null: return "Null";
    }
    return "OpenGL";
}

RendererBackend RendererBackendFromString(std::string_view str)
{
    if (str == "OpenGL") return RendererBackend::OpenGL;
    if (str == "Vulkan") return RendererBackend::Vulkan;
    if (str == "Null") return RendererBackend::Null;
    return RendererBackend::OpenGL;
}

const char* ShaderBackendToString(ShaderBackend value)
{
    switch (value) {
    case ShaderBackend::Glsl: return "Glsl";
    case ShaderBackend::Glasm: return "Glasm";
    case ShaderBackend::SpirV: return "SpirV";
    }
    return "Glsl";
}

ShaderBackend ShaderBackendFromString(std::string_view str)
{
    if (str == "Glsl") return ShaderBackend::Glsl;
    if (str == "Glasm") return ShaderBackend::Glasm;
    if (str == "SpirV") return ShaderBackend::SpirV;
    return ShaderBackend::Glsl;
}

const char* GpuAccuracyToString(GpuAccuracy value)
{
    switch (value) {
    case GpuAccuracy::Normal: return "Normal";
    case GpuAccuracy::High: return "High";
    case GpuAccuracy::Extreme: return "Extreme";
    }
    return "Normal";
}

GpuAccuracy GpuAccuracyFromString(std::string_view str)
{
    if (str == "Normal") return GpuAccuracy::Normal;
    if (str == "High") return GpuAccuracy::High;
    if (str == "Extreme") return GpuAccuracy::Extreme;
    return GpuAccuracy::Normal;
}

const char* FullscreenModeToString(FullscreenMode value)
{
    switch (value) {
    case FullscreenMode::Borderless: return "Borderless";
    case FullscreenMode::Exclusive: return "Exclusive";
    }
    return "Borderless";
}

FullscreenMode FullscreenModeFromString(std::string_view str)
{
    if (str == "Borderless") return FullscreenMode::Borderless;
    if (str == "Exclusive") return FullscreenMode::Exclusive;
    return FullscreenMode::Borderless;
}

const char* NvdecEmulationToString(NvdecEmulation value)
{
    switch (value) {
    case NvdecEmulation::Off: return "Off";
    case NvdecEmulation::Cpu: return "Cpu";
    case NvdecEmulation::Gpu: return "Gpu";
    }
    return "Off";
}

NvdecEmulation NvdecEmulationFromString(std::string_view str)
{
    if (str == "Off") return NvdecEmulation::Off;
    if (str == "Cpu") return NvdecEmulation::Cpu;
    if (str == "Gpu") return NvdecEmulation::Gpu;
    return NvdecEmulation::Off;
}

const char* ResolutionSetupToString(ResolutionSetup value)
{
    switch (value) {
    case ResolutionSetup::Res1_2X: return "Res1_2X";
    case ResolutionSetup::Res3_4X: return "Res3_4X";
    case ResolutionSetup::Res1X: return "Res1X";
    case ResolutionSetup::Res3_2X: return "Res3_2X";
    case ResolutionSetup::Res2X: return "Res2X";
    case ResolutionSetup::Res3X: return "Res3X";
    case ResolutionSetup::Res4X: return "Res4X";
    case ResolutionSetup::Res5X: return "Res5X";
    case ResolutionSetup::Res6X: return "Res6X";
    case ResolutionSetup::Res7X: return "Res7X";
    case ResolutionSetup::Res8X: return "Res8X";
    }
    return "Res1_2X";
}

ResolutionSetup ResolutionSetupFromString(std::string_view str)
{
    if (str == "Res1_2X") return ResolutionSetup::Res1_2X;
    if (str == "Res3_4X") return ResolutionSetup::Res3_4X;
    if (str == "Res1X") return ResolutionSetup::Res1X;
    if (str == "Res3_2X") return ResolutionSetup::Res3_2X;
    if (str == "Res2X") return ResolutionSetup::Res2X;
    if (str == "Res3X") return ResolutionSetup::Res3X;
    if (str == "Res4X") return ResolutionSetup::Res4X;
    if (str == "Res5X") return ResolutionSetup::Res5X;
    if (str == "Res6X") return ResolutionSetup::Res6X;
    if (str == "Res7X") return ResolutionSetup::Res7X;
    if (str == "Res8X") return ResolutionSetup::Res8X;
    return ResolutionSetup::Res1_2X;
}

const char* ScalingFilterToString(ScalingFilter value)
{
    switch (value) {
    case ScalingFilter::NearestNeighbor: return "NearestNeighbor";
    case ScalingFilter::Bilinear: return "Bilinear";
    case ScalingFilter::Bicubic: return "Bicubic";
    case ScalingFilter::Gaussian: return "Gaussian";
    case ScalingFilter::ScaleForce: return "ScaleForce";
    case ScalingFilter::Fsr: return "Fsr";
    case ScalingFilter::MaxEnum: return "MaxEnum";
    }
    return "NearestNeighbor";
}

ScalingFilter ScalingFilterFromString(std::string_view str)
{
    if (str == "NearestNeighbor") return ScalingFilter::NearestNeighbor;
    if (str == "Bilinear") return ScalingFilter::Bilinear;
    if (str == "Bicubic") return ScalingFilter::Bicubic;
    if (str == "Gaussian") return ScalingFilter::Gaussian;
    if (str == "ScaleForce") return ScalingFilter::ScaleForce;
    if (str == "Fsr") return ScalingFilter::Fsr;
    if (str == "MaxEnum") return ScalingFilter::MaxEnum;
    return ScalingFilter::NearestNeighbor;
}

const char* AntiAliasingToString(AntiAliasing value)
{
    switch (value) {
    case AntiAliasing::None: return "None";
    case AntiAliasing::Fxaa: return "Fxaa";
    case AntiAliasing::Smaa: return "Smaa";
    case AntiAliasing::MaxEnum: return "MaxEnum";
    }
    return "None";
}

AntiAliasing AntiAliasingFromString(std::string_view str)
{
    if (str == "None") return AntiAliasing::None;
    if (str == "Fxaa") return AntiAliasing::Fxaa;
    if (str == "Smaa") return AntiAliasing::Smaa;
    if (str == "MaxEnum") return AntiAliasing::MaxEnum;
    return AntiAliasing::None;
}

const char* AspectRatioToString(AspectRatio value)
{
    switch (value) {
    case AspectRatio::R16_9: return "R16_9";
    case AspectRatio::R4_3: return "R4_3";
    case AspectRatio::R21_9: return "R21_9";
    case AspectRatio::R16_10: return "R16_10";
    case AspectRatio::Stretch: return "Stretch";
    }
    return "R16_9";
}

AspectRatio AspectRatioFromString(std::string_view str)
{
    if (str == "R16_9") return AspectRatio::R16_9;
    if (str == "R4_3") return AspectRatio::R4_3;
    if (str == "R21_9") return AspectRatio::R21_9;
    if (str == "R16_10") return AspectRatio::R16_10;
    if (str == "Stretch") return AspectRatio::Stretch;
    return AspectRatio::R16_9;
}
