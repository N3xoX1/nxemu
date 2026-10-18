#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Log.h>
#include <algorithm>
#include <cstring>
#include <stb_image.h>

namespace
{
// Contains a colon before any slash, so RmlUi passes it through unchanged
// instead of resolving it against the document's directory.
constexpr const char * kMemoryScheme = "nxmem:";

// The interface blends in nonlinear space, like browsers do.
constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t kSetsPerPool = 256;
constexpr VkDeviceSize kMemoryBlockBytes = 16 * 1024 * 1024;

std::vector<uint8_t> ReadFile(const Rml::String & source)
{
    Rml::FileInterface * files = Rml::GetFileInterface();
    const Rml::FileHandle handle = files->Open(source);
    if (!handle)
    {
        return {};
    }
    std::vector<uint8_t> data(files->Length(handle));
    const size_t read = files->Read(data.data(), data.size(), handle);
    files->Close(handle);
    data.resize(read);
    return data;
}

VkExtent2D ExtentOf(Rml::Vector2i size)
{
    return {static_cast<uint32_t>(std::max(size.x, 1)), static_cast<uint32_t>(std::max(size.y, 1))};
}
} // namespace

UiRenderer::UiRenderer()
{
    m_scissor = Rml::Rectanglei::MakeInvalid();
}

UiRenderer::~UiRenderer()
{
    Unbind();
}

bool UiRenderer::Bind(const VulkanContext & context)
{
    Unbind();
    if (!m_api.Load(context.getInstanceProcAddr, context.instance, context.device))
    {
        return Fail("The video module's Vulkan device lacks functions the interface needs");
    }
    m_context = context;
    m_stencilFormat = VK_FORMAT_UNDEFINED;
    for (size_t i = 0; i < m_scratchSlots.size(); i++)
    {
        m_scratchSlots[i] = static_cast<uint8_t>(i);
    }
    if (CreateAllocator() && ChooseFormats() && CreateSamplers() && CreateLayouts() && CreateRenderPasses() && CreateShaderModules())
    {
        return true;
    }
    Unbind();
    return false;
}

void UiRenderer::Unbind()
{
    if (!IsBound())
    {
        return;
    }
    for (Upload & upload : m_uploads)
    {
        DestroyBuffer(upload.staging);
    }
    m_uploads.clear();
    for (GradientShader * shader : m_gradients)
    {
        DestroyGradientResources(*shader);
    }
    FlushRetired();
    DestroyTargets();
    m_extent = {};

    for (const auto & [key, pipeline] : m_pipelines)
    {
        m_api.DestroyPipeline(m_context.device, pipeline, nullptr);
    }
    m_pipelines.clear();
    for (VkShaderModule & module : m_fragments)
    {
        m_api.DestroyShaderModule(m_context.device, module, nullptr);
        module = VK_NULL_HANDLE;
    }
    m_api.DestroyShaderModule(m_context.device, m_geometryVertex, nullptr);
    m_api.DestroyShaderModule(m_context.device, m_fullscreenVertex, nullptr);
    m_api.DestroyRenderPass(m_context.device, m_layerPass, nullptr);
    m_api.DestroyRenderPass(m_context.device, m_scratchPass, nullptr);
    m_api.DestroyPipelineLayout(m_context.device, m_imageLayout, nullptr);
    m_api.DestroyPipelineLayout(m_context.device, m_uniformLayout, nullptr);
    for (VkDescriptorPool pool : m_descriptorPools)
    {
        m_api.DestroyDescriptorPool(m_context.device, pool, nullptr);
    }
    m_descriptorPools.clear();
    m_api.DestroyDescriptorSetLayout(m_context.device, m_imageSetLayout, nullptr);
    m_api.DestroyDescriptorSetLayout(m_context.device, m_uniformSetLayout, nullptr);
    m_api.DestroySampler(m_context.device, m_textureSampler, nullptr);
    m_api.DestroySampler(m_context.device, m_targetSampler, nullptr);
    if (m_allocator != VK_NULL_HANDLE)
    {
        vmaDestroyAllocator(m_allocator);
    }
    m_geometryVertex = m_fullscreenVertex = VK_NULL_HANDLE;
    m_layerPass = m_scratchPass = VK_NULL_HANDLE;
    m_imageLayout = m_uniformLayout = VK_NULL_HANDLE;
    m_imageSetLayout = m_uniformSetLayout = VK_NULL_HANDLE;
    m_textureSampler = m_targetSampler = VK_NULL_HANDLE;
    m_allocator = VK_NULL_HANDLE;
    m_commandBuffer = VK_NULL_HANDLE;
    m_boundPipeline = VK_NULL_HANDLE;
    m_context = {};
}

bool UiRenderer::Fail(const std::string & message)
{
    m_error = message;
    return false;
}

bool UiRenderer::CreateAllocator()
{
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = m_api.GetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = m_api.GetDeviceProcAddr;

    VmaAllocatorCreateInfo info{};
    info.instance = m_context.instance;
    info.physicalDevice = m_context.physicalDevice;
    info.device = m_context.device;
    info.pVulkanFunctions = &functions;
    info.vulkanApiVersion = VK_API_VERSION_1_0;
    // The allocator takes memory from the device a block at a time, and its
    // usual block is far more than an interface's textures ever fill.
    info.preferredLargeHeapBlockSize = kMemoryBlockBytes;
    if (vmaCreateAllocator(&info, &m_allocator) != VK_SUCCESS)
    {
        return Fail("Unable to create the interface's memory allocator");
    }
    return true;
}

bool UiRenderer::ChooseFormats()
{
    for (VkFormat format : {VK_FORMAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT})
    {
        VkFormatProperties properties{};
        m_api.GetPhysicalDeviceFormatProperties(m_context.physicalDevice, format, &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
        {
            m_stencilFormat = format;
            break;
        }
    }
    if (m_stencilFormat == VK_FORMAT_UNDEFINED)
    {
        return Fail("The graphics device has no stencil buffer format");
    }

    // Geometry edges are only antialiased by multisampling.
    VkPhysicalDeviceProperties properties{};
    m_api.GetPhysicalDeviceProperties(m_context.physicalDevice, &properties);
    VkImageFormatProperties color{}, stencil{};
    if (m_api.GetPhysicalDeviceImageFormatProperties(m_context.physicalDevice, kColorFormat, VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &color) != VK_SUCCESS ||
        m_api.GetPhysicalDeviceImageFormatProperties(m_context.physicalDevice, m_stencilFormat, VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, &stencil) != VK_SUCCESS)
    {
        return Fail("The graphics device cannot create the interface's colour and stencil images");
    }
    // Framebuffer limits alone do not guarantee 4x support for these formats
    // and usage flags. Both attachments must support the chosen sample count.
    const VkSampleCountFlags supported = properties.limits.framebufferColorSampleCounts &
        properties.limits.framebufferStencilSampleCounts & color.sampleCounts & stencil.sampleCounts;
    m_samples = (supported & VK_SAMPLE_COUNT_4_BIT) != 0 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;
    return true;
}

std::vector<UiRenderer::GraphicsDevice> UiRenderer::GraphicsDevices() const
{
    std::vector<GraphicsDevice> result;
    if (!IsBound())
    {
        return result;
    }
    uint32_t count = 0;
    if (m_api.EnumeratePhysicalDevices(m_context.instance, &count, nullptr) != VK_SUCCESS || count == 0)
    {
        return result;
    }
    std::vector<VkPhysicalDevice> devices(count);
    const VkResult listed = m_api.EnumeratePhysicalDevices(m_context.instance, &count, devices.data());
    if (listed != VK_SUCCESS && listed != VK_INCOMPLETE)
    {
        return result;
    }
    devices.resize(count);
    for (const VkPhysicalDevice device : devices)
    {
        VkPhysicalDeviceProperties properties{};
        m_api.GetPhysicalDeviceProperties(device, &properties);
        result.push_back({properties.deviceName, properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, device == m_context.physicalDevice});
    }
    return result;
}

bool UiRenderer::CreateSamplers()
{
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (m_api.CreateSampler(m_context.device, &info, nullptr, &m_textureSampler) != VK_SUCCESS)
    {
        return Fail("Unable to create the interface's texture sampler");
    }
    // Reads outside a target must not bring its edge pixels back in.
    info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (m_api.CreateSampler(m_context.device, &info, nullptr, &m_targetSampler) != VK_SUCCESS)
    {
        return Fail("Unable to create the interface's target sampler");
    }
    return true;
}

bool UiRenderer::CreateLayouts()
{
    const auto createSetLayout = [this](VkDescriptorType type, VkDescriptorSetLayout & layout) {
        VkDescriptorSetLayoutBinding binding{};
        binding.descriptorType = type;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = 1;
        info.pBindings = &binding;
        return m_api.CreateDescriptorSetLayout(m_context.device, &info, nullptr, &layout) == VK_SUCCESS;
    };
    if (!createSetLayout(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_imageSetLayout) ||
        !createSetLayout(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, m_uniformSetLayout))
    {
        return Fail("Unable to create the interface's descriptor layouts");
    }

    VkPushConstantRange constants{};
    constants.stageFlags = UiRendererInternal::kConstantStages;
    constants.size = UiRendererInternal::kConstantSize;

    // Set 0 is the image a program reads, set 1 the mask of the mask program.
    const VkDescriptorSetLayout imageSets[] = {m_imageSetLayout, m_imageSetLayout};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 2;
    info.pSetLayouts = imageSets;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &constants;
    if (m_api.CreatePipelineLayout(m_context.device, &info, nullptr, &m_imageLayout) != VK_SUCCESS)
    {
        return Fail("Unable to create the interface's pipeline layout");
    }
    info.setLayoutCount = 1;
    info.pSetLayouts = &m_uniformSetLayout;
    if (m_api.CreatePipelineLayout(m_context.device, &info, nullptr, &m_uniformLayout) != VK_SUCCESS)
    {
        return Fail("Unable to create the interface's pipeline layout");
    }
    return true;
}

bool UiRenderer::CreateRenderPasses()
{
    // Passes keep what their targets hold: the interface is drawn in many
    // passes per frame, switching targets for every layer and filter.
    VkAttachmentDescription attachments[2]{};
    attachments[0].format = kColorFormat;
    attachments[0].samples = m_samples;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1].format = m_stencilFormat;
    attachments[1].samples = m_samples;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference stencil{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    subpass.pDepthStencilAttachment = &stencil;

    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 2;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    // Layers share a stencil attachment and may resume the same colour
    // attachment without a layout transition. Order both between passes.
    constexpr VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    constexpr VkAccessFlags reads = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    constexpr VkAccessFlags writes = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    const VkSubpassDependency dependencies[] = {
        {VK_SUBPASS_EXTERNAL, 0, stages, stages, writes, reads | writes, VK_DEPENDENCY_BY_REGION_BIT},
        {0, VK_SUBPASS_EXTERNAL, stages, stages, writes, reads | writes, VK_DEPENDENCY_BY_REGION_BIT},
    };
    info.dependencyCount = static_cast<uint32_t>(std::size(dependencies));
    info.pDependencies = dependencies;
    if (m_api.CreateRenderPass(m_context.device, &info, nullptr, &m_layerPass) != VK_SUCCESS)
    {
        return Fail("Unable to create the interface's layer render pass");
    }

    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    subpass.pDepthStencilAttachment = nullptr;
    info.attachmentCount = 1;
    if (m_api.CreateRenderPass(m_context.device, &info, nullptr, &m_scratchPass) != VK_SUCCESS)
    {
        return Fail("Unable to create the interface's filter render pass");
    }
    return true;
}

VkPipeline UiRenderer::PipelineFor(Program program, Blend blend, Stencil stencil, bool hasStencil)
{
    const uint32_t key = static_cast<uint32_t>(program) | static_cast<uint32_t>(blend) << 8 | static_cast<uint32_t>(stencil) << 16 |
                         static_cast<uint32_t>(hasStencil) << 24;
    if (const auto itr = m_pipelines.find(key); itr != m_pipelines.end())
    {
        return itr->second;
    }

    const bool drawsGeometry = program == Program::Color || program == Program::Texture || program == Program::Gradient;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = drawsGeometry ? m_geometryVertex : m_fullscreenVertex;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_fragments[static_cast<size_t>(program)];
    stages[1].pName = "main";

    const VkVertexInputBindingDescription binding{0, sizeof(Rml::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(Rml::Vertex, position))},
        {1, 0, VK_FORMAT_R8G8B8A8_UNORM, static_cast<uint32_t>(offsetof(Rml::Vertex, colour))},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(Rml::Vertex, tex_coord))},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (drawsGeometry)
    {
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 3;
        vertexInput.pVertexAttributeDescriptions = attributes;
    }

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = hasStencil ? m_samples : VK_SAMPLE_COUNT_1_BIT;

    VkStencilOpState stencilOps{};
    stencilOps.failOp = stencilOps.depthFailOp = stencilOps.passOp = VK_STENCIL_OP_KEEP;
    stencilOps.compareOp = VK_COMPARE_OP_EQUAL;
    stencilOps.compareMask = 0xff;
    stencilOps.writeMask = 0xff;
    if (stencil == Stencil::Replace || stencil == Stencil::Increment)
    {
        stencilOps.compareOp = VK_COMPARE_OP_ALWAYS;
        stencilOps.passOp = stencil == Stencil::Replace ? VK_STENCIL_OP_REPLACE : VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    }
    VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.stencilTestEnable = stencil != Stencil::Off;
    depthStencil.front = depthStencil.back = stencilOps;

    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = blend != Blend::Replace;
    attachment.colorBlendOp = attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    if (blend == Blend::Constant)
    {
        attachment.srcColorBlendFactor = attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
        attachment.dstColorBlendFactor = attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    }
    else
    {
        attachment.srcColorBlendFactor = attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstColorBlendFactor = attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    }
    const bool writesMask = stencil == Stencil::Replace || stencil == Stencil::Increment;
    attachment.colorWriteMask = writesMask ? 0 : VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &attachment;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                            VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<uint32_t>(std::size(dynamicStates));
    dynamic.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamic;
    info.layout = program == Program::Gradient ? m_uniformLayout : m_imageLayout;
    info.renderPass = hasStencil ? m_layerPass : m_scratchPass;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (m_api.CreateGraphicsPipelines(m_context.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
    {
        Rml::Log::Message(Rml::Log::LT_ERROR, "Unable to create an interface pipeline.");
        pipeline = VK_NULL_HANDLE;
    }
    m_pipelines.emplace(key, pipeline);
    return pipeline;
}

UiRenderer::Buffer UiRenderer::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    // Buffers are written once from the CPU; interface geometry is small
    // enough that keeping it in host-visible memory costs nothing.
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    Buffer buffer;
    VmaAllocationInfo result{};
    if (vmaCreateBuffer(m_allocator, &info, &allocation, &buffer.buffer, &buffer.allocation, &result) != VK_SUCCESS)
    {
        return {};
    }
    buffer.mapped = result.pMappedData;
    return buffer;
}

void UiRenderer::DestroyBuffer(Buffer & buffer)
{
    if (buffer.buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
    }
    buffer = {};
}

bool UiRenderer::CreateImage(Image & image, VkExtent2D extent, VkFormat format, VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                             VkImageAspectFlags aspect)
{
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(m_allocator, &info, &allocation, &image.image, &image.allocation, nullptr) != VK_SUCCESS)
    {
        image = {};
        return false;
    }

    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {aspect, 0, 1, 0, 1};
    if (m_api.CreateImageView(m_context.device, &view, nullptr, &image.view) != VK_SUCCESS)
    {
        DestroyImage(image);
        return false;
    }
    return true;
}

void UiRenderer::DestroyImage(Image & image)
{
    if (image.view != VK_NULL_HANDLE)
    {
        m_api.DestroyImageView(m_context.device, image.view, nullptr);
    }
    if (image.image != VK_NULL_HANDLE)
    {
        vmaDestroyImage(m_allocator, image.image, image.allocation);
    }
    image = {};
}

UiRenderer::DescriptorSet UiRenderer::AllocateSet(VkDescriptorSetLayout layout)
{
    const auto allocate = [&](VkDescriptorPool pool) {
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool = pool;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        DescriptorSet set;
        if (m_api.AllocateDescriptorSets(m_context.device, &info, &set.set) == VK_SUCCESS)
        {
            set.pool = pool;
            return set;
        }
        return DescriptorSet{};
    };
    // Scrolling or closing documents frees sets in older pools too. Reuse
    // them before growing the allocation for another batch of textures.
    for (auto pool = m_descriptorPools.rbegin(); pool != m_descriptorPools.rend(); ++pool)
    {
        DescriptorSet set = allocate(*pool);
        if (set.set != VK_NULL_HANDLE)
        {
            return set;
        }
    }

    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSetsPerPool},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSetsPerPool},
    };
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    info.maxSets = kSetsPerPool;
    info.poolSizeCount = static_cast<uint32_t>(std::size(sizes));
    info.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (m_api.CreateDescriptorPool(m_context.device, &info, nullptr, &pool) == VK_SUCCESS)
    {
        m_descriptorPools.push_back(pool);
        DescriptorSet set = allocate(pool);
        if (set.set != VK_NULL_HANDLE)
        {
            return set;
        }
    }
    Rml::Log::Message(Rml::Log::LT_ERROR, "Unable to allocate an interface descriptor set.");
    return {};
}

void UiRenderer::FreeSet(DescriptorSet & set)
{
    if (set.set != VK_NULL_HANDLE)
    {
        m_api.FreeDescriptorSets(m_context.device, set.pool, 1, &set.set);
    }
    set = {};
}

void UiRenderer::WriteImageSet(const DescriptorSet & set, VkImageView view, VkSampler sampler)
{
    if (set.set == VK_NULL_HANDLE)
    {
        return;
    }
    const VkDescriptorImageInfo image{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set.set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    m_api.UpdateDescriptorSets(m_context.device, 1, &write, 0, nullptr);
}

UiRenderer::Texture * UiRenderer::CreateTexture(VkExtent2D extent)
{
    auto texture = std::make_unique<Texture>();
    if (!CreateImage(texture->image, extent, kColorFormat, VK_SAMPLE_COUNT_1_BIT,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
    {
        return nullptr;
    }
    texture->set = AllocateSet(m_imageSetLayout);
    if (texture->set.set == VK_NULL_HANDLE)
    {
        DestroyImage(texture->image);
        return nullptr;
    }
    WriteImageSet(texture->set, texture->image.view, m_textureSampler);
    return texture.release();
}

void UiRenderer::DestroyTexture(Texture * texture)
{
    FreeSet(texture->set);
    DestroyImage(texture->image);
    delete texture;
}

void UiRenderer::Retire(std::function<void()> destroy)
{
    if (!IsBound())
    {
        // Nothing drawn with the device is left to wait for.
        destroy();
        return;
    }
    m_retired.push_back(std::move(destroy));
}

void UiRenderer::FlushRetired()
{
    // Destroying may retire more, so work on what is queued now.
    std::vector<std::function<void()>> retired;
    retired.swap(m_retired);
    for (const std::function<void()> & destroy : retired)
    {
        destroy();
    }
}

void UiRenderer::FlushUploads()
{
    if (m_uploads.empty() || m_commandBuffer == VK_NULL_HANDLE)
    {
        return;
    }
    EndPass();
    for (Upload & upload : m_uploads)
    {
        const VkImage image = upload.texture->image.image;
        VulkanTransitionImage(m_api, m_commandBuffer, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {upload.extent.width, upload.extent.height, 1};
        m_api.CmdCopyBufferToImage(m_commandBuffer, upload.staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VulkanTransitionImage(m_api, m_commandBuffer, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Retire([this, staging = upload.staging]() mutable { DestroyBuffer(staging); });
    }
    m_uploads.clear();
}

std::string UiRenderer::AddMemoryImage(std::vector<uint8_t> encoded)
{
    std::string source = kMemoryScheme + std::to_string(m_nextMemoryImage++);
    m_memoryImages.emplace(source, std::move(encoded));
    return source;
}

void UiRenderer::RemoveMemoryImage(const std::string & source)
{
    if (m_memoryImages.erase(source) != 0)
    {
        Rml::ReleaseTexture(source);
    }
}

void UiRenderer::BeginFrame(VkCommandBuffer commandBuffer, int width, int height)
{
    FlushRetired();
    m_frameTime = std::chrono::steady_clock::now();
    ReleaseUnusedTargets();

    const VkExtent2D extent = ExtentOf({width, height});
    if (extent.width != m_extent.width || extent.height != m_extent.height)
    {
        DestroyTargets();
        m_extent = extent;
    }
    m_commandBuffer = commandBuffer;
    m_viewport = m_extent;
    m_passTarget = nullptr;
    m_boundPipeline = VK_NULL_HANDLE;
    m_scissor = Rml::Rectanglei::MakeInvalid();
    m_clipMask = false;
    m_stencilReference = 1;

    // RmlUi's origin is the top-left corner, like Vulkan's. Depth is squeezed
    // from [-1, 1] into Vulkan's [0, 1] so transformed elements are not clipped.
    const Rml::Matrix4f depthRange = Rml::Matrix4f::FromRows({1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 0.5f, 0.5f}, {0, 0, 0, 1});
    m_projection = depthRange * Rml::Matrix4f::ProjectOrtho(0, static_cast<float>(m_extent.width), 0, static_cast<float>(m_extent.height), -10000, 10000);
    SetTransform(nullptr);

    FlushUploads();
    m_layerCount = 0;
    PushLayer();
}

void UiRenderer::EndFrame(VkImage output)
{
    Target & layer = TopLayer();
    EndPass();
    Transition(layer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkImageSubresourceLayers color{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    const VkExtent3D extent{m_extent.width, m_extent.height, 1};
    if (layer.hasStencil && m_samples != VK_SAMPLE_COUNT_1_BIT)
    {
        const VkImageResolve region{color, {}, color, {}, extent};
        m_api.CmdResolveImage(m_commandBuffer, layer.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    else
    {
        const VkImageCopy region{color, {}, color, {}, extent};
        m_api.CmdCopyImage(m_commandBuffer, layer.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    VulkanTransitionImage(m_api, m_commandBuffer, output, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_layerCount = 0;
    m_commandBuffer = VK_NULL_HANDLE;
}

Rml::CompiledGeometryHandle UiRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
{
    if (!IsBound())
    {
        return {};
    }
    const VkDeviceSize vertexSize = vertices.size() * sizeof(Rml::Vertex);
    const VkDeviceSize indexSize = indices.size() * sizeof(int);
    auto geometry = std::make_unique<Geometry>();
    geometry->buffer = CreateBuffer(vertexSize + indexSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (geometry->buffer.mapped == nullptr)
    {
        DestroyBuffer(geometry->buffer);
        return {};
    }
    std::memcpy(geometry->buffer.mapped, vertices.data(), vertexSize);
    std::memcpy(static_cast<uint8_t *>(geometry->buffer.mapped) + vertexSize, indices.data(), indexSize);
    vmaFlushAllocation(m_allocator, geometry->buffer.allocation, 0, VK_WHOLE_SIZE);
    geometry->indexOffset = vertexSize;
    geometry->indexCount = static_cast<uint32_t>(indices.size());
    return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry.release());
}

void UiRenderer::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle textureHandle)
{
    const Geometry & geometry = *reinterpret_cast<const Geometry *>(handle);
    const Texture * texture = reinterpret_cast<const Texture *>(textureHandle);
    FlushUploads();
    DrawGeometry(geometry, translation, texture != nullptr ? Program::Texture : Program::Color, m_imageLayout,
                 texture != nullptr ? texture->set.set : VK_NULL_HANDLE, ClipStencil(TopLayer()), m_stencilReference);
}

void UiRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle handle)
{
    Geometry * geometry = reinterpret_cast<Geometry *>(handle);
    Retire([this, geometry] {
        DestroyBuffer(geometry->buffer);
        delete geometry;
    });
}

std::string UiRenderer::FileImageSource(const std::string & utf8Path)
{
    // RmlUi takes a source starting with a slash as relative to the root and
    // removes that slash before opening the file.
    return utf8Path.starts_with('/') ? "/" + utf8Path : utf8Path;
}

Rml::TextureHandle UiRenderer::LoadTexture(Rml::Vector2i & dimensions, const Rml::String & source)
{
    std::vector<uint8_t> file;
    const std::vector<uint8_t> * encoded = &file;
    if (const auto itr = m_memoryImages.find(source); itr != m_memoryImages.end())
    {
        encoded = &itr->second;
    }
    else
    {
        file = ReadFile(source);
    }
    if (encoded->empty())
    {
        return {};
    }

    int width = 0, height = 0, channels = 0;
    stbi_uc * pixels = stbi_load_from_memory(encoded->data(), static_cast<int>(encoded->size()), &width, &height, &channels, 4);
    if (pixels == nullptr)
    {
        return {};
    }

    // RmlUi textures use premultiplied alpha.
    const size_t size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    for (size_t i = 0; i < size; i += 4)
    {
        const uint32_t alpha = pixels[i + 3];
        pixels[i + 0] = static_cast<stbi_uc>(pixels[i + 0] * alpha / 255);
        pixels[i + 1] = static_cast<stbi_uc>(pixels[i + 1] * alpha / 255);
        pixels[i + 2] = static_cast<stbi_uc>(pixels[i + 2] * alpha / 255);
    }

    dimensions = {width, height};
    const Rml::TextureHandle texture = GenerateTexture({pixels, size}, dimensions);
    stbi_image_free(pixels);
    return texture;
}

Rml::TextureHandle UiRenderer::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions)
{
    if (!IsBound())
    {
        return {};
    }
    const VkExtent2D extent = ExtentOf(dimensions);
    Texture * texture = CreateTexture(extent);
    if (texture == nullptr)
    {
        return {};
    }
    // The pixels reach the image at the next draw; textures are also created
    // outside frames, when there is no command buffer to record into.
    Upload upload;
    upload.texture = texture;
    upload.extent = extent;
    upload.staging = CreateBuffer(source.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (upload.staging.mapped == nullptr)
    {
        DestroyBuffer(upload.staging);
        DestroyTexture(texture);
        return {};
    }
    std::memcpy(upload.staging.mapped, source.data(), source.size());
    vmaFlushAllocation(m_allocator, upload.staging.allocation, 0, VK_WHOLE_SIZE);
    m_uploads.push_back(upload);
    return reinterpret_cast<Rml::TextureHandle>(texture);
}

void UiRenderer::ReleaseTexture(Rml::TextureHandle handle)
{
    Texture * texture = reinterpret_cast<Texture *>(handle);
    const auto pending = std::find_if(m_uploads.begin(), m_uploads.end(), [texture](const Upload & upload) { return upload.texture == texture; });
    if (pending != m_uploads.end())
    {
        DestroyBuffer(pending->staging);
        m_uploads.erase(pending);
    }
    Retire([this, texture] { DestroyTexture(texture); });
}

void UiRenderer::EnableScissorRegion(bool enable)
{
    // Enabling is always followed by SetScissorRegion.
    if (!enable)
    {
        SetScissor(Rml::Rectanglei::MakeInvalid());
    }
}

void UiRenderer::SetScissorRegion(Rml::Rectanglei region)
{
    SetScissor(region);
}

void UiRenderer::EnableClipMask(bool enable)
{
    m_clipMask = enable;
}

void UiRenderer::RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle handle, Rml::Vector2f translation)
{
    const Geometry & geometry = *reinterpret_cast<const Geometry *>(handle);
    Target & target = TopLayer();
    if (!target.hasStencil)
    {
        // Filter scratch targets have no stencil attachment.
        return;
    }

    const auto clearMask = [&](uint32_t value) {
        BeginPass(target);
        VkClearAttachment attachment{};
        attachment.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        attachment.clearValue.depthStencil = {1.0f, value};
        const VkClearRect rect{ScissorRect(), 0, 1};
        if (rect.rect.extent.width > 0 && rect.rect.extent.height > 0)
        {
            m_api.CmdClearAttachments(m_commandBuffer, 1, &attachment, 1, &rect);
        }
    };

    Stencil mode = Stencil::Replace;
    uint32_t writeValue = 1;
    uint32_t testValue = 1;
    switch (operation)
    {
    case Rml::ClipMaskOperation::Set:
        clearMask(0);
        break;
    case Rml::ClipMaskOperation::SetInverse:
        clearMask(1);
        writeValue = 0;
        break;
    case Rml::ClipMaskOperation::Intersect:
        mode = Stencil::Increment;
        testValue = m_stencilReference + 1;
        break;
    }
    DrawGeometry(geometry, translation, Program::Color, m_imageLayout, VK_NULL_HANDLE, mode, writeValue);
    m_stencilReference = testValue;
}

void UiRenderer::SetTransform(const Rml::Matrix4f * transform)
{
    m_transform = transform != nullptr ? m_projection * (*transform) : m_projection;
}

bool UiRenderer::CreateTarget(Target & target, bool hasStencil)
{
    target = {};
    target.hasStencil = hasStencil;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    (hasStencil ? 0 : VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!CreateImage(target.image, m_extent, kColorFormat, hasStencil ? m_samples : VK_SAMPLE_COUNT_1_BIT, usage, VK_IMAGE_ASPECT_COLOR_BIT))
    {
        return false;
    }
    if (hasStencil && m_stencil.image == VK_NULL_HANDLE)
    {
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
        if (m_stencilFormat != VK_FORMAT_S8_UINT)
        {
            aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        if (!CreateImage(m_stencil, m_extent, m_stencilFormat, m_samples, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, aspect))
        {
            DestroyTarget(target);
            return false;
        }
        m_stencilReady = false;
    }

    const VkImageView attachments[] = {target.image.view, m_stencil.view};
    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = hasStencil ? m_layerPass : m_scratchPass;
    info.attachmentCount = hasStencil ? 2 : 1;
    info.pAttachments = attachments;
    info.width = m_extent.width;
    info.height = m_extent.height;
    info.layers = 1;
    if (m_api.CreateFramebuffer(m_context.device, &info, nullptr, &target.framebuffer) != VK_SUCCESS)
    {
        DestroyTarget(target);
        return false;
    }
    if (!hasStencil)
    {
        target.set = AllocateSet(m_imageSetLayout);
        WriteImageSet(target.set, target.image.view, m_targetSampler);
    }
    return true;
}

void UiRenderer::DestroyTarget(Target & target)
{
    if (target.framebuffer != VK_NULL_HANDLE)
    {
        m_api.DestroyFramebuffer(m_context.device, target.framebuffer, nullptr);
    }
    FreeSet(target.set);
    DestroyImage(target.image);
    target = {};
}

void UiRenderer::DestroyTargets()
{
    for (std::unique_ptr<Target> & layer : m_layers)
    {
        DestroyTarget(*layer);
    }
    m_layers.clear();
    m_layerCount = 0;
    for (Target & target : m_scratch)
    {
        DestroyTarget(target);
    }
    DestroyImage(m_stencil);
    m_passTarget = nullptr;
}

void UiRenderer::ReleaseUnusedTargets()
{
    // Menus and filters temporarily need full-window images. Keep them warm
    // through animations, then release their memory during ordinary gameplay.
    // BeginFrame runs after the previous UI fence, so these images are idle.
    constexpr auto idle = std::chrono::seconds(5);
    while (m_layers.size() > 1 && m_frameTime - m_layers.back()->lastUsed >= idle)
    {
        DestroyTarget(*m_layers.back());
        m_layers.pop_back();
    }
    for (Target & target : m_scratch)
    {
        if (target.image.image != VK_NULL_HANDLE && m_frameTime - target.lastUsed >= idle)
        {
            DestroyTarget(target);
        }
    }
}

UiRenderer::Target & UiRenderer::TopLayer()
{
    return *m_layers[m_layerCount - 1];
}

UiRenderer::Target & UiRenderer::ScratchTarget(Scratch slot)
{
    Target & target = m_scratch[m_scratchSlots[static_cast<size_t>(slot)]];
    if (target.image.image == VK_NULL_HANDLE)
    {
        CreateTarget(target, false);
    }
    target.lastUsed = m_frameTime;
    return target;
}

void UiRenderer::SwapScratch(Scratch first, Scratch second)
{
    std::swap(m_scratchSlots[static_cast<size_t>(first)], m_scratchSlots[static_cast<size_t>(second)]);
}

Rml::LayerHandle UiRenderer::PushLayer()
{
    if (m_layerCount == m_layers.size())
    {
        m_layers.push_back(std::make_unique<Target>());
        CreateTarget(*m_layers.back(), true);
    }
    m_layerCount++;
    TopLayer().lastUsed = m_frameTime;
    ClearColor(TopLayer(), {{0, 0}, m_extent});
    return static_cast<Rml::LayerHandle>(m_layerCount - 1);
}

void UiRenderer::PopLayer()
{
    m_layerCount--;
}

void UiRenderer::BeginPass(Target & target)
{
    if (m_passTarget == &target)
    {
        return;
    }
    Transition(target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    EndPass();
    if (target.hasStencil && !m_stencilReady)
    {
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
        if (m_stencilFormat != VK_FORMAT_S8_UINT)
        {
            aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        VulkanTransitionImage(m_api, m_commandBuffer, m_stencil.image, aspect, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        m_stencilReady = true;
    }

    VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    info.renderPass = target.hasStencil ? m_layerPass : m_scratchPass;
    info.framebuffer = target.framebuffer;
    info.renderArea = {{0, 0}, m_extent};
    m_api.CmdBeginRenderPass(m_commandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    m_passTarget = &target;
    m_boundPipeline = VK_NULL_HANDLE;

    const VkViewport viewport{0, 0, static_cast<float>(m_viewport.width), static_cast<float>(m_viewport.height), 0, 1};
    m_api.CmdSetViewport(m_commandBuffer, 0, 1, &viewport);
    const VkRect2D scissor = ScissorRect();
    m_api.CmdSetScissor(m_commandBuffer, 0, 1, &scissor);
}

void UiRenderer::EndPass()
{
    if (m_passTarget != nullptr)
    {
        m_api.CmdEndRenderPass(m_commandBuffer);
        m_passTarget = nullptr;
    }
}

void UiRenderer::Transition(Target & target, VkImageLayout layout)
{
    if (target.layout == layout)
    {
        return;
    }
    // Layouts cannot change inside a render pass.
    EndPass();
    VulkanTransitionImage(m_api, m_commandBuffer, target.image.image, VK_IMAGE_ASPECT_COLOR_BIT, target.layout, layout);
    target.layout = layout;
}

void UiRenderer::SetViewportSize(int width, int height)
{
    m_viewport = ExtentOf({width, height});
    if (m_passTarget != nullptr)
    {
        const VkViewport viewport{0, 0, static_cast<float>(m_viewport.width), static_cast<float>(m_viewport.height), 0, 1};
        m_api.CmdSetViewport(m_commandBuffer, 0, 1, &viewport);
    }
}

void UiRenderer::SetScissor(Rml::Rectanglei region)
{
    m_scissor = region;
    if (m_passTarget != nullptr)
    {
        const VkRect2D scissor = ScissorRect();
        m_api.CmdSetScissor(m_commandBuffer, 0, 1, &scissor);
    }
}

VkRect2D UiRenderer::ScissorRect() const
{
    if (!m_scissor.Valid())
    {
        return {{0, 0}, m_extent};
    }
    const int width = static_cast<int>(m_extent.width);
    const int height = static_cast<int>(m_extent.height);
    const int left = std::clamp(m_scissor.Left(), 0, width);
    const int top = std::clamp(m_scissor.Top(), 0, height);
    const int right = std::clamp(m_scissor.Right(), left, width);
    const int bottom = std::clamp(m_scissor.Bottom(), top, height);
    return {{left, top}, {static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top)}};
}

void UiRenderer::ClearColor(Target & target, VkRect2D area)
{
    if (area.extent.width == 0 || area.extent.height == 0)
    {
        return;
    }
    BeginPass(target);
    VkClearAttachment attachment{};
    attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    const VkClearRect rect{area, 0, 1};
    m_api.CmdClearAttachments(m_commandBuffer, 1, &attachment, 1, &rect);
}

void UiRenderer::BindPipeline(Program program, Blend blend, Stencil stencil)
{
    const VkPipeline pipeline = PipelineFor(program, blend, stencil, m_passTarget->hasStencil);
    if (pipeline != m_boundPipeline && pipeline != VK_NULL_HANDLE)
    {
        m_api.CmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    }
    m_boundPipeline = pipeline;
}

UiRenderer::Stencil UiRenderer::ClipStencil(const Target & target) const
{
    return m_clipMask && target.hasStencil ? Stencil::Test : Stencil::Off;
}

void UiRenderer::DrawGeometry(const Geometry & geometry, Rml::Vector2f translation, Program program, VkPipelineLayout layout,
                              VkDescriptorSet set, Stencil stencil, uint32_t stencilReference)
{
    Target & target = TopLayer();
    if (!target.hasStencil)
    {
        // Filter scratch targets have no stencil attachment.
        if (stencil == Stencil::Replace || stencil == Stencil::Increment)
        {
            return;
        }
        stencil = Stencil::Off;
    }
    BeginPass(target);
    BindPipeline(program, Blend::Premultiplied, stencil);
    if (m_boundPipeline == VK_NULL_HANDLE)
    {
        return;
    }
    if (stencil != Stencil::Off)
    {
        m_api.CmdSetStencilReference(m_commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, stencilReference);
    }
    if (set != VK_NULL_HANDLE)
    {
        m_api.CmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    }

    UiRendererInternal::GeometryConstants constants{};
    UiRendererInternal::CopyMatrix(constants.transform, m_transform);
    constants.translate[0] = translation.x;
    constants.translate[1] = translation.y;
    m_api.CmdPushConstants(m_commandBuffer, layout, UiRendererInternal::kConstantStages, 0, sizeof(constants), &constants);

    const VkDeviceSize vertexOffset = 0;
    m_api.CmdBindVertexBuffers(m_commandBuffer, 0, 1, &geometry.buffer.buffer, &vertexOffset);
    m_api.CmdBindIndexBuffer(m_commandBuffer, geometry.buffer.buffer, geometry.indexOffset, VK_INDEX_TYPE_UINT32);
    m_api.CmdDrawIndexed(m_commandBuffer, geometry.indexCount, 1, 0, 0, 0);
}
