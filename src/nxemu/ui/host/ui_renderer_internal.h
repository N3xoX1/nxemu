#pragma once

namespace UiRendererInternal
{
constexpr VkShaderStageFlags kConstantStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
constexpr uint32_t kConstantSize = 80;
constexpr int kMaxGradientStops = 16;
constexpr int kBlurWeights = 4;
struct GeometryConstants
{
    float transform[16];
    float translate[2];
};

inline void CopyMatrix(float out[16], const Rml::Matrix4f & matrix)
{
    const Rml::ColumnMajorMatrix4f columns = matrix;
    std::memcpy(out, columns.data(), sizeof(float) * 16);
}
} // namespace UiRendererInternal

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
    float blendFactor = 1;
};
