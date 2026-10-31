#pragma once
#include "vulkan/vulkan_api.h"
#include <cstdint>
#include <memory>
#include <nxemu-module-spec/base.h>
#include <string>
#include <vector>

namespace Common
{
class DynamicLibrary;
}

class UiPresenter
{
public:
    UiPresenter();
    ~UiPresenter();

    bool Create(IRenderWindow & window, int32_t deviceIndex);
    bool Create(IRenderWindow & window, UiPresenter & shared);
    const VulkanContext & Context() const { return m_context; }
    const std::string & Error() const { return m_error; }

    void ReleaseWindow();
    void WaitIdle();

    VkCommandBuffer BeginFrame(uint32_t width, uint32_t height, VkImage & image);
    void EndFrame();

private:
    UiPresenter(const UiPresenter &) = delete;
    UiPresenter & operator=(const UiPresenter &) = delete;

    struct Api;

    bool Fail(const std::string & message);
    bool CreateInstance();
    bool CreateSurface();
    bool ChooseDevice(int32_t deviceIndex);
    bool CreateDevice();
    bool CreateFrameObjects();
    bool CreateSwapchain(uint32_t width, uint32_t height);
    void DestroySwapchain();
    bool CreateTarget(uint32_t width, uint32_t height);
    void DestroyTarget();
    void WaitFrame();

    std::unique_ptr<Common::DynamicLibrary> m_library;
    std::unique_ptr<Api> m_api;
    IRenderWindow * m_window = nullptr;
    bool m_ownsDevice = false;
    VulkanContext m_context;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VkQueue m_queue = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    VkFence m_frameDone = VK_NULL_HANDLE;
    VkSemaphore m_imageAcquired = VK_NULL_HANDLE;
    bool m_framePending = false;

    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D m_swapchainExtent{};
    bool m_swapchainBlit = false;
    bool m_swapchainStale = false;
    std::vector<VkImage> m_swapchainImages;
    std::vector<VkSemaphore> m_renderDone;
    uint32_t m_imageIndex = 0;

    VkImage m_target = VK_NULL_HANDLE;
    VkDeviceMemory m_targetMemory = VK_NULL_HANDLE;
    VkExtent2D m_targetExtent{};

    std::string m_error;
};
