#pragma once

#include <nxemu-module-spec/video.h>
#include <string_view>

const char* AnisotropyModeToString(AnisotropyMode value);
AnisotropyMode AnisotropyModeFromString(std::string_view str);

const char* AstcDecodeModeToString(AstcDecodeMode value);
AstcDecodeMode AstcDecodeModeFromString(std::string_view str);

const char* AstcRecompressionToString(AstcRecompression value);
AstcRecompression AstcRecompressionFromString(std::string_view str);

const char* VSyncModeToString(VSyncMode value);
VSyncMode VSyncModeFromString(std::string_view str);

const char* VramUsageModeToString(VramUsageMode value);
VramUsageMode VramUsageModeFromString(std::string_view str);

const char* RendererBackendToString(RendererBackend value);
RendererBackend RendererBackendFromString(std::string_view str);

const char* ShaderBackendToString(ShaderBackend value);
ShaderBackend ShaderBackendFromString(std::string_view str);

const char* GpuAccuracyToString(GpuAccuracy value);
GpuAccuracy GpuAccuracyFromString(std::string_view str);

const char* FullscreenModeToString(FullscreenMode value);
FullscreenMode FullscreenModeFromString(std::string_view str);

const char* NvdecEmulationToString(NvdecEmulation value);
NvdecEmulation NvdecEmulationFromString(std::string_view str);

const char* ResolutionSetupToString(ResolutionSetup value);
ResolutionSetup ResolutionSetupFromString(std::string_view str);

const char* ScalingFilterToString(ScalingFilter value);
ScalingFilter ScalingFilterFromString(std::string_view str);

const char* AntiAliasingToString(AntiAliasing value);
AntiAliasing AntiAliasingFromString(std::string_view str);

const char* AspectRatioToString(AspectRatio value);
AspectRatio AspectRatioFromString(std::string_view str);
