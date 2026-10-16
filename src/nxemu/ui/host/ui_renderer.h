#pragma once
#include "vulkan/vma.h"
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Types.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Draws the interface with Vulkan: the full RmlUi render interface, including
// clip masks, layers, filters and gradients.
//
// The renderer owns neither the device nor a swapchain: it draws with the video
// module's device. Each frame it records into a command buffer it is handed
// and leaves the result in an image it is handed, which the video module then
// lays over the game.
//
// That device goes away when the modules are replaced, while RmlUi keeps this
// renderer for as long as it runs. So the renderer is bound to a device and
// unbound from it, and what RmlUi holds across the two is rebuilt on demand.
class UiRenderer final : public Rml::RenderInterface
{
public:
    UiRenderer();
    ~UiRenderer() override;

    // Starts drawing with `context`. Returns false, with Error() set, when the
    // device cannot be used.
    bool Bind(const VulkanContext & context);
    // Releases everything created on the device. RmlUi has to have released
    // its textures and geometry first; frames recorded earlier must be done.
    void Unbind();
    bool IsBound() const { return m_context.device != VK_NULL_HANDLE; }
    const std::string & Error() const { return m_error; }

    // A graphics device of the computer, as the video module numbers them.
    struct GraphicsDevice
    {
        std::string name;
        bool integrated; // shares the processor's memory: the slow one of a computer that has two
        bool inUse;      // the one the renderer is bound to
    };
    // The devices a game can be drawn with. None when the renderer is unbound.
    std::vector<GraphicsDevice> GraphicsDevices() const;

    // The value to use as an <img> src for an image file given by its
    // absolute path.
    static std::string FileImageSource(const std::string & utf8Path);
    // Registers an encoded image and returns the value to use as an <img> src.
    std::string AddMemoryImage(std::vector<uint8_t> encoded);
    // Forgets an image no element shows any more.
    void RemoveMemoryImage(const std::string & source);

    // Starts recording a frame of the given size. Everything recorded for
    // earlier frames must have finished executing.
    void BeginFrame(VkCommandBuffer commandBuffer, int width, int height);
    // Finishes the frame into `output`, an R8G8B8A8_UNORM image of the frame's
    // size, initially in TRANSFER_DST_OPTIMAL: it receives the interface with
    // premultiplied alpha and is left in SHADER_READ_ONLY_OPTIMAL.
    void EndFrame(VkImage output);

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
    // Single-sample targets filters read from and write to.
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
    // An image the interface is drawn into.
    struct Target
    {
        Image image;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        DescriptorSet set;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        // Layers retain stencil clipping even when only 1x samples are supported.
        bool hasStencil = false;
        std::chrono::steady_clock::time_point lastUsed{};
    };
    struct GradientShader
    {
        // The uniform block, kept to rebuild the two below on another device.
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
    // Destroys a resource once the frames that may still use it are done.
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
    void DrawGeometry(const Geometry & geometry, Rml::Vector2f translation, Program program, VkPipelineLayout layout,
                      VkDescriptorSet set, Stencil stencil, uint32_t stencilReference);
    void DrawFullscreen(Program program, Target & source, Target & destination, Blend blend, const FullscreenConstants & constants,
                        Target * mask = nullptr);
    // Copies a layer into the primary scratch target, resolving its samples.
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

    // RmlUi keeps its compiled shaders across devices.
    std::unordered_set<GradientShader *> m_gradients;
    std::vector<std::function<void()>> m_retired;
    std::vector<Upload> m_uploads;
    std::unordered_map<std::string, std::vector<uint8_t>> m_memoryImages;
    uint64_t m_nextMemoryImage = 0;

    // Layers share one stencil buffer, so a clip mask carries across them.
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
