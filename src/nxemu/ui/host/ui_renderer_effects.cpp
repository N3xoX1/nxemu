#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/Math.h>
#include <algorithm>
#include <cmath>

#include <ui_shaders/blend_mask_frag_spv.h>
#include <ui_shaders/blur_frag_spv.h>
#include <ui_shaders/color_frag_spv.h>
#include <ui_shaders/color_matrix_frag_spv.h>
#include <ui_shaders/drop_shadow_frag_spv.h>
#include <ui_shaders/fullscreen_vert_spv.h>
#include <ui_shaders/geometry_vert_spv.h>
#include <ui_shaders/gradient_frag_spv.h>
#include <ui_shaders/passthrough_frag_spv.h>
#include <ui_shaders/texture_frag_spv.h>

static_assert(sizeof(UiRendererInternal::GeometryConstants) <= UiRendererInternal::kConstantSize);

enum class FilterType
{
    Passthrough,
    Blur,
    DropShadow,
    ColorMatrix,
    MaskImage,
};

struct UiRenderer::Filter
{
    FilterType type;
    // Passthrough
    float blendFactor = 1;
    // Blur, drop shadow
    float sigma = 0;
    // Drop shadow
    Rml::Vector2f offset;
    Rml::ColourbPremultiplied color;
    // Colour matrix
    Rml::Matrix4f colorMatrix;
};

namespace
{
enum class GradientFunction : int32_t
{
    Linear,
    Radial,
    Conic,
    RepeatingLinear,
    RepeatingRadial,
    RepeatingConic,
};

struct GradientUniforms
{
    int32_t function;
    int32_t numStops;
    float p[2];
    float v[2];
    float padding[2];
    float stopColors[UiRendererInternal::kMaxGradientStops][4];
    float stopPositions[UiRendererInternal::kMaxGradientStops][4];
};
static_assert(offsetof(GradientUniforms, stopColors) == 32);

void SigmaToParameters(float desiredSigma, int & passLevel, float & sigma)
{
    constexpr int kMaxPasses = 10;
    constexpr float kMaxSinglePassSigma = 3.0f;
    passLevel = Rml::Math::Clamp(Rml::Math::Log2(static_cast<int>(desiredSigma * (2.f / kMaxSinglePassSigma))), 0, kMaxPasses);
    sigma = Rml::Math::Clamp(desiredSigma / static_cast<float>(1 << passLevel), 0.0f, kMaxSinglePassSigma);
}

void SetBlurWeights(float weights[UiRendererInternal::kBlurWeights], float sigma)
{
    float normalization = 0.0f;
    for (int i = 0; i < UiRendererInternal::kBlurWeights; i++)
    {
        if (std::abs(sigma) < 0.1f)
        {
            weights[i] = static_cast<float>(i == 0);
        }
        else
        {
            weights[i] = std::exp(-static_cast<float>(i * i) / (2.0f * sigma * sigma)) / (std::sqrt(2.f * Rml::Math::RMLUI_PI) * sigma);
        }
        normalization += (i == 0 ? 1.f : 2.0f) * weights[i];
    }
    for (int i = 0; i < UiRendererInternal::kBlurWeights; i++)
    {
        weights[i] /= normalization;
    }
}

template <typename Constants>
void SetTexCoordLimits(Constants & effect, Rml::Rectanglei rect, VkExtent2D extent)
{
    const Rml::Vector2f size(static_cast<float>(extent.width), static_cast<float>(extent.height));
    const Rml::Vector2f min = (Rml::Vector2f(rect.p0) + Rml::Vector2f(0.5f)) / size;
    const Rml::Vector2f max = (Rml::Vector2f(rect.p1) - Rml::Vector2f(0.5f)) / size;
    effect.texCoordMin[0] = min.x;
    effect.texCoordMin[1] = min.y;
    effect.texCoordMax[0] = max.x;
    effect.texCoordMax[1] = max.y;
}

VkOffset3D Corner(Rml::Vector2i point)
{
    return {point.x, point.y, 0};
}
} // namespace

bool UiRenderer::CreateShaderModules()
{
    static_assert(offsetof(FullscreenConstants, blendFactor) == UiRendererInternal::kConstantSize);

    const auto create = [this](const uint32_t * code, size_t size, VkShaderModule & module) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = size;
        info.pCode = code;
        return m_api.CreateShaderModule(m_context.device, &info, nullptr, &module) == VK_SUCCESS;
    };
    const auto fragment = [this](Program program) -> VkShaderModule & { return m_fragments[static_cast<size_t>(program)]; };

    const bool created =
        create(geometry_vert_spv, sizeof(geometry_vert_spv), m_geometryVertex) &&
        create(fullscreen_vert_spv, sizeof(fullscreen_vert_spv), m_fullscreenVertex) &&
        create(color_frag_spv, sizeof(color_frag_spv), fragment(Program::Color)) &&
        create(texture_frag_spv, sizeof(texture_frag_spv), fragment(Program::Texture)) &&
        create(gradient_frag_spv, sizeof(gradient_frag_spv), fragment(Program::Gradient)) &&
        create(passthrough_frag_spv, sizeof(passthrough_frag_spv), fragment(Program::Passthrough)) &&
        create(color_matrix_frag_spv, sizeof(color_matrix_frag_spv), fragment(Program::ColorMatrix)) &&
        create(blend_mask_frag_spv, sizeof(blend_mask_frag_spv), fragment(Program::BlendMask)) &&
        create(blur_frag_spv, sizeof(blur_frag_spv), fragment(Program::Blur)) &&
        create(drop_shadow_frag_spv, sizeof(drop_shadow_frag_spv), fragment(Program::DropShadow));
    return created || Fail("Unable to create the interface's shaders");
}

void UiRenderer::DrawFullscreen(Program program, Target & source, Target & destination, Blend blend,
                                const FullscreenConstants & constants, Target * mask)
{
    Transition(source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (mask != nullptr)
    {
        Transition(*mask, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    BeginPass(destination);
    const Stencil stencil = ClipStencil(destination);
    BindPipeline(program, blend, stencil);
    if (m_boundPipeline == VK_NULL_HANDLE)
    {
        return;
    }
    if (stencil != Stencil::Off)
    {
        m_api.CmdSetStencilReference(m_commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, m_stencilReference);
    }
    if (blend == Blend::Constant)
    {
        const float factors[4] = {constants.blendFactor, constants.blendFactor, constants.blendFactor, constants.blendFactor};
        m_api.CmdSetBlendConstants(m_commandBuffer, factors);
    }
    const VkDescriptorSet sets[2] = {source.set.set, mask != nullptr ? mask->set.set : VK_NULL_HANDLE};
    m_api.CmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_imageLayout, 0, mask != nullptr ? 2 : 1, sets, 0, nullptr);
    m_api.CmdPushConstants(m_commandBuffer, m_imageLayout, UiRendererInternal::kConstantStages, 0, UiRendererInternal::kConstantSize, &constants);
    m_api.CmdDraw(m_commandBuffer, 3, 1, 0, 0);
}

void UiRenderer::ResolveLayer(Target & layer)
{
    const VkRect2D rect = ScissorRect();
    if (rect.extent.width == 0 || rect.extent.height == 0)
    {
        return;
    }
    Target & primary = ScratchTarget(Scratch::Primary);
    EndPass();
    Transition(layer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(primary, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    const VkImageSubresourceLayers color{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    const VkOffset3D offset{rect.offset.x, rect.offset.y, 0};
    const VkExtent3D extent{rect.extent.width, rect.extent.height, 1};
    if (layer.hasStencil && m_samples != VK_SAMPLE_COUNT_1_BIT)
    {
        const VkImageResolve region{color, offset, color, offset, extent};
        m_api.CmdResolveImage(m_commandBuffer, layer.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, primary.image.image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    else
    {
        const VkImageCopy region{color, offset, color, offset, extent};
        m_api.CmdCopyImage(m_commandBuffer, layer.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, primary.image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
}

void UiRenderer::BlitScaled(Target & source, Rml::Rectanglei sourceRect, Target & destination, Rml::Rectanglei destinationRect)
{
    if (sourceRect.Width() <= 0 || sourceRect.Height() <= 0 || destinationRect.Width() <= 0 || destinationRect.Height() <= 0)
    {
        return;
    }
    EndPass();
    Transition(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit region{};
    region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[0] = Corner(sourceRect.p0);
    region.srcOffsets[1] = Corner(sourceRect.p1);
    region.srcOffsets[1].z = 1;
    region.dstOffsets[0] = Corner(destinationRect.p0);
    region.dstOffsets[1] = Corner(destinationRect.p1);
    region.dstOffsets[1].z = 1;
    m_api.CmdBlitImage(m_commandBuffer, source.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.image.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
}

void UiRenderer::CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blendMode,
                                 Rml::Span<const Rml::CompiledFilterHandle> filters)
{
    ResolveLayer(*m_layers[source]);
    RenderFilters(filters);
    DrawFullscreen(Program::Passthrough, ScratchTarget(Scratch::Primary), *m_layers[destination],
                   blendMode == Rml::BlendMode::Replace ? Blend::Replace : Blend::Premultiplied, {});
}

Rml::TextureHandle UiRenderer::SaveLayerAsTexture()
{
    const VkRect2D bounds = ScissorRect();
    if (bounds.extent.width == 0 || bounds.extent.height == 0)
    {
        return {};
    }
    Texture * texture = CreateTexture(bounds.extent);
    if (texture == nullptr)
    {
        Rml::Log::Message(Rml::Log::LT_ERROR, "Failed to create render texture.");
        return {};
    }

    ResolveLayer(TopLayer());
    Target & primary = ScratchTarget(Scratch::Primary);
    Transition(primary, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VulkanTransitionImage(m_api, m_commandBuffer, texture->image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageSubresourceLayers color{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    const VkImageCopy region{color, {bounds.offset.x, bounds.offset.y, 0}, color, {0, 0, 0}, {bounds.extent.width, bounds.extent.height, 1}};
    m_api.CmdCopyImage(m_commandBuffer, primary.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture->image.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VulkanTransitionImage(m_api, m_commandBuffer, texture->image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return reinterpret_cast<Rml::TextureHandle>(texture);
}

Rml::CompiledFilterHandle UiRenderer::SaveLayerAsMaskImage()
{
    ResolveLayer(TopLayer());
    DrawFullscreen(Program::Passthrough, ScratchTarget(Scratch::Primary), ScratchTarget(Scratch::BlendMask), Blend::Replace, {});
    return reinterpret_cast<Rml::CompiledFilterHandle>(new Filter{FilterType::MaskImage});
}

Rml::CompiledFilterHandle UiRenderer::CompileFilter(const Rml::String & name, const Rml::Dictionary & parameters)
{
    const auto colorMatrix = [](const Rml::Matrix4f & matrix) {
        Filter filter{FilterType::ColorMatrix};
        filter.colorMatrix = matrix;
        return reinterpret_cast<Rml::CompiledFilterHandle>(new Filter(filter));
    };

    if (name == "opacity")
    {
        Filter filter{FilterType::Passthrough};
        filter.blendFactor = Rml::Get(parameters, "value", 1.0f);
        return reinterpret_cast<Rml::CompiledFilterHandle>(new Filter(filter));
    }
    if (name == "blur")
    {
        Filter filter{FilterType::Blur};
        filter.sigma = Rml::Get(parameters, "sigma", 1.0f);
        return reinterpret_cast<Rml::CompiledFilterHandle>(new Filter(filter));
    }
    if (name == "drop-shadow")
    {
        Filter filter{FilterType::DropShadow};
        filter.sigma = Rml::Get(parameters, "sigma", 0.f);
        filter.color = Rml::Get(parameters, "color", Rml::Colourb()).ToPremultiplied();
        filter.offset = Rml::Get(parameters, "offset", Rml::Vector2f(0.f));
        return reinterpret_cast<Rml::CompiledFilterHandle>(new Filter(filter));
    }
    if (name == "brightness")
    {
        const float value = Rml::Get(parameters, "value", 1.0f);
        return colorMatrix(Rml::Matrix4f::Diag(value, value, value, 1.f));
    }
    if (name == "contrast")
    {
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float grayness = 0.5f - 0.5f * value;
        Rml::Matrix4f matrix = Rml::Matrix4f::Diag(value, value, value, 1.f);
        matrix.SetColumn(3, Rml::Vector4f(grayness, grayness, grayness, 1.f));
        return colorMatrix(matrix);
    }
    if (name == "invert")
    {
        const float value = Rml::Math::Clamp(Rml::Get(parameters, "value", 1.0f), 0.f, 1.f);
        const float inverted = 1.f - 2.f * value;
        Rml::Matrix4f matrix = Rml::Matrix4f::Diag(inverted, inverted, inverted, 1.f);
        matrix.SetColumn(3, Rml::Vector4f(value, value, value, 1.f));
        return colorMatrix(matrix);
    }
    if (name == "grayscale")
    {
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float rest = 1.f - value;
        const Rml::Vector3f gray = value * Rml::Vector3f(0.2126f, 0.7152f, 0.0722f);
        return colorMatrix(Rml::Matrix4f::FromRows(
            {gray.x + rest, gray.y, gray.z, 0.f},
            {gray.x, gray.y + rest, gray.z, 0.f},
            {gray.x, gray.y, gray.z + rest, 0.f},
            {0.f, 0.f, 0.f, 1.f}));
    }
    if (name == "sepia")
    {
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float rest = 1.f - value;
        const Rml::Vector3f red = value * Rml::Vector3f(0.393f, 0.769f, 0.189f);
        const Rml::Vector3f green = value * Rml::Vector3f(0.349f, 0.686f, 0.168f);
        const Rml::Vector3f blue = value * Rml::Vector3f(0.272f, 0.534f, 0.131f);
        return colorMatrix(Rml::Matrix4f::FromRows(
            {red.x + rest, red.y, red.z, 0.f},
            {green.x, green.y + rest, green.z, 0.f},
            {blue.x, blue.y, blue.z + rest, 0.f},
            {0.f, 0.f, 0.f, 1.f}));
    }
    if (name == "hue-rotate")
    {
        // Hue rotation and saturation follow https://www.w3.org/TR/filter-effects-1/#attr-valuedef-type-huerotate
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float s = Rml::Math::Sin(value);
        const float c = Rml::Math::Cos(value);
        return colorMatrix(Rml::Matrix4f::FromRows(
            {0.213f + 0.787f * c - 0.213f * s, 0.715f - 0.715f * c - 0.715f * s, 0.072f - 0.072f * c + 0.928f * s, 0.f},
            {0.213f - 0.213f * c + 0.143f * s, 0.715f + 0.285f * c + 0.140f * s, 0.072f - 0.072f * c - 0.283f * s, 0.f},
            {0.213f - 0.213f * c - 0.787f * s, 0.715f - 0.715f * c + 0.715f * s, 0.072f + 0.928f * c + 0.072f * s, 0.f},
            {0.f, 0.f, 0.f, 1.f}));
    }
    if (name == "saturate")
    {
        const float value = Rml::Get(parameters, "value", 1.0f);
        return colorMatrix(Rml::Matrix4f::FromRows(
            {0.213f + 0.787f * value, 0.715f - 0.715f * value, 0.072f - 0.072f * value, 0.f},
            {0.213f - 0.213f * value, 0.715f + 0.285f * value, 0.072f - 0.072f * value, 0.f},
            {0.213f - 0.213f * value, 0.715f - 0.715f * value, 0.072f + 0.928f * value, 0.f},
            {0.f, 0.f, 0.f, 1.f}));
    }

    Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported filter type '%s'.", name.c_str());
    return {};
}

void UiRenderer::ReleaseFilter(Rml::CompiledFilterHandle filter)
{
    delete reinterpret_cast<Filter *>(filter);
}

void UiRenderer::RenderFilters(Rml::Span<const Rml::CompiledFilterHandle> filters)
{
    const Rml::Rectanglei window = m_scissor.Valid()
                                       ? m_scissor
                                       : Rml::Rectanglei::FromSize({static_cast<int>(m_extent.width), static_cast<int>(m_extent.height)});
    for (const Rml::CompiledFilterHandle handle : filters)
    {
        const Filter & filter = *reinterpret_cast<const Filter *>(handle);
        switch (filter.type)
        {
        case FilterType::Passthrough:
        {
            FullscreenConstants constants;
            constants.blendFactor = filter.blendFactor;
            DrawFullscreen(Program::Passthrough, ScratchTarget(Scratch::Primary), ScratchTarget(Scratch::Secondary), Blend::Constant, constants);
            SwapScratch(Scratch::Primary, Scratch::Secondary);
            break;
        }
        case FilterType::Blur:
            RenderBlur(filter.sigma, Scratch::Primary, Scratch::Secondary, window);
            break;
        case FilterType::DropShadow:
        {
            FullscreenConstants constants;
            for (int i = 0; i < 4; i++)
            {
                constants.effect.color[i] = filter.color[i] / 255.f;
            }
            SetTexCoordLimits(constants.effect, window, m_extent);
            constants.uvOffset[0] = -filter.offset.x / static_cast<float>(m_extent.width);
            constants.uvOffset[1] = -filter.offset.y / static_cast<float>(m_extent.height);
            DrawFullscreen(Program::DropShadow, ScratchTarget(Scratch::Primary), ScratchTarget(Scratch::Secondary), Blend::Replace, constants);
            if (filter.sigma >= 0.5f)
            {
                RenderBlur(filter.sigma, Scratch::Secondary, Scratch::Tertiary, window);
            }
            DrawFullscreen(Program::Passthrough, ScratchTarget(Scratch::Primary), ScratchTarget(Scratch::Secondary), Blend::Premultiplied, {});
            SwapScratch(Scratch::Primary, Scratch::Secondary);
            break;
        }
        case FilterType::ColorMatrix:
        {
            FullscreenConstants constants;
            UiRendererInternal::CopyMatrix(constants.matrix, filter.colorMatrix);
            DrawFullscreen(Program::ColorMatrix, ScratchTarget(Scratch::Primary), ScratchTarget(Scratch::Secondary), Blend::Replace, constants);
            SwapScratch(Scratch::Primary, Scratch::Secondary);
            break;
        }
        case FilterType::MaskImage:
            DrawFullscreen(Program::BlendMask, ScratchTarget(Scratch::Primary), ScratchTarget(Scratch::Secondary), Blend::Replace, {},
                           &ScratchTarget(Scratch::BlendMask));
            SwapScratch(Scratch::Primary, Scratch::Secondary);
            break;
        }
    }
}

void UiRenderer::RenderBlur(float sigma, Scratch sourceDestinationSlot, Scratch temporarySlot, Rml::Rectanglei window)
{
    if (window.Width() <= 0 || window.Height() <= 0)
    {
        return;
    }
    Target & sourceDestination = ScratchTarget(sourceDestinationSlot);
    Target & temporary = ScratchTarget(temporarySlot);
    const int width = static_cast<int>(m_extent.width);
    const int height = static_cast<int>(m_extent.height);

    int passLevel = 0;
    SigmaToParameters(sigma, passLevel, sigma);
    const Rml::Rectanglei originalScissor = m_scissor;

    Rml::Rectanglei scissor = window;
    SetViewportSize(width / 2, height / 2);
    FullscreenConstants downscale;
    downscale.uvScale[0] = width % 2 == 1 ? 1.f - 1.f / static_cast<float>(width) : 1.f;
    downscale.uvScale[1] = height % 2 == 1 ? 1.f - 1.f / static_cast<float>(height) : 1.f;
    for (int i = 0; i < passLevel; i++)
    {
        scissor.p0 = (scissor.p0 + Rml::Vector2i(1)) / 2;
        scissor.p1 = Rml::Math::Max(scissor.p1 / 2, scissor.p0);
        const bool fromSource = i % 2 == 0;
        SetScissor(scissor);
        DrawFullscreen(Program::Passthrough, fromSource ? sourceDestination : temporary, fromSource ? temporary : sourceDestination,
                       Blend::Replace, downscale);
    }
    SetViewportSize(width, height);
    SetScissor(scissor);

    if (passLevel % 2 == 0)
    {
        DrawFullscreen(Program::Passthrough, sourceDestination, temporary, Blend::Replace, {});
    }

    FullscreenConstants blur;
    SetBlurWeights(blur.effect.weights, sigma);
    SetTexCoordLimits(blur.effect, scissor, m_extent);

    blur.effect.texelOffset[0] = 0;
    blur.effect.texelOffset[1] = 1.f / static_cast<float>(height);
    DrawFullscreen(Program::Blur, temporary, sourceDestination, Blend::Replace, blur);

    SetScissor(scissor.Extend(1));
    ClearColor(temporary, ScissorRect());
    SetScissor(scissor);
    blur.effect.texelOffset[0] = 1.f / static_cast<float>(width);
    blur.effect.texelOffset[1] = 0;
    DrawFullscreen(Program::Blur, sourceDestination, temporary, Blend::Replace, blur);

    BlitScaled(temporary, scissor, sourceDestination, window);
    const Rml::Rectanglei exact = Rml::Rectanglei::FromPositionSize(scissor.p0 * (1 << passLevel), scissor.Size() * (1 << passLevel));
    if (exact.p0 != window.p0 || exact.p1 != window.p1)
    {
        BlitScaled(temporary, scissor, sourceDestination, exact);
    }

    SetScissor(originalScissor);
}

Rml::CompiledShaderHandle UiRenderer::CompileShader(const Rml::String & name, const Rml::Dictionary & parameters)
{
    GradientUniforms uniforms{};
    const bool repeating = Rml::Get(parameters, "repeating", false);
    Rml::Vector2f p, v;
    if (name == "linear-gradient")
    {
        uniforms.function = static_cast<int32_t>(repeating ? GradientFunction::RepeatingLinear : GradientFunction::Linear);
        p = Rml::Get(parameters, "p0", Rml::Vector2f(0.f));
        v = Rml::Get(parameters, "p1", Rml::Vector2f(0.f)) - p;
    }
    else if (name == "radial-gradient")
    {
        uniforms.function = static_cast<int32_t>(repeating ? GradientFunction::RepeatingRadial : GradientFunction::Radial);
        p = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
        v = Rml::Vector2f(1.f) / Rml::Get(parameters, "radius", Rml::Vector2f(1.f));
    }
    else if (name == "conic-gradient")
    {
        uniforms.function = static_cast<int32_t>(repeating ? GradientFunction::RepeatingConic : GradientFunction::Conic);
        p = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
        const float angle = Rml::Get(parameters, "angle", 0.f);
        v = {Rml::Math::Cos(angle), Rml::Math::Sin(angle)};
    }
    else
    {
        Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported shader type '%s'.", name.c_str());
        return {};
    }
    uniforms.p[0] = p.x;
    uniforms.p[1] = p.y;
    uniforms.v[0] = v.x;
    uniforms.v[1] = v.y;

    const auto stops = parameters.find("color_stop_list");
    if (stops == parameters.end() || stops->second.GetType() != Rml::Variant::COLORSTOPLIST)
    {
        return {};
    }
    const Rml::ColorStopList & stopList = stops->second.GetReference<Rml::ColorStopList>();
    uniforms.numStops = std::min(static_cast<int>(stopList.size()), UiRendererInternal::kMaxGradientStops);
    for (int i = 0; i < uniforms.numStops; i++)
    {
        uniforms.stopPositions[i][0] = stopList[i].position.number;
        for (int channel = 0; channel < 4; channel++)
        {
            uniforms.stopColors[i][channel] = stopList[i].color[channel] / 255.f;
        }
    }

    auto shader = std::make_unique<GradientShader>();
    shader->data.resize(sizeof(uniforms));
    std::memcpy(shader->data.data(), &uniforms, sizeof(uniforms));
    m_gradients.insert(shader.get());
    return reinterpret_cast<Rml::CompiledShaderHandle>(shader.release());
}

bool UiRenderer::CreateGradientResources(GradientShader & shader)
{
    shader.uniforms = CreateBuffer(shader.data.size(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    shader.set = AllocateSet(m_uniformSetLayout);
    if (shader.uniforms.mapped == nullptr || shader.set.set == VK_NULL_HANDLE)
    {
        DestroyGradientResources(shader);
        return false;
    }
    std::memcpy(shader.uniforms.mapped, shader.data.data(), shader.data.size());
    vmaFlushAllocation(m_allocator, shader.uniforms.allocation, 0, VK_WHOLE_SIZE);

    const VkDescriptorBufferInfo buffer{shader.uniforms.buffer, 0, shader.data.size()};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = shader.set.set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &buffer;
    m_api.UpdateDescriptorSets(m_context.device, 1, &write, 0, nullptr);
    return true;
}

void UiRenderer::DestroyGradientResources(GradientShader & shader)
{
    DestroyBuffer(shader.uniforms);
    FreeSet(shader.set);
}

void UiRenderer::RenderShader(Rml::CompiledShaderHandle shaderHandle, Rml::CompiledGeometryHandle geometryHandle,
                              Rml::Vector2f translation, Rml::TextureHandle /*texture*/)
{
    GradientShader & shader = *reinterpret_cast<GradientShader *>(shaderHandle);
    if (shader.set.set == VK_NULL_HANDLE && !CreateGradientResources(shader))
    {
        return;
    }
    const Geometry & geometry = *reinterpret_cast<const Geometry *>(geometryHandle);
    DrawGeometry(geometry, translation, Program::Gradient, m_uniformLayout, shader.set.set, ClipStencil(TopLayer()), m_stencilReference);
}

void UiRenderer::ReleaseShader(Rml::CompiledShaderHandle handle)
{
    GradientShader * shader = reinterpret_cast<GradientShader *>(handle);
    m_gradients.erase(shader);
    Retire([this, shader] {
        DestroyGradientResources(*shader);
        delete shader;
    });
}
