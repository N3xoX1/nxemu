#pragma once
#include "ui_renderer.h"
#include "vulkan/vulkan_api.h"
#include <cstddef>
#include <RmlUi/Core/Types.h>
#include <cstring>

// Data layouts shared between the renderer's sources and its shaders.
namespace UiRendererInternal
{
constexpr VkShaderStageFlags kConstantStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
constexpr uint32_t kConstantSize = 80;
constexpr int kMaxGradientStops = 16;
constexpr int kBlurWeights = 4;

// Push constants of geometry.vert.
struct GeometryConstants
{
    float transform[16];
    float translate[2];
};

inline void CopyMatrix(float out[16], const Rml::Matrix4f & matrix)
{
    // Shaders read matrices by column, whichever storage RmlUi was built with.
    const Rml::ColumnMajorMatrix4f columns = matrix;
    std::memcpy(out, columns.data(), sizeof(float) * 16);
}
} // namespace UiRendererInternal

// Push constants of fullscreen.vert and of the fragment shaders drawn with it.
// The colour matrix program reads `matrix` where the others read `effect`.
struct UiRenderer::FullscreenConstants
{
    struct Effect
    {
        float texCoordMin[2] = {0, 0};
        float texCoordMax[2] = {1, 1};
        float color[4] = {0, 0, 0, 0};
        float texelOffset[2] = {0, 0};
        float padding[2] = {0, 0};
        float weights[UiRendererInternal::kBlurWeights] = {0, 0, 0, 0};
    };

    float uvScale[2] = {1, 1};
    float uvOffset[2] = {0, 0};
    union
    {
        Effect effect{};
        float matrix[16];
    };
    // Not sent to the shaders: the factor of Blend::Constant.
    float blendFactor = 1;
};
