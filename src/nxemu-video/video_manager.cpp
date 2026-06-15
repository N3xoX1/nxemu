#include "render_window.h"
#include "video_manager.h"
#include "video_settings.h"
#include "yuzu_video_core/control/channel_state.h"
#include "yuzu_video_core/dma_pusher.h"
#include "yuzu_video_core/host1x/host1x.h"
#include "yuzu_video_core/video_core.h"
#include "yuzu_video_core/gpu.h"
#include "yuzu_video_core/renderer_base.h"
#include "yuzu_video_core/rasterizer_interface.h"
#include <nxemu-core/settings/identifiers.h>
#include "yuzu_video_core/shader_notify.h"
#include <stop_token>
#include "yuzu_common/settings.h"
#include "nxemu-video/video_settings.h"
#include <chrono>
#include <future>
#include <thread>

extern IModuleSettings * g_settings;

struct VideoManager::Impl 
{
    Impl(IRenderWindow & window, ISystemModules & modules) :
        m_window(window),
        m_modules(modules)
    {
    }

    bool Initialize()
    {
        m_host1x = std::make_unique<Tegra::Host1x::Host1x>(m_modules.OperatingSystem().DeviceMemory());
        m_emuWindow = std::make_unique<RenderWindow>(m_window);
        return true;
    }
    
    void EmulationStarting(void)
    {
        if (m_init.valid())
        {
            m_init.wait();
        }
        if (m_gpuCore)
        {
            m_gpuCore->NotifyShutdown();
            m_gpuCore = nullptr;
        }
        g_settings->SetBool(NXCoreSetting::DisplayedFrames, false);

        Layout::FramebufferLayout layout;
        if (m_emuWindow)
        {
            layout = m_emuWindow->GetFramebufferLayout();
        }
        m_emuWindow = nullptr;
        m_emuWindow = std::make_unique<RenderWindow>(m_window);
        m_emuWindow->UpdateCurrentFramebufferLayout(layout.width, layout.height);
        m_gpuCore = VideoCore::CreateGPU(m_modules, *(m_emuWindow.get()), *m_host1x);
        if (!m_gpuCore)
        {
            return;
        }
        m_init = std::async(std::launch::async, [this]() {
            const auto notify_disk_cache_progress = [](VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total) {
                if (g_settings == nullptr)
                {
                    return;
                }
                int stage_int = -1;
                switch (stage)
                {
                case VideoCore::LoadCallbackStage::Prepare:
                    stage_int = 0;
                    break;
                case VideoCore::LoadCallbackStage::Build:
                    stage_int = 1;
                    break;
                case VideoCore::LoadCallbackStage::Complete:
                    stage_int = 2;
                    break;
                }
                g_settings->SetInt(NXCoreSetting::DiskCacheLoadStage, stage_int);
                g_settings->SetInt(NXCoreSetting::DiskCacheLoadCurrent, static_cast<int32_t>(value));
                g_settings->SetInt(NXCoreSetting::DiskCacheLoadTotal, static_cast<int32_t>(total));
                g_settings->SetInt(NXCoreSetting::DiskCacheLoadTick, g_settings->GetInt(NXCoreSetting::DiskCacheLoadTick) + 1);
            };

            notify_disk_cache_progress(VideoCore::LoadCallbackStage::Prepare, 0, 0);
            if (videoSettings.use_disk_shader_cache.GetValue())
            {
                const u64 program_id = m_modules.OperatingSystem().GetProgramId();
                const std::stop_token stop_token;
                m_gpuCore->ObtainContext();
                m_gpuCore->Renderer().ReadRasterizer()->LoadDiskResources(program_id, stop_token,
                                                                          notify_disk_cache_progress);
                m_gpuCore->ReleaseContext();
            }
            else
            {
                m_gpuCore->ObtainContext();
            }
            notify_disk_cache_progress(VideoCore::LoadCallbackStage::Complete, 0, 0);
            if (!videoSettings.use_disk_shader_cache.GetValue())
            {
                m_gpuCore->ReleaseContext();
            }

            m_gpuCore->Start();
        });
    }
    std::unique_ptr<Tegra::Host1x::Host1x> m_host1x;
    std::unique_ptr<RenderWindow> m_emuWindow;
    std::unique_ptr<Tegra::GPU> m_gpuCore;
    IRenderWindow & m_window;
    ISystemModules & m_modules;
    Tegra::MemoryManagerRegistry m_memoryManagerRegistry;
    std::future<void> m_init;
};

VideoManager::VideoManager(IRenderWindow & window, ISystemModules & modules) :
    impl{ std::make_unique<Impl>(window, modules) }
{
}

VideoManager::~VideoManager()
{
    impl->m_host1x.release();
    if (impl->m_gpuCore)
    {
        impl->m_gpuCore.reset();
    }
    impl->m_emuWindow.release();
}

void VideoManager::EmulationStarting()
{
    impl->EmulationStarting();
}

bool VideoManager::Initialize()
{
    SetupVideoSetting();
    return impl->Initialize();
}

void VideoManager::Shutdown()
{
    impl->m_host1x.reset();
    if (impl->m_gpuCore != nullptr)
    {
        impl->m_gpuCore->NotifyShutdown();
    }
};

uint32_t VideoManager::AllocAsEx(uint64_t addressSpaceBits, uint64_t splitAddress, uint64_t bigPageBits, uint64_t pageBits)
{
    std::shared_ptr<Tegra::MemoryManager> memory = std::make_shared<Tegra::MemoryManager>(impl->m_host1x->MemoryManager(), addressSpaceBits, splitAddress, bigPageBits, pageBits);
    impl->m_gpuCore->InitAddressSpace(*memory);

    return impl->m_memoryManagerRegistry.AddMemoryManager(memory);
}

uint64_t VideoManager::MapBufferEx(uint32_t gmmu, uint64_t gpuAddr, uint64_t deviceAddr, uint64_t size, uint16_t kind, bool isBigPages)
{
    std::shared_ptr<Tegra::MemoryManager> memory = impl->m_memoryManagerRegistry.GetMemoryManager(gmmu);
    if (memory == nullptr)
    {
        UNIMPLEMENTED();
        return 0;
    }
    return memory->Map(gpuAddr, deviceAddr, size, (Tegra::PTEKind)kind, isBigPages);
}

uint64_t VideoManager::MapSparse(uint32_t gmmu, uint64_t gpuAddr, uint64_t size, bool isBigPages)
{
    std::shared_ptr<Tegra::MemoryManager> memory = impl->m_memoryManagerRegistry.GetMemoryManager(gmmu);
    if (memory == nullptr)
    {
        UNIMPLEMENTED();
        return 0;
    }
    return memory->MapSparse(gpuAddr, size, isBigPages);
}

void VideoManager::Unmap(uint32_t gmmu, uint64_t gpuAddr, uint64_t size)
{
    std::shared_ptr<Tegra::MemoryManager> memory = impl->m_memoryManagerRegistry.GetMemoryManager(gmmu);
    if (memory == nullptr)
    {
        UNIMPLEMENTED();
        return;
    }
    memory->Unmap(gpuAddr, size);
}

uint64_t VideoManager::Host1xMemoryAllocate(uint64_t size)
{
    return impl->m_host1x->MemoryManager().Allocate(size);
}

uint32_t VideoManager::Host1xAllocatorAllocate(uint32_t size)
{
    Common::FlatAllocator<u32, 0, 32> & gmmu_allocator = impl->m_host1x->Allocator();
    return gmmu_allocator.Allocate(static_cast<u32>(size));
}

void VideoManager::Host1xAllocatorFree(uint32_t address, uint32_t size)
{
    impl->m_host1x->Allocator().Free(address, size);
}

void VideoManager::Host1xGMMUMap(uint64_t address, uint64_t virtual_address, uint64_t size)
{
    auto & gmmu = impl->m_host1x->GMMU();
    gmmu.Map(static_cast<GPUVAddr>(address), virtual_address, size);
}

void VideoManager::Host1xGMMUUnmap(uint64_t address, uint64_t size)
{
    auto & gmmu = impl->m_host1x->GMMU();
    gmmu.Unmap(static_cast<GPUVAddr>(address), size);
}

void VideoManager::Host1xMemoryMap(uint64_t address, uint64_t virtualAddress, uint64_t size, uint64_t asid, bool track)
{
    impl->m_host1x->MemoryManager().Map(address, virtualAddress, size, Core::Asid{asid}, track);
}

void VideoManager::Host1xMemoryUnmap(uint64_t address, uint64_t size)
{
    impl->m_host1x->MemoryManager().Unmap(address, size);
}

void VideoManager::Host1xFree(uint64_t regionStart, uint64_t regionSize)
{
    impl->m_host1x->MemoryManager().Free(regionStart, regionSize);
}

void VideoManager::Host1xMemoryTrackContinuity(uint64_t address, uint64_t virtualAddress, uint64_t size, uint64_t asid)
{
    impl->m_host1x->MemoryManager().TrackContinuity(address, virtualAddress, size, Core::Asid{asid});
}

uint64_t VideoManager::Host1xRegisterProcess(IMemory * memory)
{
    Core::Asid asid = impl->m_host1x->MemoryManager().RegisterProcess(memory);
    return asid.id;
}

void VideoManager::Host1xUnregisterProcess(uint64_t asid)
{
    Tegra::MaxwellDeviceMemoryManager & smmu = impl->m_host1x->MemoryManager();
    smmu.UnregisterProcess(Core::Asid{asid});
}

void VideoManager::RequestComposite(VideoFramebufferConfig * layers, uint32_t layerCount, VideoNvFence * fences, uint32_t fenceCount)
{
    std::vector<Tegra::FramebufferConfig> output_layers;
    std::vector<Service::Nvidia::NvFence> output_fences;
    output_layers.reserve(layerCount);
    output_fences.reserve(fenceCount);

    for (uint32_t i = 0; i < layerCount; i++)
    {
        output_layers.emplace_back(Tegra::FramebufferConfig{
            .address = layers[i].address,
            .offset = layers[i].offset,
            .width = layers[i].width,
            .height = layers[i].height,
            .stride = layers[i].stride,
            .pixel_format = (Service::android::PixelFormat)layers[i].pixelFormat,
            .transform_flags = (Service::android::BufferTransformFlags)layers[i].transformFlags,
            .crop_rect = Common::Rectangle<int>{layers[i].cropLeft, layers[i].cropTop, layers[i].cropRight, layers[i].cropBottom },
            .blending = (Tegra::BlendMode)layers[i].blending,
        });
    }
    for (uint32_t i = 0; i < fenceCount; i++)
    {
        output_fences.emplace_back(Service::Nvidia::NvFence{
            .id = fences[i].id,
            .value = fences[i].value,
        });
    }
    impl->m_gpuCore->RequestComposite(std::move(output_layers), std::move(output_fences));
}

void VideoManager::UpdateFramebufferLayout(uint32_t width, uint32_t height)
{
    impl->m_emuWindow->UpdateCurrentFramebufferLayout(width, height);
}

IChannelState * VideoManager::AllocateChannel()
{
    return std::make_unique<IChannelStatePtr>(*impl->m_gpuCore, impl->m_memoryManagerRegistry, std::move(impl->m_gpuCore->AllocateChannel())).release();
}

void VideoManager::PushGPUEntries(int32_t bindId, const uint64_t * commandList, uint32_t commandListSize, const uint32_t * prefetchCommandlist, uint32_t prefetchCommandlistSize)
{
    Tegra::CommandList entries(commandListSize);
    memcpy(entries.command_lists.data(), commandList, sizeof(uint64_t) * commandListSize);
    if (prefetchCommandlistSize > 0)
    {
        entries.prefetch_command_list.resize(prefetchCommandlistSize);
        memcpy(entries.prefetch_command_list.data(), prefetchCommandlist, sizeof(uint32_t) * prefetchCommandlistSize);
    }
    impl->m_gpuCore->PushGPUEntries(bindId, std::move(entries));
}

void VideoManager::PushCommandBuffer(int32_t bindId, const uint32_t * commandList, uint32_t commandListSize)
{
    Tegra::ChCommandHeaderList cmdlist(commandListSize);
    memcpy(cmdlist.data(), commandList, sizeof(Tegra::ChCommandHeader) * commandListSize);
    impl->m_gpuCore->PushCommandBuffer(bindId, cmdlist);
}

void VideoManager::ApplyOpOnDeviceMemoryPointer(const uint8_t * pointer, uint32_t * scratchBuffer, size_t scratchBufferSize, DeviceMemoryOperation operation, void * userData)
{
    Common::ScratchBuffer<u32> tempBuffer;
    impl->m_host1x->MemoryManager().ApplyOpOnPointer(pointer, tempBuffer, [operation, userData](DAddr address) {
        operation(address, userData);
    });
}

RasterizerDownloadArea VideoManager::OnCPURead(uint64_t addr, uint64_t size)
{
    return impl->m_gpuCore->OnCPURead(addr, size);
}

bool VideoManager::OnCPUWrite(uint64_t addr, uint64_t size)
{
    return impl->m_gpuCore->OnCPUWrite(addr, size);
}

void VideoManager::DeregisterHostAction(uint32_t syncpoint_id, uint32_t handle)
{
    impl->m_host1x->GetSyncpointManager().DeregisterHostAction(syncpoint_id, handle);
}

uint32_t VideoManager::HostSyncpointValue(uint32_t id)
{
    return impl->m_host1x->GetSyncpointManager().GetHostSyncpointValue(id);
}

uint32_t VideoManager::HostSyncpointRegisterAction(uint32_t fence_id, uint32_t target_value, HostActionCallback operation, uint32_t slot, void* userData)
{
    return impl->m_host1x->GetSyncpointManager().RegisterHostAction(fence_id, target_value, [operation, userData, slot]() {
        operation(slot, userData);
    });
}

void VideoManager::WaitHost(uint32_t syncpoint_id, uint32_t expected_value)
{
    impl->m_host1x->GetSyncpointManager().WaitHost(syncpoint_id, expected_value);
}

uint32_t VideoManager::ShadersBuilding()
{
    auto & shader_notify = impl->m_gpuCore->ShaderNotify();
    return shader_notify.ShadersBuilding();
}

bool VideoManager::UseNvdec()
{
    return impl->m_gpuCore->UseNvdec();
}

void VideoManager::ClearCdmaInstance(uint32_t id)
{
    impl->m_gpuCore->ClearCdmaInstance(id);
}