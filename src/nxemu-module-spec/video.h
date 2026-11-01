#pragma once
#include "base.h"
#include <stdint.h>
#include <stop_token>

enum class AnisotropyMode : uint32_t
{
    Automatic = 0,
    Default,
    X2,
    X4,
    X8,
    X16,
};

enum class AstcDecodeMode : uint32_t
{
    Cpu = 0,
    Gpu,
    CpuAsynchronous,
};

enum class AstcRecompression : uint32_t
{
    Uncompressed = 0,
    Bc1,
    Bc3,
};

enum class VSyncMode : uint32_t
{
    Immediate = 0,
    Mailbox,
    Fifo,
    FifoRelaxed,
};

enum class VramUsageMode : uint32_t
{
    Conservative = 0,
    Aggressive,
};

enum class RendererBackend : uint32_t
{
    OpenGL = 0,
    Vulkan,
    Null,
};

enum class ShaderBackend : uint32_t
{
    Glsl = 0,
    Glasm,
    SpirV,
};

enum class GpuAccuracy : uint32_t
{
    Normal = 0,
    High,
    Extreme,
};

enum class FullscreenMode : uint32_t
{
    Borderless = 0,
    Exclusive,
};

enum class NvdecEmulation : uint32_t
{
    Off = 0,
    Cpu,
    Gpu,
};

enum class ResolutionSetup : uint32_t
{
    Res1_2X = 0,
    Res3_4X,
    Res1X,
    Res3_2X,
    Res2X,
    Res3X,
    Res4X,
    Res5X,
    Res6X,
    Res7X,
    Res8X,
};

enum class ScalingFilter : uint32_t
{
    NearestNeighbor = 0,
    Bilinear,
    Bicubic,
    Gaussian,
    ScaleForce,
    Fsr,
    MaxEnum,
};

enum class AntiAliasing : uint32_t
{
    None = 0,
    Fxaa,
    Smaa,
    MaxEnum,
};

enum class AspectRatio : uint32_t
{
    R16_9 = 0,
    R4_3,
    R21_9,
    R16_10,
    Stretch,
};

nxinterface IMemory;

struct VideoFramebufferConfig 
{
    uint64_t address;
    uint32_t offset;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixelFormat;
    uint32_t transformFlags;
    int32_t cropLeft;
    int32_t cropTop;
    int32_t cropRight;
    int32_t cropBottom;
    uint32_t blending;
};

struct VideoNvFence
{
    int32_t id;
    uint32_t value;
};

struct RasterizerDownloadArea 
{
    uint64_t startAddress;
    uint64_t endAddress;
    bool preemtive;
    bool native_read = false;
    // The CPU accessed another guest page of the same host page.
    bool shared_page = false;
    // Result: the shared page awaits an asynchronous copy and stays inaccessible.
    bool deferred = false;
};

nxinterface IChannelState
{
    virtual bool Initialized() const = 0;
    virtual int32_t BindId() const = 0;
    virtual void Init(uint64_t programId) = 0;
    virtual void SetMemoryManager(uint32_t id) = 0;
    virtual void Release() = 0;
};

struct VideoInterfaceDevice
{
    void * getInstanceProcAddr; // PFN_vkGetInstanceProcAddr
    void * instance;            // VkInstance
    void * physicalDevice;      // VkPhysicalDevice
    void * device;              // VkDevice
};

struct VideoInterfaceFrame
{
    void * commandBuffer; // VkCommandBuffer to record the frame into
    uint64_t image;       // VkImage: R8G8B8A8_UNORM, in TRANSFER_DST_OPTIMAL.
                          // Write premultiplied alpha, leave in SHADER_READ_ONLY_OPTIMAL.
};

struct VideoInterfaceArea
{
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
};

typedef void (*HostActionCallback)(uint32_t slot, void * userData);
typedef void (*RasterizerDirtyCollect)(uint64_t device_address, uint64_t size, void * user_data);

struct ShaderProfileTask
{
    bool importing{}, compact{true};
    uint64_t title_id{};
    const char *cache{}, *recording{}, *game_root{}, *base{}, *update{}, *profile{}, *index{}, *label{};
    // Export only: the installed profile kept first (optional), and the author's
    // revision and note.
    const char *previous{}, *revision{}, *note{};
    // Export only: the author recorded the whole game.
    bool full_game{};
    // Import inspection only (optional): a recorded game version to compare with the profile.
    const char *context{};
};
enum class ShaderProfilePrevious : uint32_t
{
    None,
    Merged,
    Unreadable,
};
struct ShaderProfileResult
{
    uint64_t programs{}, environments{}, graphics{}, compute{}, unresolved{}, missing{}, reused{}, bytes{};
    double seconds{};
    // Recipes kept from the installed profile, and game versions the profile covers.
    uint64_t inherited{}, versions{};
    // Recipes and game versions dropped, oldest first, to stay within the limits.
    uint64_t dropped_recipes{}, dropped_versions{};
    ShaderProfilePrevious previous{};
    // The profile's own description, NUL-terminated UTF-8.
    char label[4097]{}, revision[33]{}, note[1025]{};
    // Import inspection: the author recorded the whole game, and the game
    // versions the recipes were recorded with, one per line ("1.0.0 + update 4.0.1").
    bool full_game{};
    char targets[1025]{};
    // Import inspection with a context: 2 when the profile was exported from that
    // version, 1 when it also covers it, 0 otherwise. Startup prefers them in this order.
    uint32_t context_match{};
};
nxinterface IShaderProfileProgress
{
    virtual bool ProfileProgress(const char * phase, uint64_t completed, uint64_t total) = 0;
};

nxinterface IVideo
{
    virtual bool Initialize() = 0;
    virtual void Shutdown() = 0;
    virtual uint32_t AllocAsEx(uint64_t addressSpaceBits, uint64_t splitAddress, uint64_t bigPageBits, uint64_t pageBits) = 0;
    virtual uint64_t MapBufferEx(uint32_t gmmu, uint64_t gpuAddr, uint64_t deviceAddr, uint64_t size, uint16_t kind, bool isBigPages) = 0;
    virtual uint64_t MapSparse(uint32_t gmmu, uint64_t gpuAddr, uint64_t size, bool isBigPages) = 0;
    virtual void Unmap(uint32_t gmmu, uint64_t gpuAddr, uint64_t size) = 0;
    virtual uint64_t Host1xMemoryAllocate(uint64_t size) = 0;
    virtual uint32_t Host1xAllocatorAllocate(uint32_t size) = 0;
    virtual void Host1xAllocatorFree(uint32_t address, uint32_t size) = 0;
    virtual void Host1xGMMUMap(uint64_t address, uint64_t virtual_address, uint64_t size) = 0;
    virtual void Host1xGMMUUnmap(uint64_t address, uint64_t size) = 0;
    virtual void Host1xMemoryMap(uint64_t address, uint64_t virtualAddress, uint64_t size, uint64_t asid, bool track) = 0;
    virtual void Host1xMemoryUnmap(uint64_t address, uint64_t size) = 0;
    virtual void Host1xFree(uint64_t regionStart, uint64_t regionSize) = 0;
    virtual void Host1xMemoryTrackContinuity(uint64_t address, uint64_t virtualAddress, uint64_t size, uint64_t asid) = 0;
    virtual void RequestComposite(VideoFramebufferConfig * layers, uint32_t layerCount, VideoNvFence * fences, uint32_t fenceCount) = 0;
    virtual uint64_t Host1xRegisterProcess(IMemory * memory) = 0;
    virtual void UpdateFramebufferLayout(uint32_t width, uint32_t height) = 0;
    virtual IChannelState * AllocateChannel() = 0;
    virtual void PushGPUEntries(int32_t bindId, const uint64_t * commandList, uint32_t commandListSize, const uint32_t * prefetchCommandlist, uint32_t prefetchCommandlistSize) = 0;
    virtual void PushCommandBuffer(int32_t bindId, const uint32_t * commandList, uint32_t commandListSize) = 0;
    virtual RasterizerDownloadArea HandleRasterizerDownload(const uint8_t * pointer, uint64_t size, RasterizerDownloadArea current_area) = 0;
    virtual void HandleRasterizerWrite(const uint8_t * pointer, uint64_t size, uint64_t * last_page, RasterizerDirtyCollect collect, void * user_data) = 0;
    virtual void Host1xUnregisterProcess(uint64_t asid) = 0;
    virtual void DeregisterHostAction(uint32_t syncpoint_id, uint32_t handle) = 0;
    virtual uint32_t HostSyncpointValue(uint32_t id) = 0;
    virtual void HostSyncpointIncrement(uint32_t id) = 0;
    virtual uint32_t HostSyncpointRegisterAction(uint32_t fence_id, uint32_t target_value, HostActionCallback operation, uint32_t slot, void * userData) = 0;
    virtual bool WaitHost(uint32_t syncpoint_id, uint32_t expected_value, std::stop_token stop) = 0;
    virtual uint32_t ShadersBuilding() = 0;
    virtual bool UseNvdec() = 0;
    virtual void ClearCdmaInstance(uint32_t id) = 0;
    virtual uint32_t GetAppletCaptureBuffer(uint8_t * out, uint32_t out_size) = 0;
    virtual void InvalidateMemory(const uint8_t * pointer, uint64_t size) = 0;
    virtual void NotifyWindowChanged() = 0;
    virtual uint32_t GetLinearAppletCaptureBuffer(uint8_t * out, uint32_t out_size, uint32_t * out_width, uint32_t * out_height) = 0;
    virtual void WaitForComposite() = 0;
    virtual void CancelPendingComposite() = 0;
    virtual bool AttachInterface(VideoInterfaceDevice * device) = 0;
    virtual bool BeginInterfaceFrame(uint32_t width, uint32_t height, VideoInterfaceFrame * frame) = 0;
    virtual void EndInterfaceFrame(const VideoInterfaceArea * gameArea) = 0;
    virtual void WaitInterfaceIdle() = 0;
    // Owns resources on every path; release exactly once after completion or cancellation,
    // before ClearCdmaInstance returns. The callback must not throw.
    virtual void PushCommandBufferWithResources(int32_t bindId, const uint32_t * commandList,
                                                uint32_t commandListSize, void * resources, void (*release)(void *)) = 0;
    // Synchronous offline task. The caller keeps this module alive until return.
    virtual bool ProcessShaderProfile(const ShaderProfileTask & task, IShaderProfileProgress & progress,
                                      ShaderProfileResult & result, char * error, uint32_t error_size) = 0;
};

EXPORT IVideo * CALL CreateVideo(IRenderWindow & renderWindow, ISystemModules & modules);
EXPORT void CALL DestroyVideo(IVideo * video);
