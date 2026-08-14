#pragma once
#include <nxemu-module-spec/cpu.h>
#include <nxemu-module-spec/video.h>
#include <memory>

namespace Tegra {
class MemoryManager;
}

class VideoManager :
    public IVideo
{
public:
    VideoManager(IRenderWindow & window, ISystemModules & modules);
    ~VideoManager();

    void EmulationStarting();

    // IVideo
    bool Initialize() override;
    void Shutdown() override;
    uint32_t AllocAsEx(uint64_t addressSpaceBits, uint64_t splitAddress, uint64_t bigPageBits, uint64_t pageBits) override;
    uint64_t MapBufferEx(uint32_t gmmu, uint64_t gpuAddr, uint64_t deviceAddr, uint64_t size, uint16_t kind, bool isBigPages) override;
    uint64_t MapSparse(uint32_t gmmu, uint64_t gpuAddr, uint64_t size, bool isBigPages) override;
    void Unmap(uint32_t gmmu, uint64_t gpuAddr, uint64_t size) override;
    uint64_t Host1xMemoryAllocate(uint64_t size) override;
    uint32_t Host1xAllocatorAllocate(uint32_t size) override;
    void Host1xAllocatorFree(uint32_t address, uint32_t size) override;
    void Host1xGMMUMap(uint64_t address, uint64_t virtual_address, uint64_t size) override;
    void Host1xGMMUUnmap(uint64_t address, uint64_t size) override;
    void Host1xMemoryMap(uint64_t address, uint64_t virtualAddress, uint64_t size, uint64_t asid, bool track) override;
    void Host1xMemoryUnmap(uint64_t address, uint64_t size) override;
    void Host1xFree(uint64_t regionStart, uint64_t regionSize) override;
    void Host1xMemoryTrackContinuity(uint64_t address, uint64_t virtualAddress, uint64_t size, uint64_t asid) override;
    uint64_t Host1xRegisterProcess(IMemory* memory) override;
    void Host1xUnregisterProcess(uint64_t asid) override;
    void RequestComposite(VideoFramebufferConfig * layers, uint32_t layerCount, VideoNvFence * fences, uint32_t fenceCount) override;
    void UpdateFramebufferLayout(uint32_t width, uint32_t height) override;
    IChannelState * AllocateChannel() override;
    void PushGPUEntries(int32_t bindId, const uint64_t * commandList, uint32_t commandListSize, const uint32_t * prefetchCommandlist, uint32_t prefetchCommandlistSize) override;
    void PushCommandBuffer(int32_t bindId, const uint32_t * commandList, uint32_t commandListSize) override;
    RasterizerDownloadArea HandleRasterizerDownload(const uint8_t * pointer, uint64_t size, RasterizerDownloadArea current_area) override;
    void HandleRasterizerWrite(const uint8_t * pointer, uint64_t size, uint64_t * last_page, RasterizerDirtyCollect collect, void * user_data) override;
    void DeregisterHostAction(uint32_t syncpoint_id, uint32_t handle) override;
    uint32_t HostSyncpointValue(uint32_t id) override;
    uint32_t HostSyncpointRegisterAction(uint32_t fence_id, uint32_t target_value, HostActionCallback operation, uint32_t slot, void * userData) override;
    void WaitHost(uint32_t syncpoint_id, uint32_t expected_value) override;
    uint32_t ShadersBuilding() override;
    bool UseNvdec() override;
    void ClearCdmaInstance(uint32_t id) override;
    uint32_t GetAppletCaptureBuffer(uint8_t * out, uint32_t out_size) override;
    void InvalidateMemory(const uint8_t * pointer, uint64_t size) override;

private:
    VideoManager() = delete;
    VideoManager(const VideoManager&) = delete;
    VideoManager& operator=(const VideoManager&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl;
};
