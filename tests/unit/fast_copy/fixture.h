// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_common/virtual_buffer.h"
#include "yuzu_video_core/engines/fast_copy.h"
#include "yuzu_video_core/memory_manager.h"
#include "yuzu_video_core/rasterizer_interface.h"
#include <cstring>

struct TestDMA : Tegra::Engines::AccelerateDMAInterface {
    u64 image_attempts{};
    bool ImageToImage(Tegra::MemoryManager&, const Tegra::DMA::ImageCopy&,
                      const Tegra::DMA::ImageOperand&, const Tegra::DMA::ImageOperand&) override {
        ++image_attempts;
        return false;
    }
    bool BufferCopy(GPUVAddr, GPUVAddr, u64) override {
        return false;
    }
    bool BufferClear(GPUVAddr, u64, u32) override {
        return false;
    }
    bool ImageToBuffer(const Tegra::DMA::ImageCopy&, const Tegra::DMA::ImageOperand&,
                       const Tegra::DMA::BufferOperand&) override {
        return false;
    }
    bool BufferToImage(const Tegra::DMA::ImageCopy&, const Tegra::DMA::BufferOperand&,
                       const Tegra::DMA::ImageOperand&) override {
        return false;
    }
};

// Deliberately models cache notifications only; this fixture is not a GPU renderer.
struct TestRasterizer : VideoCore::RasterizerInterface {
    TestDMA dma;
    bool dirty{};
    u64 flushes{}, invalidations{}, dirty_checks{};
    void FlushRegion(DAddr, u64, VideoCommon::CacheType) override {
        ++flushes;
    }
    bool MustFlushRegion(DAddr, u64, VideoCommon::CacheType) override {
        ++dirty_checks;
        return dirty;
    }
    void InvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {
        ++invalidations;
    }
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override {
        return dma;
    }
    void Draw(bool is_indexed, u32 instance_count) override {}
    void DrawTexture() override {}
    void Clear(u32 layer_count) override {}
    void DispatchCompute() override {}
    void ResetCounter(VideoCommon::QueryType type) override {}
    void Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
               VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) override {}
    void BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr, u32 size) override {}
    void DisableGraphicsUniformBuffer(size_t stage, u32 index) override {}
    void SignalFence(std::function<void()>&& func) override {}
    void SignalMemoryOperationFence(std::function<void()>&& func) override {}
    void WaitForFence() override {}
    void SyncOperation(std::function<void()>&& func) override {}
    void SignalSyncPoint(u32 value) override {}
    void SignalReference() override {}
    void ReleaseFences(bool force) override {}
    void FlushAll() override {}
    RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override {
        return {};
    }
    void OnCacheInvalidation(PAddr addr, u64 size) override {}
    bool OnCPUWrite(PAddr addr, u64 size) override {
        return {};
    }
    void InvalidateGPUCache() override {}
    void UnmapMemory(DAddr addr, u64 size) override {}
    void ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size, bool preserve_contents) override {}
    void FlushAndInvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which) override {}
    void WaitForIdle() override {}
    void FragmentBarrier() override {}
    void TiledCacheBarrier() override {}
    void FlushCommands() override {}
    void TickFrame() override {}
    void AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                  std::span<const u8> memory) override {}
};

struct TestMemory : IDeviceMemory, IMemory {
    Common::VirtualBuffer<u8> bytes{64 * 1024 * 1024};
    const u8* BackingBasePointer() const override {
        return bytes.data();
    }
    u64 BackingSize() const override {
        return bytes.size();
    }
    u8* VirtualBasePointer() override {
        return bytes.data();
    }
    void EnableDirectMappedAddress() override {}
    u8* GetPointerSilent(u64 addr) override {
        return bytes.data() + addr;
    }
    bool ReadBlock(u64 addr, void* dst, u64 size) override {
        std::memcpy(dst, bytes.data() + addr, size);
        return true;
    }
    bool WriteBlock(u64 addr, const void* src, u64 size) override {
        std::memcpy(bytes.data() + addr, src, size);
        return true;
    }
    bool IsValidVirtualAddressRange(uint64_t base, uint64_t size) const override {
        return {};
    }
    void RasterizerMarkRegionCached(uint64_t vaddr, uint64_t size, bool cached) override {}
    bool InvalidateNCE(uint64_t vaddr, uint64_t size) override {
        return {};
    }
    bool NceAccessAllowed(uint64_t vaddr, bool write) override {
        return {};
    }
    bool NceTrackWrites(uint64_t vaddr) override {
        return {};
    }
    uint8_t Read8(uint64_t addr) override {
        return {};
    }
    uint16_t Read16(uint64_t addr) override {
        return {};
    }
    uint32_t Read32(uint64_t addr) override {
        return {};
    }
    uint64_t Read64(uint64_t addr) override {
        return {};
    }
    void Write8(uint64_t addr, uint8_t value) override {}
    void Write16(uint64_t addr, uint16_t value) override {}
    void Write32(uint64_t addr, uint32_t value) override {}
    void Write64(uint64_t addr, uint64_t value) override {}
    bool WriteExclusive8(uint64_t addr, uint8_t value, uint8_t expected) override {
        return {};
    }
    bool WriteExclusive16(uint64_t addr, uint16_t value, uint16_t expected) override {
        return {};
    }
    bool WriteExclusive32(uint64_t addr, uint32_t value, uint32_t expected) override {
        return {};
    }
    bool WriteExclusive64(uint64_t addr, uint64_t value, uint64_t expected) override {
        return {};
    }
    bool WriteExclusive128(uint64_t addr, uint64_t valueHi, uint64_t valueLow, uint64_t expectedHi,
                           uint64_t expectedLow) override {
        return {};
    }
};

struct CopyFixture {
    TestMemory physical;
    TestRasterizer rasterizer;
    Tegra::MaxwellDeviceMemoryManager device{physical};
    Tegra::MemoryManager gpu{device, 32, 1ULL << 32};
    static constexpr GPUVAddr Source = 0x100000;
    static constexpr GPUVAddr Destination = 0x2000000;
    static constexpr size_t RegionSize = 16 * 1024 * 1024;
    CopyFixture() {
        device.BindInterface(&rasterizer);
        const auto asid = device.RegisterProcess(&physical);
        device.Map(Source, Source, RegionSize, asid, true);
        device.Map(Destination, Destination, RegionSize, asid, true);
        gpu.BindRasterizer(&rasterizer);
        gpu.Map(Source, Source, RegionSize);
        gpu.Map(Destination, Destination, RegionSize);
    }
};
