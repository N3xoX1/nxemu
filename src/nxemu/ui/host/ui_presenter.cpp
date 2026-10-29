#include "ui_presenter.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <yuzu_common/dynamic_library.h>
#include <yuzu_common/fs/path_util.h>

#ifdef _WIN32
extern "C" __declspec(dllimport) void * __stdcall GetModuleHandleW(const wchar_t * moduleName);
#endif

#define NXEMU_PRESENTER_INSTANCE_FUNCTIONS(X)  \
    X(DestroyInstance)                         \
    X(EnumeratePhysicalDevices)                \
    X(GetPhysicalDeviceProperties)             \
    X(GetPhysicalDeviceQueueFamilyProperties)  \
    X(GetPhysicalDeviceMemoryProperties)       \
    X(GetPhysicalDeviceFormatProperties)       \
    X(EnumerateDeviceExtensionProperties)      \
    X(CreateDevice)                            \
    X(GetDeviceProcAddr)                       \
    X(DestroySurfaceKHR)                       \
    X(GetPhysicalDeviceSurfaceSupportKHR)      \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(GetPhysicalDeviceSurfaceFormatsKHR)

#define NXEMU_PRESENTER_DEVICE_FUNCTIONS(X) \
    X(DestroyDevice)                        \
    X(GetDeviceQueue)                       \
    X(DeviceWaitIdle)                       \
    X(CreateCommandPool)                    \
    X(DestroyCommandPool)                   \
    X(AllocateCommandBuffers)               \
    X(ResetCommandBuffer)                   \
    X(BeginCommandBuffer)                   \
    X(EndCommandBuffer)                     \
    X(CreateFence)                          \
    X(DestroyFence)                         \
    X(WaitForFences)                        \
    X(ResetFences)                          \
    X(CreateSemaphore)                      \
    X(DestroySemaphore)                     \
    X(QueueSubmit)                          \
    X(CreateSwapchainKHR)                   \
    X(DestroySwapchainKHR)                  \
    X(GetSwapchainImagesKHR)                \
    X(AcquireNextImageKHR)                  \
    X(QueuePresentKHR)                      \
    X(CreateImage)                          \
    X(DestroyImage)                         \
    X(GetImageMemoryRequirements)           \
    X(AllocateMemory)                       \
    X(FreeMemory)                           \
    X(BindImageMemory)                      \
    X(CmdPipelineBarrier)                   \
    X(CmdBlitImage)                         \
    X(CmdCopyImage)

struct UiPresenter::Api
{
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_vkCreateInstance CreateInstance = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;

#define NXEMU_VK_DECLARE(name) PFN_vk##name name = nullptr;
    NXEMU_PRESENTER_INSTANCE_FUNCTIONS(NXEMU_VK_DECLARE)
    NXEMU_PRESENTER_DEVICE_FUNCTIONS(NXEMU_VK_DECLARE)
#undef NXEMU_VK_DECLARE
};

namespace
{
constexpr VkFormat kTargetFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t kSwapchainImages = 3;
constexpr char kPortabilitySubset[] = "VK_KHR_portability_subset";

struct Win32SurfaceCreateInfo
{
    VkStructureType sType;
    const void * pNext;
    VkFlags flags;
    void * hinstance;
    void * hwnd;
};
struct WaylandSurfaceCreateInfo
{
    VkStructureType sType;
    const void * pNext;
    VkFlags flags;
    void * display;
    void * surface;
};
struct MetalSurfaceCreateInfo
{
    VkStructureType sType;
    const void * pNext;
    VkFlags flags;
    const void * layer;
};
typedef VkResult(VKAPI_PTR * CreatePlatformSurface)(VkInstance instance, const void * createInfo, const VkAllocationCallbacks * allocator, VkSurfaceKHR * surface);

#if defined(_WIN32)
constexpr char kPlatformSurfaceExtension[] = "VK_KHR_win32_surface";
#elif defined(__APPLE__)
constexpr char kPlatformSurfaceExtension[] = "VK_EXT_metal_surface";
#else
constexpr char kPlatformSurfaceExtension[] = "VK_KHR_wayland_surface";
#endif

bool HasExtension(const std::vector<VkExtensionProperties> & extensions, const char * name)
{
    for (const VkExtensionProperties & extension : extensions)
    {
        if (std::strcmp(extension.extensionName, name) == 0)
        {
            return true;
        }
    }
    return false;
}

void Barrier(PFN_vkCmdPipelineBarrier pipelineBarrier, VkCommandBuffer commandBuffer, VkImage image, VkPipelineStageFlags sourceStage, VkAccessFlags sourceAccess,
             VkPipelineStageFlags destinationStage, VkAccessFlags destinationAccess, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    pipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}
} // namespace

UiPresenter::UiPresenter() :
    m_api(std::make_unique<Api>())
{
}

UiPresenter::~UiPresenter()
{
    if (m_context.device != VK_NULL_HANDLE)
    {
        m_api->DeviceWaitIdle(m_context.device);
        DestroySwapchain();
        DestroyTarget();
        m_api->DestroySemaphore(m_context.device, m_imageAcquired, nullptr);
        m_api->DestroyFence(m_context.device, m_frameDone, nullptr);
        m_api->DestroyCommandPool(m_context.device, m_commandPool, nullptr);
        m_api->DestroyDevice(m_context.device, nullptr);
    }
    if (m_surface != VK_NULL_HANDLE)
    {
        m_api->DestroySurfaceKHR(m_context.instance, m_surface, nullptr);
    }
    if (m_context.instance != VK_NULL_HANDLE)
    {
        m_api->DestroyInstance(m_context.instance, nullptr);
    }
}

bool UiPresenter::Create(IRenderWindow & window, int32_t deviceIndex)
{
    m_window = &window;
    return CreateInstance() && CreateSurface() && ChooseDevice(deviceIndex) && CreateDevice();
}

bool UiPresenter::Fail(const std::string & message)
{
    m_error = message;
    return false;
}

bool UiPresenter::CreateInstance()
{
    m_library = std::make_unique<Common::DynamicLibrary>();
#if defined(__APPLE__)
    const std::filesystem::path exeDir = Common::FS::GetExeDirectory();
    const bool bundled = exeDir.filename() == "MacOS" && exeDir.parent_path().filename() == "Contents";
    const std::filesystem::path moltenVk = bundled ? exeDir.parent_path() / "Frameworks/libMoltenVK.dylib" : exeDir / "libMoltenVK.dylib";
    if (!m_library->Open(moltenVk.string().c_str()))
    {
        return Fail("Unable to load MoltenVK");
    }
#else
    if (!m_library->Open(Common::DynamicLibrary::GetVersionedFilename("vulkan", 1).c_str()) &&
        !m_library->Open(Common::DynamicLibrary::GetVersionedFilename("vulkan").c_str()))
    {
        return Fail("Unable to load the Vulkan library");
    }
#endif
    Api & api = *m_api;
    if (!m_library->GetSymbol("vkGetInstanceProcAddr", &api.GetInstanceProcAddr))
    {
        return Fail("The Vulkan library has no vkGetInstanceProcAddr");
    }
    api.CreateInstance = reinterpret_cast<PFN_vkCreateInstance>(api.GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    api.EnumerateInstanceExtensionProperties = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        api.GetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
    if (api.CreateInstance == nullptr || api.EnumerateInstanceExtensionProperties == nullptr)
    {
        return Fail("The Vulkan library is incomplete");
    }

    uint32_t count = 0;
    api.EnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    api.EnumerateInstanceExtensionProperties(nullptr, &count, available.data());
    available.resize(count);

    std::vector<const char *> extensions = {VK_KHR_SURFACE_EXTENSION_NAME, kPlatformSurfaceExtension};
    for (const char * extension : extensions)
    {
        if (!HasExtension(available, extension))
        {
            return Fail(std::string("Vulkan lacks ") + extension);
        }
    }
    VkInstanceCreateFlags flags = 0;
    if (HasExtension(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
    {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "NXEmu";
    application.pEngineName = "NXEmu";
    application.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.flags = flags;
    info.pApplicationInfo = &application;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    if (api.CreateInstance(&info, nullptr, &m_context.instance) != VK_SUCCESS)
    {
        m_context.instance = VK_NULL_HANDLE;
        return Fail("Unable to create a Vulkan instance");
    }
    m_context.getInstanceProcAddr = api.GetInstanceProcAddr;

    bool complete = true;
#define NXEMU_VK_LOAD(name)                                                                              \
    api.name = reinterpret_cast<PFN_vk##name>(api.GetInstanceProcAddr(m_context.instance, "vk" #name)); \
    complete = complete && api.name != nullptr;
    NXEMU_PRESENTER_INSTANCE_FUNCTIONS(NXEMU_VK_LOAD)
#undef NXEMU_VK_LOAD
    return complete || Fail("The Vulkan instance is incomplete");
}

bool UiPresenter::CreateSurface()
{
    const VkInstance instance = m_context.instance;
    VkResult result = VK_ERROR_EXTENSION_NOT_PRESENT;
    switch (m_window->WindowSystem())
    {
    case RenderWindowSystem::Windows:
    {
#ifdef _WIN32
        const CreatePlatformSurface create = reinterpret_cast<CreatePlatformSurface>(m_api->GetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
        const Win32SurfaceCreateInfo info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, nullptr, 0, GetModuleHandleW(nullptr), m_window->RenderSurface()};
        if (create != nullptr && info.hwnd != nullptr)
        {
            result = create(instance, &info, nullptr, &m_surface);
        }
#endif
        break;
    }
    case RenderWindowSystem::Wayland:
    {
        const CreatePlatformSurface create = reinterpret_cast<CreatePlatformSurface>(m_api->GetInstanceProcAddr(instance, "vkCreateWaylandSurfaceKHR"));
        const WaylandSurfaceCreateInfo info{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, nullptr, 0, m_window->DisplayConnection(), m_window->RenderSurface()};
        if (create != nullptr && info.display != nullptr && info.surface != nullptr)
        {
            result = create(instance, &info, nullptr, &m_surface);
        }
        break;
    }
    case RenderWindowSystem::Cocoa:
    {
        const CreatePlatformSurface create = reinterpret_cast<CreatePlatformSurface>(m_api->GetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT"));
        const MetalSurfaceCreateInfo info{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT, nullptr, 0, m_window->RenderSurface()};
        if (create != nullptr && info.layer != nullptr)
        {
            result = create(instance, &info, nullptr, &m_surface);
        }
        break;
    }
    default:
        break;
    }
    if (result != VK_SUCCESS)
    {
        m_surface = VK_NULL_HANDLE;
        return Fail("Unable to create a Vulkan surface for the window");
    }
    return true;
}

bool UiPresenter::ChooseDevice(int32_t deviceIndex)
{
    const Api & api = *m_api;
    uint32_t count = 0;
    if (api.EnumeratePhysicalDevices(m_context.instance, &count, nullptr) != VK_SUCCESS || count == 0)
    {
        return Fail("No Vulkan device found");
    }
    std::vector<VkPhysicalDevice> devices(count);
    const VkResult listed = api.EnumeratePhysicalDevices(m_context.instance, &count, devices.data());
    if (listed != VK_SUCCESS && listed != VK_INCOMPLETE)
    {
        return Fail("No Vulkan device found");
    }
    devices.resize(count);
    if (deviceIndex > 0 && static_cast<uint32_t>(deviceIndex) < count)
    {
        std::rotate(devices.begin(), devices.begin() + deviceIndex, devices.begin() + deviceIndex + 1);
    }

    for (VkPhysicalDevice device : devices)
    {
        uint32_t extensionCount = 0;
        api.EnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        api.EnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());
        extensions.resize(extensionCount);
        if (!HasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        {
            continue;
        }
        uint32_t familyCount = 0;
        api.GetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        api.GetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
        for (uint32_t family = 0; family < familyCount; family++)
        {
            VkBool32 presents = VK_FALSE;
            if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
                api.GetPhysicalDeviceSurfaceSupportKHR(device, family, m_surface, &presents) == VK_SUCCESS && presents == VK_TRUE)
            {
                m_context.physicalDevice = device;
                m_queueFamily = family;
                return true;
            }
        }
    }
    return Fail("No Vulkan device can show the window");
}

bool UiPresenter::CreateDevice()
{
    Api & api = *m_api;
    uint32_t extensionCount = 0;
    api.EnumerateDeviceExtensionProperties(m_context.physicalDevice, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> available(extensionCount);
    api.EnumerateDeviceExtensionProperties(m_context.physicalDevice, nullptr, &extensionCount, available.data());
    available.resize(extensionCount);
    std::vector<const char *> extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (HasExtension(available, kPortabilitySubset))
    {
        extensions.push_back(kPortabilitySubset);
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = m_queueFamily;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    VkDevice device = VK_NULL_HANDLE;
    if (api.CreateDevice(m_context.physicalDevice, &info, nullptr, &device) != VK_SUCCESS)
    {
        return Fail("Unable to create the interface's Vulkan device");
    }

    bool complete = true;
#define NXEMU_VK_LOAD(name)                                                                \
    api.name = reinterpret_cast<PFN_vk##name>(api.GetDeviceProcAddr(device, "vk" #name)); \
    complete = complete && api.name != nullptr;
    NXEMU_PRESENTER_DEVICE_FUNCTIONS(NXEMU_VK_LOAD)
#undef NXEMU_VK_LOAD
    if (!complete)
    {
        if (api.DestroyDevice != nullptr)
        {
            api.DestroyDevice(device, nullptr);
        }
        return Fail("The interface's Vulkan device is incomplete");
    }
    m_context.device = device;
    api.GetDeviceQueue(device, m_queueFamily, 0, &m_queue);

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_queueFamily;
    VkCommandBufferAllocateInfo bufferInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    bufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    bufferInfo.commandBufferCount = 1;
    const VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    const VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (api.CreateCommandPool(device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS)
    {
        return Fail("Unable to create the interface's command pool");
    }
    bufferInfo.commandPool = m_commandPool;
    if (api.AllocateCommandBuffers(device, &bufferInfo, &m_commandBuffer) != VK_SUCCESS ||
        api.CreateFence(device, &fenceInfo, nullptr, &m_frameDone) != VK_SUCCESS ||
        api.CreateSemaphore(device, &semaphoreInfo, nullptr, &m_imageAcquired) != VK_SUCCESS)
    {
        return Fail("Unable to create the interface's frame objects");
    }
    return true;
}

void UiPresenter::ReleaseWindow()
{
    if (m_context.device == VK_NULL_HANDLE)
    {
        return;
    }
    WaitFrame();
    m_api->DeviceWaitIdle(m_context.device);
    DestroySwapchain();
    if (m_surface != VK_NULL_HANDLE)
    {
        m_api->DestroySurfaceKHR(m_context.instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }
}

void UiPresenter::WaitIdle()
{
    WaitFrame();
}

void UiPresenter::WaitFrame()
{
    if (m_framePending)
    {
        m_api->WaitForFences(m_context.device, 1, &m_frameDone, VK_TRUE, UINT64_MAX);
        m_api->ResetFences(m_context.device, 1, &m_frameDone);
        m_framePending = false;
    }
}

bool UiPresenter::CreateSwapchain(uint32_t width, uint32_t height)
{
    const Api & api = *m_api;
    VkSurfaceCapabilitiesKHR capabilities{};
    if (api.GetPhysicalDeviceSurfaceCapabilitiesKHR(m_context.physicalDevice, m_surface, &capabilities) != VK_SUCCESS)
    {
        return false;
    }
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX)
    {
        extent.width = std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0 || (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
    {
        return false;
    }

    uint32_t formatCount = 0;
    api.GetPhysicalDeviceSurfaceFormatsKHR(m_context.physicalDevice, m_surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    api.GetPhysicalDeviceSurfaceFormatsKHR(m_context.physicalDevice, m_surface, &formatCount, formats.data());
    formats.resize(formatCount);
    if (formats.empty())
    {
        return false;
    }
    VkSurfaceFormatKHR format = formats[0];
    if (format.format == VK_FORMAT_UNDEFINED)
    {
        format.format = VK_FORMAT_B8G8R8A8_UNORM;
    }
    for (const VkSurfaceFormatKHR & candidate : formats)
    {
        if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM || candidate.format == VK_FORMAT_R8G8B8A8_UNORM)
        {
            format = candidate;
            break;
        }
    }

    uint32_t imageCount = std::max(kSwapchainImages, capabilities.minImageCount);
    if (capabilities.maxImageCount != 0)
    {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((capabilities.supportedCompositeAlpha & alpha) == 0)
    {
        for (VkCompositeAlphaFlagBitsKHR candidate : {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
        {
            if ((capabilities.supportedCompositeAlpha & candidate) != 0)
            {
                alpha = candidate;
                break;
            }
        }
    }

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = m_surface;
    info.minImageCount = imageCount;
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = m_swapchain;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    const VkResult created = api.CreateSwapchainKHR(m_context.device, &info, nullptr, &swapchain);
    DestroySwapchain();
    if (created != VK_SUCCESS)
    {
        return false;
    }
    m_swapchain = swapchain;
    m_swapchainFormat = format.format;
    m_swapchainExtent = extent;
    m_swapchainStale = false;

    VkFormatProperties properties{};
    api.GetPhysicalDeviceFormatProperties(m_context.physicalDevice, format.format, &properties);
    m_swapchainBlit = (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;

    uint32_t count = 0;
    api.GetSwapchainImagesKHR(m_context.device, m_swapchain, &count, nullptr);
    m_swapchainImages.resize(count);
    api.GetSwapchainImagesKHR(m_context.device, m_swapchain, &count, m_swapchainImages.data());
    m_swapchainImages.resize(count);
    const VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    m_renderDone.assign(count, VK_NULL_HANDLE);
    for (VkSemaphore & semaphore : m_renderDone)
    {
        if (api.CreateSemaphore(m_context.device, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS)
        {
            DestroySwapchain();
            return false;
        }
    }
    return true;
}

void UiPresenter::DestroySwapchain()
{
    if (m_context.device == VK_NULL_HANDLE)
    {
        return;
    }
    if (m_swapchain != VK_NULL_HANDLE)
    {
        m_api->DeviceWaitIdle(m_context.device);
    }
    for (VkSemaphore semaphore : m_renderDone)
    {
        m_api->DestroySemaphore(m_context.device, semaphore, nullptr);
    }
    m_renderDone.clear();
    m_swapchainImages.clear();
    if (m_swapchain != VK_NULL_HANDLE)
    {
        m_api->DestroySwapchainKHR(m_context.device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
}

bool UiPresenter::CreateTarget(uint32_t width, uint32_t height)
{
    DestroyTarget();
    const Api & api = *m_api;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = kTargetFormat;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (api.CreateImage(m_context.device, &info, nullptr, &m_target) != VK_SUCCESS)
    {
        m_target = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements requirements{};
    api.GetImageMemoryRequirements(m_context.device, m_target, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    api.GetPhysicalDeviceMemoryProperties(m_context.physicalDevice, &memory);
    uint32_t type = UINT32_MAX;
    for (uint32_t index = 0; index < memory.memoryTypeCount && type == UINT32_MAX; index++)
    {
        if ((requirements.memoryTypeBits & (1u << index)) != 0 && (memory.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0)
        {
            type = index;
        }
    }
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    if (type == UINT32_MAX || api.AllocateMemory(m_context.device, &allocation, nullptr, &m_targetMemory) != VK_SUCCESS ||
        api.BindImageMemory(m_context.device, m_target, m_targetMemory, 0) != VK_SUCCESS)
    {
        DestroyTarget();
        return false;
    }
    m_targetExtent = {width, height};
    return true;
}

void UiPresenter::DestroyTarget()
{
    if (m_target != VK_NULL_HANDLE)
    {
        m_api->DestroyImage(m_context.device, m_target, nullptr);
        m_target = VK_NULL_HANDLE;
    }
    if (m_targetMemory != VK_NULL_HANDLE)
    {
        m_api->FreeMemory(m_context.device, m_targetMemory, nullptr);
        m_targetMemory = VK_NULL_HANDLE;
    }
    m_targetExtent = {};
}

VkCommandBuffer UiPresenter::BeginFrame(uint32_t width, uint32_t height, VkImage & image)
{
    if (m_context.device == VK_NULL_HANDLE || width == 0 || height == 0)
    {
        return VK_NULL_HANDLE;
    }
    const Api & api = *m_api;
    WaitFrame();
    if (m_surface == VK_NULL_HANDLE && !CreateSurface())
    {
        return VK_NULL_HANDLE;
    }
    if ((m_targetExtent.width != width || m_targetExtent.height != height) && !CreateTarget(width, height))
    {
        return VK_NULL_HANDLE;
    }

    bool acquired = false;
    for (int attempt = 0; attempt < 2 && !acquired; attempt++)
    {
        if ((m_swapchain == VK_NULL_HANDLE || m_swapchainStale) && !CreateSwapchain(width, height))
        {
            return VK_NULL_HANDLE;
        }
        const VkResult result = api.AcquireNextImageKHR(m_context.device, m_swapchain, UINT64_MAX, m_imageAcquired, VK_NULL_HANDLE, &m_imageIndex);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
        {
            acquired = true;
            m_swapchainStale = result == VK_SUBOPTIMAL_KHR;
        }
        else if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            m_swapchainStale = true;
        }
        else
        {
            if (result == VK_ERROR_SURFACE_LOST_KHR)
            {
                ReleaseWindow();
            }
            return VK_NULL_HANDLE;
        }
    }
    if (!acquired)
    {
        return VK_NULL_HANDLE;
    }

    api.ResetCommandBuffer(m_commandBuffer, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    api.BeginCommandBuffer(m_commandBuffer, &begin);
    Barrier(api.CmdPipelineBarrier, m_commandBuffer, m_target, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    image = m_target;
    return m_commandBuffer;
}

void UiPresenter::EndFrame()
{
    const Api & api = *m_api;
    const VkImage output = m_swapchainImages[m_imageIndex];
    Barrier(api.CmdPipelineBarrier, m_commandBuffer, m_target, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Barrier(api.CmdPipelineBarrier, m_commandBuffer, output, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageSubresourceLayers color{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    const bool sameShape = m_swapchainFormat == kTargetFormat && m_swapchainExtent.width == m_targetExtent.width && m_swapchainExtent.height == m_targetExtent.height;
    if (m_swapchainBlit && !sameShape)
    {
        VkImageBlit region{};
        region.srcSubresource = color;
        region.srcOffsets[1] = {static_cast<int32_t>(m_targetExtent.width), static_cast<int32_t>(m_targetExtent.height), 1};
        region.dstSubresource = color;
        region.dstOffsets[1] = {static_cast<int32_t>(m_swapchainExtent.width), static_cast<int32_t>(m_swapchainExtent.height), 1};
        api.CmdBlitImage(m_commandBuffer, m_target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
    }
    else
    {
        VkImageCopy region{};
        region.srcSubresource = color;
        region.dstSubresource = color;
        region.extent = {std::min(m_targetExtent.width, m_swapchainExtent.width), std::min(m_targetExtent.height, m_swapchainExtent.height), 1};
        api.CmdCopyImage(m_commandBuffer, m_target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    Barrier(api.CmdPipelineBarrier, m_commandBuffer, output, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    api.EndCommandBuffer(m_commandBuffer);

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &m_imageAcquired;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &m_commandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &m_renderDone[m_imageIndex];
    if (api.QueueSubmit(m_queue, 1, &submit, m_frameDone) != VK_SUCCESS)
    {
        m_swapchainStale = true;
        return;
    }
    m_framePending = true;

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &m_renderDone[m_imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &m_swapchain;
    present.pImageIndices = &m_imageIndex;
    const VkResult result = api.QueuePresentKHR(m_queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        m_swapchainStale = true;
    }
    else if (result == VK_ERROR_SURFACE_LOST_KHR)
    {
        ReleaseWindow();
    }
}
