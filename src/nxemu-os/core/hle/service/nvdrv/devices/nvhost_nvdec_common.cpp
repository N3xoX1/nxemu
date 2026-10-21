// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <utility>

#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/nvmap.h"
#include "core/hle/service/nvdrv/core/pinned_handle.h"
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "core/hle/service/nvdrv/devices/nvhost_nvdec_common.h"
#include "core/memory.h"

namespace Service::Nvidia::Devices {

namespace {
// Copies count amount of type T from the input vector into the dst vector.
// Returns the number of bytes written into dst.
template <typename T>
std::size_t SliceVectors(std::span<const u8> input, std::vector<T>& dst, std::size_t count,
                         std::size_t offset) {
    if (dst.empty()) {
        return 0;
    }
    const size_t bytes_copied = count * sizeof(T);
    if (input.size() < offset + bytes_copied) {
        return 0;
    }
    std::memcpy(dst.data(), input.data() + offset, bytes_copied);
    return bytes_copied;
}

// Writes the data in src to an offset into the dst vector. The offset is specified in bytes
// Returns the number of bytes written into dst.
template <typename T>
std::size_t WriteVectors(std::span<u8> dst, const std::vector<T>& src, std::size_t offset) {
    if (src.empty()) {
        return 0;
    }
    const size_t bytes_copied = src.size() * sizeof(T);
    if (dst.size() < offset + bytes_copied) {
        return 0;
    }
    std::memcpy(dst.data() + offset, src.data(), bytes_copied);
    return bytes_copied;
}
} // Anonymous namespace

nvhost_nvdec_common::nvhost_nvdec_common(Core::System& system_, NvCore::Container& core_, NvCore::ChannelType channel_type_) :
    nvdevice{system_}, 
    core{core_}, 
    syncpoint_manager{core.GetSyncpointManager()},
    nvmap{core.GetNvMapFile()}, 
    channel_type{channel_type_}
{
    auto& syncpts_accumulated = core.Host1xDeviceFile().syncpts_accumulated;
    if (syncpts_accumulated.empty())
    {
        channel_syncpoint = syncpoint_manager.AllocateSyncpoint(false);
    }
    else
    {
        channel_syncpoint = syncpts_accumulated.front();
        syncpts_accumulated.pop_front();
    }
}

nvhost_nvdec_common::~nvhost_nvdec_common()
{
    // Module destruction can close all device objects without an explicit ioctl
    // close. Join workers while the container, process and mappings still exist.
    auto& channels = core.Host1xDeviceFile().fd_to_id;
    for (const auto& [fd, session] : sessions) {
        if (const auto it = channels.find(fd); it != channels.end()) {
            system.GetVideo().ClearCdmaInstance(it->second);
        }
    }
    core.Host1xDeviceFile().mappings.Replace(this, {});
    mappings.reset();
    core.Host1xDeviceFile().syncpts_accumulated.push_back(channel_syncpoint);
}

NvResult nvhost_nvdec_common::SetNVMAPfd(IoctlSetNvmapFD& params)
{
    LOG_DEBUG(Service_NVDRV, "called, fd={}", params.nvmap_fd);

    nvmap_fd = params.nvmap_fd;
    return NvResult::Success;
}

NvResult nvhost_nvdec_common::Submit(IoctlSubmit& params, std::span<u8> data, DeviceFD fd)
{
    LOG_DEBUG(Service_NVDRV, "called NVDEC Submit, cmd_buffer_count={}", params.cmd_buffer_count);

    // Instantiate param buffers
    std::vector<CommandBuffer> command_buffers(params.cmd_buffer_count);
    std::vector<Reloc> relocs(params.relocation_count);
    std::vector<u32> reloc_shifts(params.relocation_count);
    std::vector<SyncptIncr> syncpt_increments(params.syncpoint_count);
    std::vector<u32> fence_thresholds(params.fence_count);

    // Slice input into their respective buffers
    std::size_t offset = 0;
    offset += SliceVectors(data, command_buffers, params.cmd_buffer_count, offset);
    offset += SliceVectors(data, relocs, params.relocation_count, offset);
    offset += SliceVectors(data, reloc_shifts, params.relocation_count, offset);
    offset += SliceVectors(data, syncpt_increments, params.syncpoint_count, offset);
    offset += SliceVectors(data, fence_thresholds, params.fence_count, offset);

    IVideo & video = system.GetVideo();
    const auto it = sessions.find(fd);
    if (it == sessions.end()) return NvResult::InvalidState;
    const auto session = it->second.lock();
    if (!session || !session->is_active.load(std::memory_order_acquire)) {
        return NvResult::InvalidState;
    }

    if (video.UseNvdec())
    {
        for (std::size_t i = 0; i < syncpt_increments.size(); i++)
        {
            const SyncptIncr& syncpt_incr = syncpt_increments[i];
            fence_thresholds[i] = syncpoint_manager.IncrementSyncpointMaxExt(syncpt_incr.id, syncpt_incr.increments);
        }
    }
    for (const auto& cmd_buffer : command_buffers)
    {
        const auto object = nvmap.GetHandle(cmd_buffer.memory_id);
        ASSERT_OR_EXECUTE(object, return NvResult::InvalidState;);
        std::vector<uint32_t> cmdlist(cmd_buffer.word_count);
        session->process->GetMemory().ReadBlock(object->address + cmd_buffer.offset, cmdlist.data(), cmdlist.size() * sizeof(uint32_t));
        auto snapshot = core.Host1xDeviceFile().mappings.Snapshot();
        // The callback owns the snapshot, including on enqueue rejection and
        // cancellation. Do not release any mapping at an intermediate syncpoint.
        auto resources = std::make_unique<std::shared_ptr<const Mappings>>(std::move(snapshot));
        video.PushCommandBufferWithResources(core.Host1xDeviceFile().fd_to_id[fd], cmdlist.data(),
            cmd_buffer.word_count, resources.release(), [](void* pointer) {
                delete static_cast<std::shared_ptr<const Mappings>*>(pointer);
            });
    }

    // Some games expect command_buffers to be written back
    offset = 0;
    offset += WriteVectors(data, command_buffers, offset);
    offset += WriteVectors(data, relocs, offset);
    offset += WriteVectors(data, reloc_shifts, offset);
    offset += WriteVectors(data, syncpt_increments, offset);
    offset += WriteVectors(data, fence_thresholds, offset);

    return NvResult::Success;
}

NvResult nvhost_nvdec_common::GetSyncpoint(IoctlGetSyncpoint& params)
{
    LOG_DEBUG(Service_NVDRV, "called GetSyncpoint, id={}", params.param);
    params.value = channel_syncpoint;
    return NvResult::Success;
}

NvResult nvhost_nvdec_common::GetWaitbase(IoctlGetWaitbase& params)
{
    LOG_DEBUG(Service_NVDRV, "called WAITBASE");
    params.value = 0; // Seems to be hard coded at 0
    return NvResult::Success;
}

NvResult nvhost_nvdec_common::MapBuffer(IoctlMapBuffer& params, std::span<MapBufferEntry> entries, DeviceFD fd)
{
    if (params.num_entries > entries.size()) return NvResult::BadValue;
    const auto session_it = sessions.find(fd);
    if (session_it == sessions.end()) return NvResult::InvalidState;
    const auto session = session_it->second.lock();
    if (!session || !session->is_active.load(std::memory_order_acquire)) return NvResult::InvalidState;
    Mappings added;
    added.reserve(params.num_entries);
    for (const auto& entry : entries.first(params.num_entries)) {
        auto lease = NvCore::PinnedHandle::Create(nvmap, entry.map_handle);
        if (!lease) return NvResult::BadValue;
        added.push_back(std::move(lease));
    }
    std::scoped_lock lock{mappings_mutex};
    auto next = std::make_shared<Mappings>(*mappings);
    next->insert(next->end(), added.begin(), added.end());
    core.Host1xDeviceFile().mappings.Replace(this, next);
    mappings = std::move(next);
    for (size_t i = 0; i < added.size(); ++i) entries[i].map_address = static_cast<u32>(added[i]->Address());
    return NvResult::Success;
}

NvResult nvhost_nvdec_common::UnmapBuffer(IoctlMapBuffer& params, std::span<MapBufferEntry> entries)
{
    if (params.num_entries > entries.size()) return NvResult::BadValue;
    // Publish a new mapping set. Existing jobs retain the old immutable set,
    // so guest free/unmap cannot recycle their device addresses prematurely.
    std::shared_ptr<const Mappings> previous;
    {
        std::scoped_lock lock{mappings_mutex};
        auto next = std::make_shared<Mappings>(*mappings);
        for (const auto& entry : entries.first(params.num_entries)) {
            const auto it = std::find_if(next->begin(), next->end(), [&](const auto& lease) {
                return lease->Id() == entry.map_handle;
            });
            if (it == next->end()) return NvResult::BadValue;
            next->erase(it);
        }
        core.Host1xDeviceFile().mappings.Replace(this, next);
        previous = std::exchange(mappings, std::move(next));
    }
    for (auto& entry : entries.first(params.num_entries)) entry = {};
    params = {};
    return NvResult::Success;
}

NvResult nvhost_nvdec_common::SetSubmitTimeout(u32 timeout)
{
    submit_timeout = timeout;
    LOG_DEBUG(Service_NVDRV, "called, timeout=0x{:X}", timeout);
    return NvResult::Success;
}

Kernel::KEvent* nvhost_nvdec_common::QueryEvent(u32 event_id)
{
    LOG_CRITICAL(Service_NVDRV, "Unknown HOSTX1 Event {}", event_id);
    return nullptr;
}

} // namespace Service::Nvidia::Devices
