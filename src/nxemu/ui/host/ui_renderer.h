#pragma once
#include "vulkan/vma.h"
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Types.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class UiRenderer final : public Rml::RenderInterface
{
public:
    UiRenderer();
    ~UiRenderer() override;

    bool Bind(const VulkanContext & context);
    void Unbind();
    bool IsBound() const { return m_context.device != VK_NULL_HANDLE; }
    const std::string & Error() const { return m_error; }

    void BeginFrame(VkCommandBuffer commandBuffer, int width, int height);
    void EndFrame(VkImage output);

    // RenderInterface
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i & dimensions, const Rml::String & source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;
    void EnableClipMask(bool enable) override;
    void RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation) override;
    void SetTransform(const Rml::Matrix4f * transform) override;

    Rml::LayerHandle PushLayer() override;
    void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blendMode,
                         Rml::Span<const Rml::CompiledFilterHandle> filters) override;
    void PopLayer() override;
    Rml::TextureHandle SaveLayerAsTexture() override;
    Rml::CompiledFilterHandle SaveLayerAsMaskImage() override;

    Rml::CompiledFilterHandle CompileFilter(const Rml::String & name, const Rml::Dictionary & parameters) override;
    void ReleaseFilter(Rml::CompiledFilterHandle filter) override;

    Rml::CompiledShaderHandle CompileShader(const Rml::String & name, const Rml::Dictionary & parameters) override;
    void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                      Rml::TextureHandle texture) override;
    void ReleaseShader(Rml::CompiledShaderHandle shader) override;

private:
    UiRenderer(const UiRenderer &) = delete;
    UiRenderer & operator=(const UiRenderer &) = delete;

    enum class Program : uint8_t
    {
        Color,
        Texture,
        Gradient,
        Passthrough,
        ColorMatrix,
        BlendMask,
        Blur,
        DropShadow,
        Count,
    };
    enum class Blend : uint8_t
    {
        Premultiplied,
        Replace,
        // Scales the source by the blend constants and replaces the destination.
        Constant,
    };
    enum class Stencil : uint8_t
    {
        Off,
        // Draws only where the clip mask equals the reference value.
        Test,
        // The two below write the clip mask and leave the colour untouched.
        Replace,
        Increment,
    };
    enum class Scratch : uint8_t
    {
        Primary,
        Secondary,
        Tertiary,
        BlendMask,
        Count,
    };

    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void * mapped = nullptr;
    };
    struct DescriptorSet
    {
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
    };
    struct Image
    {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    struct Geometry
    {
        Buffer buffer;
        VkDeviceSize indexOffset = 0;
        uint32_t indexCount = 0;
    };
    struct Texture
    {
        Image image;
        DescriptorSet set;
    };
    struct Upload
    {
        Texture * texture = nullptr;
        Buffer staging;
        VkExtent2D extent{};
    };
    struct Target
    {
        Image image;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        DescriptorSet set;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool hasStencil = false;
        std::chrono::steady_clock::time_point lastUsed{};
    };
    struct GradientShader
    {
        std::vector<uint8_t> data;
        Buffer uniforms;
        DescriptorSet set;
    };
    struct Filter;
    struct FullscreenConstants;

    bool Fail(const std::string & message);
    bool CreateAllocator();
    bool ChooseFormats();
    bool CreateSamplers();
    bool CreateLayouts();
    bool CreateRenderPasses();
    bool CreateShaderModules();
    VkPipeline PipelineFor(Program program, Blend blend, Stencil stencil, bool hasStencil);

    Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage);
    void DestroyBuffer(Buffer & buffer);
    bool CreateImage(Image & image, VkExtent2D extent, VkFormat format, VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                     VkImageAspectFlags aspect);
    void DestroyImage(Image & image);
    DescriptorSet AllocateSet(VkDescriptorSetLayout layout);
    void FreeSet(DescriptorSet & set);
    void WriteImageSet(const DescriptorSet & set, VkImageView view, VkSampler sampler);
    Texture * CreateTexture(VkExtent2D extent);
    void DestroyTexture(Texture * texture);
    void Retire(std::function<void()> destroy);
    void FlushRetired();
    void FlushUploads();

    bool CreateTarget(Target & target, bool hasStencil);
    void DestroyTarget(Target & target);
    void DestroyTargets();
    void ReleaseUnusedTargets();
    Target & TopLayer();
    Target & ScratchTarget(Scratch slot);
    void SwapScratch(Scratch first, Scratch second);

    void BeginPass(Target & target);
    void EndPass();
    void Transition(Target & target, VkImageLayout layout);
    void SetViewportSize(int width, int height);
    void SetScissor(Rml::Rectanglei region);
    VkRect2D ScissorRect() const;
    void ClearColor(Target & target, VkRect2D rect);
    void BindPipeline(Program program, Blend blend, Stencil stencil);
    Stencil ClipStencil(const Target & target) const;
    void DrawGeometry(const Geometry & geometry, Rml::Vector2f translation, Program program, VkPipelineLayout layout, VkDescriptorSet set, Stencil stencil, uint32_t stencilReference);
    void DrawFullscreen(Program program, Target & source, Target & destination, Blend blend, const FullscreenConstants & constants,
                        Target * mask = nullptr);
    void ResolveLayer(Target & layer);
    void RenderFilters(Rml::Span<const Rml::CompiledFilterHandle> filters);
    void RenderBlur(float sigma, Scratch sourceDestination, Scratch temporary, Rml::Rectanglei window);
    void BlitScaled(Target & source, Rml::Rectanglei sourceRect, Target & destination, Rml::Rectanglei destinationRect);

    bool CreateGradientResources(GradientShader & shader);
    void DestroyGradientResources(GradientShader & shader);

    VulkanContext m_context;
    VulkanApi m_api;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    std::string m_error;

    VkFormat m_stencilFormat = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits m_samples = VK_SAMPLE_COUNT_1_BIT;
    VkSampler m_textureSampler = VK_NULL_HANDLE;
    VkSampler m_targetSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_imageSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_uniformSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_imageLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_uniformLayout = VK_NULL_HANDLE;
    VkRenderPass m_layerPass = VK_NULL_HANDLE;
    VkRenderPass m_scratchPass = VK_NULL_HANDLE;
    VkShaderModule m_geometryVertex = VK_NULL_HANDLE;
    VkShaderModule m_fullscreenVertex = VK_NULL_HANDLE;
    std::array<VkShaderModule, static_cast<size_t>(Program::Count)> m_fragments{};
    std::unordered_map<uint32_t, VkPipeline> m_pipelines;
    std::vector<VkDescriptorPool> m_descriptorPools;

    std::unordered_set<GradientShader *> m_gradients;
    std::vector<std::function<void()>> m_retired;
    std::vector<Upload> m_uploads;
    std::unordered_map<std::string, std::vector<uint8_t>> m_memoryImages;

    Image m_stencil;
    bool m_stencilReady = false;
    std::vector<std::unique_ptr<Target>> m_layers;
    size_t m_layerCount = 0;
    std::array<Target, static_cast<size_t>(Scratch::Count)> m_scratch;
    std::array<uint8_t, static_cast<size_t>(Scratch::Count)> m_scratchSlots{};

    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    VkExtent2D m_extent{};
    std::chrono::steady_clock::time_point m_frameTime{};
    VkExtent2D m_viewport{};
    Target * m_passTarget = nullptr;
    VkPipeline m_boundPipeline = VK_NULL_HANDLE;
    Rml::Matrix4f m_projection;
    Rml::Matrix4f m_transform;
    Rml::Rectanglei m_scissor;
    bool m_clipMask = false;
    uint32_t m_stencilReference = 1;
};
