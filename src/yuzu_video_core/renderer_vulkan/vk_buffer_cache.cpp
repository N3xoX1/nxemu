// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <span>
#include <vector>

#include "yuzu_video_core/renderer_vulkan/vk_buffer_cache.h"
#include "yuzu_common/readback_reference.h"

#include "yuzu_video_core/renderer_vulkan/maxwell_to_vk.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "yuzu_video_core/renderer_vulkan/vk_update_descriptor.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"
#include "yuzu_video_core/vulkan_common/vulkan_memory_allocator.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"
#include "video_settings.h"

namespace Vulkan {
struct ReadbackShadowBudget {
    std::atomic<u64> used{};
    u64 capacity{};
};
struct ReadbackShadow {
    std::shared_ptr<ReadbackShadowBudget> budget;
    u64 bytes;
    vk::Buffer buffer;
    Common::ReadbackReference reference;
    std::shared_ptr<Common::ReadbackCpuReference> cpu_reference;
    ReadbackShadow(std::shared_ptr<ReadbackShadowBudget> budget_, u64 bytes_, vk::Buffer buffer_)
        : budget{std::move(budget_)}, bytes{bytes_}, buffer{std::move(buffer_)},
          cpu_reference{std::make_shared<Common::ReadbackCpuReference>(bytes_)} {}
    ~ReadbackShadow() { budget->used.fetch_sub(bytes, std::memory_order_relaxed); }
};
namespace {
VkBufferCopy MakeBufferCopy(const VideoCommon::BufferCopy& copy) {
    return VkBufferCopy{
        .srcOffset = copy.src_offset,
        .dstOffset = copy.dst_offset,
        .size = copy.size,
    };
}

VkIndexType IndexTypeFromNumElements(const Device& device, u32 num_elements) {
    if (num_elements <= 0xff && device.IsExtIndexTypeUint8Supported()) {
        return VK_INDEX_TYPE_UINT8_EXT;
    }
    if (num_elements <= 0xffff) {
        return VK_INDEX_TYPE_UINT16;
    }
    return VK_INDEX_TYPE_UINT32;
}

size_t BytesPerIndex(VkIndexType index_type) {
    switch (index_type) {
    case VK_INDEX_TYPE_UINT8_EXT:
        return 1;
    case VK_INDEX_TYPE_UINT16:
        return 2;
    case VK_INDEX_TYPE_UINT32:
        return 4;
    default:
        ASSERT_MSG(false, "Invalid index type={}", index_type);
        return 1;
    }
}

vk::Buffer CreateBuffer(const Device& device, const MemoryAllocator& memory_allocator, u64 size) {
    VkBufferUsageFlags flags =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (device.IsExtTransformFeedbackSupported()) {
        flags |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsExtConditionalRendering()) {
        flags |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
    }
    const VkBufferCreateInfo buffer_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = size,
        .usage = flags,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    return memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::DeviceLocal);
}
} // Anonymous namespace

Buffer::Buffer(BufferCacheRuntime& runtime, VideoCommon::NullBufferParams null_params)
    : VideoCommon::BufferBase(null_params), tracker{4096} {
    if (runtime.device.HasNullDescriptor()) {
        return;
    }
    device = &runtime.device;
    buffer = runtime.CreateNullBuffer();
    is_null = true;
}

Buffer::Buffer(BufferCacheRuntime& runtime, DAddr cpu_addr_, u64 size_bytes_)
    : VideoCommon::BufferBase(cpu_addr_, size_bytes_), device{&runtime.device},
      buffer{CreateBuffer(*device, runtime.memory_allocator, SizeBytes())}, tracker{SizeBytes()} {
    if (runtime.device.HasDebuggingToolAttached()) {
        buffer.SetObjectNameEXT(fmt::format("Buffer 0x{:x}", CpuAddr()).c_str());
    }
}

VkBufferView Buffer::View(u32 offset, u32 size, VideoCore::Surface::PixelFormat format) {
    const BufferView* view{FindOrCreateView(offset, size, format)};
    return view ? *view->handle : VK_NULL_HANDLE;
}

VkBufferView Buffer::SizeView(u32 offset, u32 size, VideoCore::Surface::PixelFormat format) {
    const BufferView* view{FindOrCreateView(offset, size, format)};
    if (!view) {
        return VK_NULL_HANDLE;
    }
    return view->size_handle ? *view->size_handle : *view->handle;
}

const Buffer::BufferView* Buffer::FindOrCreateView(u32 offset, u32 size,
                                                   VideoCore::Surface::PixelFormat format) {
    if (!device) {
        // Null buffer supported, return a null descriptor
        return nullptr;
    } else if (is_null) {
        // Null buffer not supported, adjust offset and size
        offset = 0;
        size = 0;
    }
    const auto it{std::ranges::find_if(views, [offset, size, format](const BufferView& view) {
        return offset == view.offset && size == view.size && format == view.format;
    })};
    if (it != views.end()) {
        return &*it;
    }
    MoltenVK::TexelBufferRange range{offset, size, size, true};
    if (device->NeedsTexelBufferPad() && !is_null) {
        range = MoltenVK::AlignedTexelBufferRange(
            static_cast<u32>(device->GetTexelBufferOffsetAlignment()), offset, size,
            VideoCore::Surface::BytesPerBlock(format));
        if (!range.aligned) {
            LOG_WARNING(Render_Vulkan,
                        "Texel buffer view at {} is not aligned to the texel size of format {}",
                        offset, static_cast<u32>(format));
        }
    }
    const auto create_view{[&](u32 view_size) {
        return device->GetLogical().CreateBufferView({
            .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .buffer = *buffer,
            .format = MaxwellToVK::SurfaceFormat(*device, FormatType::Buffer, false, format).format,
            .offset = range.offset,
            .range = view_size,
        });
    }};
    views.push_back({
        .offset = offset,
        .size = size,
        .format = format,
        .handle = create_view(range.size),
        // Both views are the same unless the range was moved back to an aligned offset
        .size_handle = range.size_view_size != range.size ? create_view(range.size_view_size)
                                                          : vk::BufferView{},
    });
    return &views.back();
}

class QuadIndexBuffer {
public:
    QuadIndexBuffer(const Device& device_, MemoryAllocator& memory_allocator_,
                    Scheduler& scheduler_, StagingBufferPool& staging_pool_)
        : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_},
          staging_pool{staging_pool_} {}

    virtual ~QuadIndexBuffer() = default;

    void UpdateBuffer(u32 num_indices_) {
        if (num_indices_ <= num_indices) {
            return;
        }

        scheduler.Finish();

        num_indices = num_indices_;
        index_type = IndexTypeFromNumElements(device, num_indices);

        const u32 num_quads = GetQuadsNum(num_indices);
        const u32 num_triangle_indices = num_quads * 6;
        const u32 num_first_offset_copies = 4;
        const size_t bytes_per_index = BytesPerIndex(index_type);
        const size_t size_bytes = num_triangle_indices * bytes_per_index * num_first_offset_copies;
        const VkBufferCreateInfo buffer_ci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = size_bytes,
            .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        buffer = memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::DeviceLocal);
        if (device.HasDebuggingToolAttached()) {
            buffer.SetObjectNameEXT("Quad LUT");
        }

        const bool host_visible = buffer.IsHostVisible();
        const StagingBufferRef staging = [&] {
            if (host_visible) {
                return StagingBufferRef{};
            }
            return staging_pool.Request(size_bytes, MemoryUsage::Upload);
        }();

        u8* staging_data = host_visible ? buffer.Mapped().data() : staging.mapped_span.data();
        const size_t quad_size = bytes_per_index * 6;

        for (u32 first = 0; first < num_first_offset_copies; ++first) {
            for (u32 quad = 0; quad < num_quads; ++quad) {
                MakeAndUpdateIndices(staging_data, quad_size, quad, first);
                staging_data += quad_size;
            }
        }

        if (!host_visible) {
            scheduler.RequestOutsideRenderPassOperationContext();
            scheduler.Record([src_buffer = staging.buffer, src_offset = staging.offset,
                              dst_buffer = *buffer, size_bytes](vk::CommandBuffer cmdbuf) {
                const VkBufferCopy copy{
                    .srcOffset = src_offset,
                    .dstOffset = 0,
                    .size = size_bytes,
                };
                const VkBufferMemoryBarrier write_barrier{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_INDEX_READ_BIT,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .buffer = dst_buffer,
                    .offset = 0,
                    .size = size_bytes,
                };
                cmdbuf.CopyBuffer(src_buffer, dst_buffer, copy);
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, write_barrier);
            });
        } else {
            buffer.Flush();
        }
    }

    void BindBuffer(u32 first) {
        const VkIndexType index_type_ = index_type;
        const size_t sub_first_offset = static_cast<size_t>(first % 4) * GetQuadsNum(num_indices);
        const size_t offset =
            (sub_first_offset + GetQuadsNum(first)) * 6ULL * BytesPerIndex(index_type);
        scheduler.Record([buffer_ = *buffer, index_type_, offset](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindIndexBuffer(buffer_, offset, index_type_);
        });
    }

protected:
    virtual u32 GetQuadsNum(u32 num_indices) const = 0;

    virtual void MakeAndUpdateIndices(u8* staging_data, size_t quad_size, u32 quad, u32 first) = 0;

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    StagingBufferPool& staging_pool;

    vk::Buffer buffer{};
    MemoryCommit memory_commit{};
    VkIndexType index_type{};
    u32 num_indices = 0;
};

class QuadArrayIndexBuffer : public QuadIndexBuffer {
public:
    QuadArrayIndexBuffer(const Device& device_, MemoryAllocator& memory_allocator_,
                         Scheduler& scheduler_, StagingBufferPool& staging_pool_)
        : QuadIndexBuffer(device_, memory_allocator_, scheduler_, staging_pool_) {}

    ~QuadArrayIndexBuffer() = default;

private:
    u32 GetQuadsNum(u32 num_indices_) const override {
        return num_indices_ / 4;
    }

    template <typename T>
    static std::array<T, 6> MakeIndices(u32 quad, u32 first) {
        std::array<T, 6> indices{0, 1, 2, 0, 2, 3};
        for (T& index : indices) {
            index = static_cast<T>(first + index + quad * 4);
        }
        return indices;
    }

    void MakeAndUpdateIndices(u8* staging_data, size_t quad_size, u32 quad, u32 first) override {
        switch (index_type) {
        case VK_INDEX_TYPE_UINT8_EXT:
            std::memcpy(staging_data, MakeIndices<u8>(quad, first).data(), quad_size);
            break;
        case VK_INDEX_TYPE_UINT16:
            std::memcpy(staging_data, MakeIndices<u16>(quad, first).data(), quad_size);
            break;
        case VK_INDEX_TYPE_UINT32:
            std::memcpy(staging_data, MakeIndices<u32>(quad, first).data(), quad_size);
            break;
        default:
            ASSERT(false);
            break;
        }
    }
};

class QuadStripIndexBuffer : public QuadIndexBuffer {
public:
    QuadStripIndexBuffer(const Device& device_, MemoryAllocator& memory_allocator_,
                         Scheduler& scheduler_, StagingBufferPool& staging_pool_)
        : QuadIndexBuffer(device_, memory_allocator_, scheduler_, staging_pool_) {}

    ~QuadStripIndexBuffer() = default;

private:
    u32 GetQuadsNum(u32 num_indices_) const override {
        return num_indices_ >= 4 ? (num_indices_ - 2) / 2 : 0;
    }

    template <typename T>
    static std::array<T, 6> MakeIndices(u32 quad, u32 first) {
        std::array<T, 6> indices{0, 3, 1, 0, 2, 3};
        for (T& index : indices) {
            index = static_cast<T>(first + index + quad * 2);
        }
        return indices;
    }

    void MakeAndUpdateIndices(u8* staging_data, size_t quad_size, u32 quad, u32 first) override {
        switch (index_type) {
        case VK_INDEX_TYPE_UINT8_EXT:
            std::memcpy(staging_data, MakeIndices<u8>(quad, first).data(), quad_size);
            break;
        case VK_INDEX_TYPE_UINT16:
            std::memcpy(staging_data, MakeIndices<u16>(quad, first).data(), quad_size);
            break;
        case VK_INDEX_TYPE_UINT32:
            std::memcpy(staging_data, MakeIndices<u32>(quad, first).data(), quad_size);
            break;
        default:
            ASSERT(false);
            break;
        }
    }
};

BufferCacheRuntime::BufferCacheRuntime(const Device& device_, MemoryAllocator& memory_allocator_,
                                       Scheduler& scheduler_, StagingBufferPool& staging_pool_,
                                       GuestDescriptorQueue& guest_descriptor_queue_,
                                       ComputePassDescriptorQueue& compute_pass_descriptor_queue,
                                       DescriptorPool& descriptor_pool)
    : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_},
      staging_pool{staging_pool_}, guest_descriptor_queue{guest_descriptor_queue_},
      quad_index_pass(device, scheduler, descriptor_pool, staging_pool,
                      compute_pass_descriptor_queue),
      readback_descriptor_queue{compute_pass_descriptor_queue},
      readback_descriptor_pool{descriptor_pool} {
    readback_shadow_budget = std::make_shared<ReadbackShadowBudget>();
    readback_shadow_budget->capacity = std::min<u64>(512ULL << 20, GetDeviceLocalMemory() / 16);
    if (device.GetDriverID() != VK_DRIVER_ID_QUALCOMM_PROPRIETARY) {
        // TODO: FixMe: Uint8Pass compute shader does not build on some Qualcomm drivers.
        uint8_pass = std::make_unique<Uint8Pass>(device, scheduler, descriptor_pool, staging_pool,
                                                 compute_pass_descriptor_queue);
    }
    quad_array_index_buffer = std::make_shared<QuadArrayIndexBuffer>(device_, memory_allocator_,
                                                                     scheduler_, staging_pool_);
    quad_strip_index_buffer = std::make_shared<QuadStripIndexBuffer>(device_, memory_allocator_,
                                                                     scheduler_, staging_pool_);
}

StagingBufferRef BufferCacheRuntime::UploadStagingBuffer(size_t size) {
    return staging_pool.Request(size, MemoryUsage::Upload);
}

StagingBufferRef BufferCacheRuntime::DownloadStagingBuffer(size_t size, bool deferred) {
    return staging_pool.Request(size, MemoryUsage::Download, deferred);
}

bool BufferCacheRuntime::CanDifferentialReadback(const Buffer& buffer,
                                                const VideoCommon::BufferCopy& copy) const {
    if (!differential_readback_enabled) return false;

    if (copy.size < (1ULL << 20) ||
        copy.size > Common::ReadbackReference::MaxCopySize ||
        ((copy.src_offset | copy.dst_offset | copy.size) & 3) != 0 ||
        copy.size + device.GetStorageBufferAlignment() > device.GetMaxStorageBufferRange()) {
        return false;
    }
    if (buffer.SizeBytes() > (128ULL << 20)) {
        return false;
    }
    if (buffer.readback_shadow) return true;
    const u64 used = readback_shadow_budget->used.load(std::memory_order_relaxed);
    const bool fits = used <= readback_shadow_budget->capacity &&
                      buffer.SizeBytes() <= readback_shadow_budget->capacity - used;
    return fits;
}

void BufferCacheRuntime::InvalidateReadbackReference(Buffer& buffer, u64 offset, u64 size) {
    if (buffer.readback_shadow) buffer.readback_shadow->reference.Invalidate(offset, size);
}

std::shared_ptr<Common::ReadbackCpuReference>
BufferCacheRuntime::GetReadbackCpuReference(const Buffer& buffer) const {
    return buffer.readback_shadow->cpu_reference;
}

bool BufferCacheRuntime::DifferentialReadback(Buffer& buffer, const StagingBufferRef& data,
                                             const StagingBufferRef& flags,
                                             const VideoCommon::BufferCopy& copy,
                                             u64 flag_offset) {
    if (!CanDifferentialReadback(buffer, copy)) return false;
    try {
        if (!readback_delta_pass) {
            readback_delta_pass = std::make_unique<ReadbackDeltaPass>(
                device, scheduler, readback_descriptor_pool, readback_descriptor_queue);
        }
        if (!buffer.readback_shadow) {
            const u64 bytes = buffer.SizeBytes();
            u64 used = readback_shadow_budget->used.load(std::memory_order_relaxed);
            do {
                if (used > readback_shadow_budget->capacity ||
                    bytes > readback_shadow_budget->capacity - used) return false;
            } while (!readback_shadow_budget->used.compare_exchange_weak(
                used, used + bytes, std::memory_order_relaxed));
            try {
                buffer.readback_shadow = std::make_shared<ReadbackShadow>(
                    readback_shadow_budget, bytes, CreateBuffer(device, memory_allocator, bytes));
            } catch (...) {
                readback_shadow_budget->used.fetch_sub(bytes, std::memory_order_relaxed);
                throw;
            }
            has_readback_references = true;
        }
        auto& reference = buffer.readback_shadow->reference;
        const auto valid = reference.ValidMask(copy.src_offset, copy.size);
        readback_delta_pass->Copy(buffer.Handle(), *buffer.readback_shadow->buffer,
                                 data.buffer, flags.buffer, copy.src_offset, copy.dst_offset,
                                 copy.size, flags.offset + flag_offset,
                                 flags.offset + flags.mapped_span.size(), valid);
        reference.Scheduled(copy.src_offset, copy.size);
        return true;
    } catch (const vk::Exception& exception) {
        LOG_WARNING(Render_Vulkan, "Differential GPU readback disabled: {}", exception.what());
        differential_readback_enabled = false;
        return false;
    } catch (const std::bad_alloc&) {
        LOG_WARNING(Render_Vulkan, "Differential GPU readback disabled: host reference allocation failed");
        differential_readback_enabled = false;
        return false;
    }
}

void BufferCacheRuntime::FreeDeferredStagingBuffer(StagingBufferRef& ref) {
    staging_pool.FreeDeferred(ref);
}

u64 BufferCacheRuntime::GetDeviceLocalMemory() const {
    return device.GetDeviceLocalMemory();
}

u64 BufferCacheRuntime::GetDeviceMemoryUsage() const {
    return device.GetDeviceMemoryUsage();
}

bool BufferCacheRuntime::CanReportMemoryUsage() const {
    return device.CanReportMemoryUsage();
}

bool BufferCacheRuntime::ShouldStreamUniformBuffer(const Buffer& buffer, u32 offset, u32 size) const {
    return device.GetDriverID() == VK_DRIVER_ID_MOLTENVK && buffer.IsRegionUsed(offset, size);
}

u32 BufferCacheRuntime::GetStorageBufferAlignment() const {
    return static_cast<u32>(device.GetStorageBufferAlignment());
}

void BufferCacheRuntime::TickFrame(Common::SlotVector<Buffer>& slot_buffers) noexcept {
    for (auto it = slot_buffers.begin(); it != slot_buffers.end(); it++) {
        it->ResetUsageTracking();
    }
}

void BufferCacheRuntime::MarkHostWrite(const Buffer& buffer) {
    scheduler.MarkBufferWrite(buffer.Handle());
}

void BufferCacheRuntime::Finish() {
    scheduler.Finish();
}

bool BufferCacheRuntime::CanReorderUpload(const Buffer& buffer,
                                          std::span<const VideoCommon::BufferCopy> copies) {
    if (videoSettings.disable_buffer_reorder) {
        return false;
    }
    const bool can_use_upload_cmdbuf =
        std::ranges::all_of(copies, [&](const VideoCommon::BufferCopy& copy) {
            return !buffer.IsRegionUsed(copy.dst_offset, copy.size);
        });
    return can_use_upload_cmdbuf;
}

void BufferCacheRuntime::CopyBuffer(VkBuffer dst_buffer, VkBuffer src_buffer,
                                    std::span<const VideoCommon::BufferCopy> copies, bool barrier,
                                    bool can_reorder_upload) {
    if (dst_buffer == VK_NULL_HANDLE || src_buffer == VK_NULL_HANDLE) {
        return;
    }
    static constexpr VkMemoryBarrier READ_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    static constexpr VkMemoryBarrier WRITE_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };

    // Measuring a popular game, this number never exceeds the specified size once data is warmed up
    boost::container::small_vector<VkBufferCopy, 8> vk_copies(copies.size());
    std::ranges::transform(copies, vk_copies.begin(), MakeBufferCopy);
    if (src_buffer == staging_pool.StreamBuf() && can_reorder_upload) {
        scheduler.RecordWithUploadBuffer([src_buffer, dst_buffer, vk_copies](
                                             vk::CommandBuffer, vk::CommandBuffer upload_cmdbuf) {
            upload_cmdbuf.CopyBuffer(src_buffer, dst_buffer, vk_copies);
        });
        return;
    }

    scheduler.MarkBufferWrite(dst_buffer);
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([src_buffer, dst_buffer, vk_copies, barrier](vk::CommandBuffer cmdbuf) {
        if (barrier) {
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, READ_BARRIER);
        }
        cmdbuf.CopyBuffer(src_buffer, dst_buffer, vk_copies);
        if (barrier) {
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, WRITE_BARRIER);
        }
    });
}

void BufferCacheRuntime::PreCopyBarrier() {
    static constexpr VkMemoryBarrier READ_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, READ_BARRIER);
    });
}

void BufferCacheRuntime::PostCopyBarrier() {
    static constexpr VkMemoryBarrier WRITE_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               0, WRITE_BARRIER);
    });
}

void BufferCacheRuntime::ClearBuffer(VkBuffer dest_buffer, u32 offset, size_t size, u32 value) {
    if (dest_buffer == VK_NULL_HANDLE) {
        return;
    }
    static constexpr VkMemoryBarrier READ_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    static constexpr VkMemoryBarrier WRITE_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };

    scheduler.MarkBufferWrite(dest_buffer);
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([dest_buffer, offset, size, value](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, READ_BARRIER);
        cmdbuf.FillBuffer(dest_buffer, offset, size, value);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               0, WRITE_BARRIER);
    });
}

void BufferCacheRuntime::BindIndexBuffer(PrimitiveTopology topology, IndexFormat index_format,
                                         u32 base_vertex, u32 num_indices, VkBuffer buffer,
                                         u32 offset, u32 size) {
    VkIndexType vk_index_type = MaxwellToVK::IndexFormat(index_format);
    VkDeviceSize vk_offset = offset;
    VkBuffer vk_buffer = buffer;
    if (topology == PrimitiveTopology::Quads || topology == PrimitiveTopology::QuadStrip) {
        vk_index_type = VK_INDEX_TYPE_UINT32;
        std::tie(vk_buffer, vk_offset) =
            quad_index_pass.Assemble(index_format, num_indices, base_vertex, buffer, offset,
                                     topology == PrimitiveTopology::QuadStrip);
    } else if (vk_index_type == VK_INDEX_TYPE_UINT8_EXT && !device.IsExtIndexTypeUint8Supported()) {
        vk_index_type = VK_INDEX_TYPE_UINT16;
        if (uint8_pass && size != 0 && buffer != VK_NULL_HANDLE) {
            // The binding includes firstIndex's prefix; DrawIndexed still uses
            // that firstIndex after conversion. Never read beyond the guest range.
            std::tie(vk_buffer, vk_offset) = uint8_pass->Assemble(size, buffer, offset);
        }
    }
    if (vk_buffer == VK_NULL_HANDLE) {
        // Vulkan doesn't support null index buffers. Replace it with our own null buffer.
        ReserveNullBuffer();
        vk_buffer = *null_buffer;
    }
    scheduler.Record([vk_buffer, vk_offset, vk_index_type](vk::CommandBuffer cmdbuf) {
        cmdbuf.BindIndexBuffer(vk_buffer, vk_offset, vk_index_type);
    });
}

void BufferCacheRuntime::BindQuadIndexBuffer(PrimitiveTopology topology, u32 first, u32 count) {
    if (count == 0) {
        ReserveNullBuffer();
        scheduler.Record([this](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindIndexBuffer(*null_buffer, 0, VK_INDEX_TYPE_UINT32);
        });
        return;
    }

    if (topology == PrimitiveTopology::Quads) {
        quad_array_index_buffer->UpdateBuffer(first + count);
        quad_array_index_buffer->BindBuffer(first);
    } else if (topology == PrimitiveTopology::QuadStrip) {
        quad_strip_index_buffer->UpdateBuffer(first + count);
        quad_strip_index_buffer->BindBuffer(first);
    }
}

void BufferCacheRuntime::BindVertexBuffer(u32 index, VkBuffer buffer, u32 offset, u32 size,
                                          u32 stride) {
    index = vertex_binding_map[index];
    if (index >= device.GetMaxVertexInputBindings()) {
        return;
    }
    if (device.IsExtExtendedDynamicStateSupported()) {
        scheduler.Record([index, buffer, offset, size, stride](vk::CommandBuffer cmdbuf) {
            const VkDeviceSize vk_offset = buffer != VK_NULL_HANDLE ? offset : 0;
            const VkDeviceSize vk_size = buffer != VK_NULL_HANDLE ? size : VK_WHOLE_SIZE;
            const VkDeviceSize vk_stride = stride;
            cmdbuf.BindVertexBuffers2EXT(index, 1, &buffer, &vk_offset, &vk_size, &vk_stride);
        });
    } else {
        if (!device.HasNullDescriptor() && buffer == VK_NULL_HANDLE) {
            ReserveNullBuffer();
            buffer = *null_buffer;
            offset = 0;
        }
        scheduler.Record([index, buffer, offset](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindVertexBuffer(index, buffer, offset);
        });
    }
}

void BufferCacheRuntime::BindVertexBuffers(VideoCommon::HostBindings<Buffer>& bindings) {
    if (compact_vertex_bindings) {
        std::array<VkBuffer, 32> buffers{};
        std::array<VkDeviceSize, 32> offsets{};
        std::array<VkDeviceSize, 32> sizes{};
        std::array<VkDeviceSize, 32> strides{};
        u32 mask{};
        for (u32 index = 0; index < bindings.buffers.size(); ++index) {
            const u32 host = vertex_binding_map[bindings.min_index + index];
            if (host >= device.GetMaxVertexInputBindings()) {
                continue;
            }
            buffers[host] = bindings.buffers[index]->Handle();
            offsets[host] = bindings.offsets[index];
            sizes[host] = bindings.sizes[index];
            strides[host] = bindings.strides[index];
            if (buffers[host] == VK_NULL_HANDLE) {
                offsets[host] = 0;
                sizes[host] = VK_WHOLE_SIZE;
                if (!device.HasNullDescriptor()) {
                    ReserveNullBuffer();
                    buffers[host] = *null_buffer;
                }
            }
            mask |= 1U << host;
        }
        if (mask != 0) {
            scheduler.Record([buffers, offsets, sizes, strides, mask,
                              dynamic = device.IsExtExtendedDynamicStateSupported()]
                             (vk::CommandBuffer cmdbuf) {
                u32 remaining = mask;
                while (remaining != 0) {
                    const u32 first = static_cast<u32>(std::countr_zero(remaining));
                    const u32 count = static_cast<u32>(std::countr_one(remaining >> first));
                    if (dynamic) {
                        cmdbuf.BindVertexBuffers2EXT(first, count, buffers.data() + first,
                                                    offsets.data() + first, sizes.data() + first,
                                                    strides.data() + first);
                    } else {
                        cmdbuf.BindVertexBuffers(first, count, buffers.data() + first,
                                                 offsets.data() + first);
                    }
                    const u32 range = count == 32 ? ~0U : ((1U << count) - 1) << first;
                    remaining &= ~range;
                }
            });
        }
        return;
    }
    boost::container::small_vector<VkBuffer, 32> buffer_handles;
    for (u32 index = 0; index < bindings.buffers.size(); ++index) {
        auto handle = bindings.buffers[index]->Handle();
        if (handle == VK_NULL_HANDLE) {
            bindings.offsets[index] = 0;
            bindings.sizes[index] = VK_WHOLE_SIZE;
            if (!device.HasNullDescriptor()) {
                ReserveNullBuffer();
                handle = *null_buffer;
            }
        }
        buffer_handles.push_back(handle);
    }
    const u32 device_max = device.GetMaxVertexInputBindings();
    const u32 min_binding = std::min(bindings.min_index, device_max);
    const u32 max_binding = std::min(bindings.max_index, device_max);
    const u32 binding_count = max_binding - min_binding;
    if (binding_count == 0) {
        return;
    }
    if (device.IsExtExtendedDynamicStateSupported()) {
        scheduler.Record([bindings_ = std::move(bindings),
                          buffer_handles_ = std::move(buffer_handles),
                          binding_count](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindVertexBuffers2EXT(bindings_.min_index, binding_count, buffer_handles_.data(),
                                         bindings_.offsets.data(), bindings_.sizes.data(),
                                         bindings_.strides.data());
        });
    } else {
        scheduler.Record([bindings_ = std::move(bindings),
                          buffer_handles_ = std::move(buffer_handles),
                          binding_count](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindVertexBuffers(bindings_.min_index, binding_count, buffer_handles_.data(),
                                     bindings_.offsets.data());
        });
    }
}

void BufferCacheRuntime::BindTransformFeedbackBuffer(u32 index, VkBuffer buffer, u32 offset,
                                                     u32 size) {
    if (!device.IsExtTransformFeedbackSupported()) {
        // Already logged in the rasterizer
        return;
    }
    if (buffer == VK_NULL_HANDLE) {
        // Vulkan doesn't support null transform feedback buffers.
        // Replace it with our own null buffer.
        ReserveNullBuffer();
        buffer = *null_buffer;
        offset = 0;
        size = 0;
    }
    scheduler.Record([index, buffer, offset, size](vk::CommandBuffer cmdbuf) {
        const VkDeviceSize vk_offset = offset;
        const VkDeviceSize vk_size = size;
        cmdbuf.BindTransformFeedbackBuffersEXT(index, 1, &buffer, &vk_offset, &vk_size);
    });
}

void BufferCacheRuntime::BindTransformFeedbackBuffers(VideoCommon::HostBindings<Buffer>& bindings) {
    if (!device.IsExtTransformFeedbackSupported()) {
        // Kept for the shaders writing what transform feedback would capture
        for (size_t index = 0; index < emulated_xfb_buffers.size(); ++index) {
            EmulatedTransformFeedbackBuffer next{};
            if (index < bindings.buffers.size() && bindings.buffers[index] != nullptr) {
                Buffer& buffer{*bindings.buffers[index]};
                const u64 offset{bindings.offsets[index]};
                next.buffer = buffer.Handle();
                next.offset = static_cast<u32>(offset);
                next.size = static_cast<u32>(bindings.sizes[index]);
                next.capacity = buffer.SizeBytes() > offset ? buffer.SizeBytes() - offset : 0;
            }
            EmulatedTransformFeedbackBuffer& current{emulated_xfb_buffers[index]};
            // Draws keep capturing after each other while the buffer stays the same
            if (next.buffer == current.buffer && next.offset == current.offset &&
                next.size == current.size) {
                next.written = current.written;
            }
            current = next;
        }
        return;
    }
    boost::container::small_vector<VkBuffer, 4> buffer_handles;
    for (u32 index = 0; index < bindings.buffers.size(); ++index) {
        buffer_handles.push_back(bindings.buffers[index]->Handle());
    }
    scheduler.Record([bindings_ = std::move(bindings),
                      buffer_handles_ = std::move(buffer_handles)](vk::CommandBuffer cmdbuf) {
        cmdbuf.BindTransformFeedbackBuffersEXT(0, static_cast<u32>(buffer_handles_.size()),
                                               buffer_handles_.data(), bindings_.offsets.data(),
                                               bindings_.sizes.data());
    });
}

void BufferCacheRuntime::ReserveNullBuffer() {
    if (!null_buffer) {
        null_buffer = CreateNullBuffer();
    }
}

vk::Buffer BufferCacheRuntime::CreateNullBuffer() {
    VkBufferCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = 4,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        create_info.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    vk::Buffer ret = memory_allocator.CreateBuffer(create_info, MemoryUsage::DeviceLocal);
    if (device.HasDebuggingToolAttached()) {
        ret.SetObjectNameEXT("Null buffer");
    }

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([buffer = *ret](vk::CommandBuffer cmdbuf) {
        cmdbuf.FillBuffer(buffer, 0, VK_WHOLE_SIZE, 0);
    });

    return ret;
}
} // namespace Vulkan
