// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_video_core/dma_pusher.h"

#include <algorithm>
#include <cstring>

#include "video_settings.h"
#include "yuzu_common/cityhash.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/settings.h"
#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/alignment.h"
#include "yuzu_video_core/engines/engine_upload.h"
#include "yuzu_video_core/engines/fermi_2d.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/engines/maxwell_dma.h"
#include "yuzu_video_core/gpu.h"
#include "yuzu_video_core/gpu_thread.h"
#include "yuzu_video_core/guest_memory.h"
#include "yuzu_video_core/host1x/host1x.h"
#include "yuzu_video_core/memory_manager.h"
#include "yuzu_video_core/rasterizer_interface.h"
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
#include "yuzu_common/android/performance_hint.h"
#endif

namespace Tegra
{
constexpr u32 MacroRegistersStart = 0xE00;
constexpr u32 ComputeInline = 0x6D;
// KeplerCompute and KeplerMemory share the inline to memory register layout
constexpr u32 UploadRegisters = 0x60;
constexpr u32 UploadExec = 0x6C;
constexpr u32 UploadData = 0x6D;
constexpr u32 ComputeLaunch = 0xAF;
static_assert(sizeof(Engines::Upload::Registers) == (UploadExec - UploadRegisters) * sizeof(u32));
// A copy or blit declares its destination only below this size
constexpr u64 MaxDeclaredWrite = u64{1} << 30;

DmaPusher::DmaPusher(GPU & gpu_, MemoryManager & memory_manager_, Control::ChannelState & channel_state_) :
    gpu{gpu_},
    memory_manager{memory_manager_},
    puller{gpu_, memory_manager_, *this, channel_state_}
{
}

DmaPusher::~DmaPusher() = default;

void DmaPusher::DispatchCalls()
{
    dma_pushbuffer_subindex = 0;

    ResumeUniformPrefetch();

    dma_state.is_last_call = true;

    while (gpu.IsPoweredOn())
    {
        if (!Step())
        {
            break;
        }
    }

    DrainMaxwell3D();

    if (memory_operation_sync_state == MemoryOperationSyncState::Armed)
    {
        WaitForArmedMemoryOperationSync();
    }
    else
    {
        memory_operation_sync_state = MemoryOperationSyncState::None;
    }

    // The fences it releases write what their own push declared, as in a Maxwell3D push
    PostBack([this]() {
        gpu.FlushCommands();
        gpu.OnCommandListEnd();
    }, {BackWrites::Kind::Nothing});
}

bool DmaPusher::IsThreaded() const
{
    return gpu.BackQueue().IsThreaded();
}

void DmaPusher::PostBack(std::function<void()> && work) const
{
    // Not a default argument: BackWrites is not complete inside DmaPusher
    PostBack(std::move(work), BackWrites{});
}

void DmaPusher::PostBack(std::function<void()> && work, BackWrites writes) const
{
    using Writes = VideoCommon::GPUThread::BackQueue::Writes;
    DrainMaxwell3D();
    VideoCommon::GPUThread::BackQueue & back_queue = gpu.BackQueue();
    switch (writes.kind)
    {
    case BackWrites::Kind::Nothing:
        back_queue.Push(std::move(work), Writes::Nothing);
        return;
    case BackWrites::Kind::Range:
        if (back_queue.IsThreaded())
        {
            VideoCommon::GPUThread::BackQueue::WriteRanges ranges;
            for (const auto & [address, size] : memory_manager.GetSubmappedDeviceRange(writes.address, writes.size))
            {
                ranges.emplace_back(address, address + size);
            }
            if (writes.extra_size != 0)
            {
                for (const auto & [address, size] :
                     memory_manager.GetSubmappedDeviceRange(writes.extra_address, writes.extra_size))
                {
                    ranges.emplace_back(address, address + size);
                }
            }
            back_queue.AddWrites(ranges);
        }
        back_queue.Push(std::move(work), Writes::AddedRanges);
        return;
    case BackWrites::Kind::Anywhere:
        break;
    }
    back_queue.Push(std::move(work), Writes::Anywhere);
}

std::vector<std::pair<u32, u32>> & DmaPusher::SinkFor(u32 subchannel_id) const
{
    // With the decode thread the engines other than Maxwell3D run on the GPU thread, their
    // sink is handed to them with the next executable method
    if (IsThreaded() && subchannel_type[subchannel_id] != Engines::EngineTypes::Maxwell3D)
    {
        return front_sinks[subchannel_id];
    }
    return subchannels[subchannel_id]->method_sink;
}

void DmaPusher::FlushFrontSink(u32 subchannel_id) const
{
    std::vector<std::pair<u32, u32>> & sink = front_sinks[subchannel_id];
    if (sink.empty() || subchannels[subchannel_id] == nullptr)
    {
        return;
    }
    // The writes belong to the engine bound before
    ApplyFrontSink(subchannel_id);
    PostBack([engine = subchannels[subchannel_id], writes = std::move(sink)]() {
        engine->method_sink.insert(engine->method_sink.end(), writes.begin(), writes.end());
        engine->ConsumeSink();
    }, {BackWrites::Kind::Nothing});
    sink.clear();
}

bool DmaPusher::Step()
{
    if (!ib_enable || dma_pushbuffer.empty())
    {
        return false;
    }

    CommandList & command_list = dma_pushbuffer.front();

    const size_t prefetch_size = command_list.prefetch_command_list.size();
    const size_t command_list_size = command_list.command_lists.size();

    if (prefetch_size == 0 && command_list_size == 0)
    {
        dma_pushbuffer.pop();
        dma_pushbuffer_subindex = 0;
        return true;
    }

    if (prefetch_size > 0)
    {
        ProcessCommands(command_list.prefetch_command_list);
        dma_pushbuffer.pop();
        return true;
    }

    const CommandListHeader & header = command_list.command_lists[dma_pushbuffer_subindex];
    dma_state.dma_get = header.Address();

    // Guest memory read below may be written by the queued Maxwell3D methods
    DrainMaxwell3DMemoryWrites(dma_state.dma_get, header.size * sizeof(u32));

    // SYNC_WAIT applies to the current GP entry, including zero-length entries.
    if (header.sync)
    {
        // The fence must come after the queued Maxwell3D work
        DrainMaxwell3D();
        switch (memory_operation_sync_state)
        {
        case MemoryOperationSyncState::Skip:
            memory_operation_sync_state = MemoryOperationSyncState::None;
            break;
        case MemoryOperationSyncState::Armed:
            WaitForArmedMemoryOperationSync();
            break;
        case MemoryOperationSyncState::None:
            if (Settings::IsGpuCommandSynchronizationEnabled())
            {
                SynchronizeMemoryOperations();
            }
            break;
        }
    }
    else
    {
        ASSERT(memory_operation_sync_state == MemoryOperationSyncState::None);
    }

    if (header.size > 0 && dma_state.method_count && subchannels[dma_state.subchannel])
    {
        const auto engine = subchannel_type[dma_state.subchannel];
        const bool compute_payload = engine == Engines::EngineTypes::KeplerCompute &&
                                     dma_state.method == ComputeInline && dma_state.non_incrementing;
        const bool macro_payload = engine == Engines::EngineTypes::Maxwell3D &&
                                   dma_state.method >= MacroRegistersStart;
        if (compute_payload || macro_payload)
        {
            // A safe fetch can download the parameters and clear their GPU dirty state. Keep
            // their origin so compute launches and draw macros consume live GPU
            // arguments, including at Normal accuracy where DMA reads may be unsafe.
            const std::size_t words = std::min<std::size_t>(dma_state.method_count, header.size);
            Engines::EngineInterface * const subchannel = subchannels[dma_state.subchannel];
            if (!IsThreaded())
            {
                // The queued draws must mark their GPU writes before the dirty check
                DrainMaxwell3D();
                subchannel->current_dirty =
                    memory_manager.IsMemoryDirty(dma_state.dma_get, words * sizeof(u32));
            }
            else if (compute_payload)
            {
                // The compute engine runs on the GPU thread, check there after the queued work
                PostBack([this, subchannel, address = dma_state.dma_get, words]() {
                    subchannel->current_dirty =
                        memory_manager.IsMemoryDirty(address, words * sizeof(u32));
                }, {BackWrites::Kind::Nothing});
            }
            else
            {
                // Early check, the macro call checks again after the queued work
                subchannel->current_dirty =
                    memory_manager.IsMemoryDirty(dma_state.dma_get, words * sizeof(u32));
                subchannel->current_payload = true;
            }
        }
    }

    if (header.size > 0)
    {
        // Only bypass readback when this entire entry continues a non-incrementing parameter
        // command. A completed command leaves its method behind, and a partial continuation can
        // be followed by new headers that the CPU must fetch safely.
        const bool is_gpu_consumed =
            Settings::IsDMALevelDefault() && dma_state.non_incrementing &&
            dma_state.method_count >= header.size &&
            (dma_state.method >= MacroRegistersStart ||
             (subchannel_type[dma_state.subchannel] == Engines::EngineTypes::KeplerCompute &&
              dma_state.method == ComputeInline));
        bool use_safe = Settings::UseSafeDMAReads() && !is_gpu_consumed;
        if (use_safe && IsThreaded())
        {
            // The rasterizer runs on the GPU thread: flush there, then read here
            const GPUVAddr address = dma_state.dma_get;
            const size_t size = header.size * sizeof(u32);
            // Diagnostic P0: wait for queued draws to register their GPU writes before the dirty check.
            // Also do this at High accuracy to isolate a possible decode/draw ordering regression.
            DrainMaxwell3D();
            gpu.BackQueue().WaitIdle();
            if (memory_manager.IsMemoryDirty(address, size))
            {
                DrainMaxwell3D();
                gpu.BackQueue().RunExclusive([&]() { memory_manager.FlushRegion(address, size); });
            }
            use_safe = false;
        }
        if (use_safe)
        {
            Tegra::Memory::GpuGuestMemory<Tegra::CommandHeader, GuestMemoryFlags::SafeRead> headers(
                memory_manager, dma_state.dma_get, header.size, &command_headers);
            ProcessCommands(headers);
        }
        else
        {
            Tegra::Memory::GpuGuestMemory<Tegra::CommandHeader, GuestMemoryFlags::UnsafeRead> headers(
                memory_manager, dma_state.dma_get, header.size, &command_headers);
            if (IsThreaded())
            {
                RecordUnwaitedEntry(dma_state.dma_get, header.size * sizeof(u32));
            }
            ProcessCommands(headers);
        }
    }

    // Prepare a following SYNC_WAIT at the current GP-entry boundary. Clean pushbuffers can be
    // fetched directly because DmaPusher already processes GP entries in order.
    const std::size_t next_entry_index = dma_pushbuffer_subindex + 1;
    if (next_entry_index < command_list_size && Settings::IsGpuCommandSynchronizationEnabled())
    {
        const CommandListHeader & next_header = command_list.command_lists[next_entry_index];
        if (next_header.sync)
        {
            DrainMaxwell3D();
            ASSERT(memory_operation_sync_state == MemoryOperationSyncState::None);
            // The look-ahead probe can't see the work still queued for the GPU thread
            if (!IsThreaded() && CanSkipCleanMemoryOperationSync(next_header))
            {
                memory_operation_sync_state = MemoryOperationSyncState::Skip;
            }
            else
            {
                ArmMemoryOperationSync();
            }
        }
    }

    if (++dma_pushbuffer_subindex >= command_list_size)
    {
        dma_pushbuffer.pop();
        dma_pushbuffer_subindex = 0;
    }

    return true;
}

void DmaPusher::SynchronizeMemoryOperations()
{
    ASSERT(rasterizer != nullptr);
    DrainMaxwell3D();
    gpu.BackQueue().RunExclusive([this]() { rasterizer->WaitForFence(); });
}

void DmaPusher::ArmMemoryOperationSync()
{
    ASSERT(rasterizer != nullptr);
    ASSERT(memory_operation_sync_state == MemoryOperationSyncState::None);

    memory_operation_sync_ready.store(false, std::memory_order_relaxed);
    memory_operation_sync_state = MemoryOperationSyncState::Armed;
    PostBack([this]() {
        rasterizer->SignalMemoryOperationFence([this]() {
            memory_operation_sync_ready.store(true, std::memory_order_release);
            memory_operation_sync_ready.notify_one();
        });
    }, {BackWrites::Kind::Nothing});
}

void DmaPusher::WaitForArmedMemoryOperationSync()
{
    ASSERT(memory_operation_sync_state == MemoryOperationSyncState::Armed);

#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
    const Common::Android::PerformanceHintWait hint_wait;
#endif
    memory_operation_sync_ready.wait(false, std::memory_order_acquire);
    memory_operation_sync_state = MemoryOperationSyncState::None;
}

bool DmaPusher::CanSkipCleanMemoryOperationSync(const CommandListHeader & header) const
{
    // The actual fetch must still resolve writes that can appear after this look-ahead probe.
    if (!Settings::UseSafeDMAReads() || header.size == 0 || header.conditional_fetch)
    {
        return false;
    }

    return !memory_manager.IsMemoryDirty(header.Address(), header.size * sizeof(u32));
}

void DmaPusher::ProcessCommands(std::span<const CommandHeader> commands)
{
    for (std::size_t index = 0; index < commands.size();)
    {
        const CommandHeader & command_header = commands[index];

        if (dma_state.method_count)
        {
            // Data word of methods command
            dma_state.dma_word_offset = static_cast<u32>(index * sizeof(u32));
            if (dma_state.non_incrementing)
            {
                const u32 max_write = static_cast<u32>(std::min<std::size_t>(index + dma_state.method_count, commands.size()) - index);
                CallMultiMethod(&command_header.argument, max_write);
                dma_state.method_count -= max_write;
                dma_state.is_last_call = true;
                index += max_write;
                continue;
            }
            if (dma_state.method >= non_puller_methods)
            {
                auto * const subchannel = subchannels[dma_state.subchannel];
                if (subchannel_type[dma_state.subchannel] == Engines::EngineTypes::Maxwell3D &&
                    !subchannel->execution_mask[dma_state.method])
                {
                    const size_t begin = index;
                    const u32 first_method = dma_state.method;
                    const size_t available = dma_increment_once ? 1 :
                        std::min<size_t>(dma_state.method_count, commands.size() - index);
                    do
                    {
                        ++dma_state.method;
                        ++index;
                    } while (index - begin < available &&
                             !subchannel->execution_mask[dma_state.method]);
                    const u32 count = static_cast<u32>(index - begin);
                    subchannel->PushMethodRun(first_method, commands.subspan(begin, count));
                    dma_state.is_last_call = dma_state.method_count <= count;
                    dma_state.method_count -= count;
                    if (dma_increment_once)
                    {
                        dma_state.non_incrementing = true;
                    }
                    continue;
                }
                while (index < commands.size() && dma_state.method_count != 0)
                {
                    if (subchannel->execution_mask[dma_state.method])
                    {
                        dma_state.dma_word_offset = (uint32_t)(index * sizeof(uint32_t));
                        dma_state.is_last_call = dma_state.method_count <= 1;
                        CallMethod(commands[index].argument);
                        dma_state.method++;
                        if (dma_increment_once)
                        {
                            dma_state.non_incrementing = true;
                        }
                        dma_state.method_count--;
                        index++;
                        break;
                    }
                    dma_state.is_last_call = dma_state.method_count <= 1;
                    SinkFor(dma_state.subchannel).emplace_back(dma_state.method, commands[index].argument);
                    dma_state.method++;
                    dma_state.method_count--;
                    index++;
                    if (dma_increment_once)
                    {
                        dma_state.non_incrementing = true;
                        break;
                    }
                }
                continue;
            }

            dma_state.is_last_call = dma_state.method_count <= 1;
            CallMethod(command_header.argument);

            if (!dma_state.non_incrementing)
            {
                dma_state.method++;
            }

            if (dma_increment_once)
            {
                dma_state.non_incrementing = true;
            }

            dma_state.method_count--;
        }
        else
        {
            // No command active - this is the first word of a new one
            switch (command_header.mode)
            {
            case SubmissionMode::Increasing:
                SetState(command_header);
                dma_state.non_incrementing = false;
                dma_increment_once = false;
                break;
            case SubmissionMode::NonIncreasing:
                SetState(command_header);
                dma_state.non_incrementing = true;
                dma_increment_once = false;
                break;
            case SubmissionMode::Inline:
                dma_state.method = command_header.method;
                dma_state.subchannel = command_header.subchannel;
                dma_state.dma_word_offset = static_cast<u64>(-static_cast<s64>(dma_state.dma_get)); // negate to set address as 0
                dma_state.is_last_call = true;
                CallMethod(command_header.arg_count);
                dma_state.non_incrementing = true;
                dma_increment_once = false;
                break;
            case SubmissionMode::IncreaseOnce:
                SetState(command_header);
                dma_state.non_incrementing = false;
                dma_increment_once = true;
                break;
            default:
                break;
            }
        }
        index++;
    }
}

void DmaPusher::SetState(const CommandHeader & command_header)
{
    dma_state.method = command_header.method;
    dma_state.subchannel = command_header.subchannel;
    dma_state.method_count = command_header.method_count;
}

void DmaPusher::DrainMaxwell3D() const
{
    if (maxwell3d_engine != nullptr)
    {
        static_cast<Engines::Maxwell3D *>(maxwell3d_engine)->Publish();
    }
}

bool DmaPusher::FetchesNewPages(DAddr address, u64 size) const
{
    constexpr u64 Limit = u64{1} << FetchedAddressBits;
    if (size == 0)
    {
        return false;
    }
    if (address >= Limit || size > Limit - address)
    {
        return true;
    }
    bool new_pages = false;
    const u64 last = (address + size - 1) >> FetchedPageBits;
    for (u64 page = address >> FetchedPageBits; page <= last; ++page)
    {
        u64 & word = fetched_pages[page >> 6];
        const u64 bit = u64{1} << (page & 63);
        new_pages |= (word & bit) == 0;
        word |= bit;
    }
    return new_pages;
}

bool DmaPusher::IsKnownPushbuffer(GPUVAddr address, size_t size) const
{
    if (size == 0)
    {
        return true;
    }
    const u64 generation = memory_manager.MappingGeneration();
    const u64 epoch = rasterizer != nullptr ? rasterizer->ComputeWriteEpoch() : 0;
    const u64 last = (address + size - 1) >> FetchedPageBits;
    for (u64 page = address >> FetchedPageBits; page <= last; ++page)
    {
        const KnownPage & known = known_pages[page % KnownPageCount];
        if (known.page != page || known.mapping_generation != generation ||
            known.compute_write_epoch != epoch)
        {
            return false;
        }
    }
    return true;
}

bool DmaPusher::DispatchMayWrite(GPUVAddr address, size_t size,
                                 std::span<const std::pair<DAddr, std::size_t>> ranges) const
{
    // A queued dispatch marks its shader writes only when it runs. Pushbuffers it generates
    // are the ones an earlier dispatch wrote, or fetched for the first time. The epoch is read
    // before the history, so a page it misses is not kept as known.
    const u64 generation = memory_manager.MappingGeneration();
    const u64 epoch = rasterizer != nullptr ? rasterizer->ComputeWriteEpoch() : 0;
    bool new_pages = false;
    bool written = rasterizer == nullptr;
    for (const auto & [device_address, device_size] : ranges)
    {
        new_pages |= FetchesNewPages(device_address, device_size);
        written = written || rasterizer->MayHaveComputeWrites(device_address, device_size);
    }
    if (!written && size != 0 && generation == memory_manager.MappingGeneration())
    {
        // Both page sets are 4 KiB, like the GPU small pages
        const u64 last = (address + size - 1) >> FetchedPageBits;
        for (u64 page = address >> FetchedPageBits; page <= last; ++page)
        {
            known_pages[page % KnownPageCount] = {page, generation, epoch};
        }
    }
    return queued_dispatch && (new_pages || written);
}

void DmaPusher::RecordUnwaitedEntry(GPUVAddr address, size_t size) const
{
    // Read after the entry: a dispatch that ran before the read is not counted
    const u64 first = completed_dispatches.load(std::memory_order_acquire) + 1;
    if (first > launched_dispatches)
    {
        return;
    }
    const u64 count = unwaited_count.load(std::memory_order_relaxed);
    UnwaitedEntry & entry = unwaited_entries[count % UnwaitedEntryCount];
    entry.address.store(address, std::memory_order_relaxed);
    entry.size.store(size, std::memory_order_relaxed);
    entry.first.store(first, std::memory_order_relaxed);
    entry.last.store(launched_dispatches, std::memory_order_relaxed);
    unwaited_count.store(count + 1, std::memory_order_release);
}

bool DmaPusher::UnwaitedEntryWritten(u64 id) const
{
    // Only the dispatches launched before an entry and still queued when it was read must have
    // written it first. A dispatch it launches itself comes after it.
    const u64 count = unwaited_count.load(std::memory_order_acquire);
    const u64 begin = count > UnwaitedEntryCount ? count - UnwaitedEntryCount : 0;
    for (u64 i = begin; i < count; ++i)
    {
        const UnwaitedEntry & entry = unwaited_entries[i % UnwaitedEntryCount];
        if (id < entry.first.load(std::memory_order_relaxed) || id > entry.last.load(std::memory_order_relaxed))
        {
            continue;
        }
        const GPUVAddr address = entry.address.load(std::memory_order_relaxed);
        const size_t size = entry.size.load(std::memory_order_relaxed);
        for (const auto & [device_address, device_size] : memory_manager.GetSubmappedDeviceRange(address, size))
        {
            if (rasterizer->MayHaveComputeWrites(device_address, device_size))
            {
                return true;
            }
        }
    }
    return false;
}

void DmaPusher::DispatchRan(u64 id) const
{
    // Entries are read without waiting when no dispatch wrote their pages yet. A queued
    // dispatch writing one of them first marks a new page and changes the epoch: the entry was
    // read too early. Dispatches run in launch order, so this one and the earlier ones ran.
    if (rasterizer != nullptr && !strict_dispatch_wait.load(std::memory_order_relaxed))
    {
        const u64 epoch = rasterizer->ComputeWriteEpoch();
        if (epoch != checked_epoch)
        {
            checked_epoch = epoch;
            if (UnwaitedEntryWritten(id))
            {
                strict_dispatch_wait.store(true, std::memory_order_relaxed);
                LOG_WARNING(HW_GPU, "A compute dispatch wrote commands read before it ran, waiting for every dispatch from now on");
            }
        }
    }
    completed_dispatches.store(id, std::memory_order_release);
}

void DmaPusher::DrainMaxwell3DMemoryWrites(GPUVAddr address, size_t size) const
{
    if (IsThreaded())
    {
        // Inline uploads don't show in the GPU dirty ranges, so queued work that may write this
        // entry must run before its commands are fetched or its parameters classified. The
        // Maxwell3D methods not handed over yet stay on this side unless they may write it.
        auto * const maxwell3d = static_cast<Engines::Maxwell3D *>(maxwell3d_engine);
        if (queued_dispatch && gpu.BackQueue().IsIdle())
        {
            queued_dispatch = false;
        }
        decltype(memory_manager.GetSubmappedDeviceRange(address, size)) ranges;
        bool have_ranges = false;
        if (!IsKnownPushbuffer(address, size))
        {
            ranges = memory_manager.GetSubmappedDeviceRange(address, size);
            have_ranges = true;
            if (DispatchMayWrite(address, size, {ranges.data(), ranges.size()}))
            {
                gpu.BackQueue().WaitIdle();
                queued_dispatch = false;
            }
        }
        const bool unpublished = maxwell3d != nullptr && maxwell3d->HasPendingMemoryWrites();
        if (!unpublished && !gpu.BackQueue().MayWriteAny())
        {
            return;
        }
        if (!have_ranges)
        {
            ranges = memory_manager.GetSubmappedDeviceRange(address, size);
        }
        if (unpublished && maxwell3d->UnpublishedMayWrite(ranges))
        {
            DrainMaxwell3D();
        }
        for (const auto & [device_address, device_size] : ranges)
        {
            if (gpu.BackQueue().MayWrite(device_address, device_address + device_size))
            {
                gpu.BackQueue().WaitIdle();
                break;
            }
        }
    }
    else if (maxwell3d_engine != nullptr)
    {
        auto* maxwell3d = static_cast<Engines::Maxwell3D*>(maxwell3d_engine);
        if (maxwell3d->HasPendingMemoryWrites())
        {
            maxwell3d->DrainStream();
        }
    }
}

namespace
{
/// Puller methods with no effect beyond their register, the trigger methods read them
bool IsPullerRegisterOnly(u32 method)
{
    switch (static_cast<BufferMethods>(method))
    {
    case BufferMethods::Nop:
    case BufferMethods::SemaphoreAddressHigh:
    case BufferMethods::SemaphoreAddressLow:
    case BufferMethods::SemaphoreSequencePayload:
    case BufferMethods::SyncpointPayload:
    case BufferMethods::WrcacheFlush:
        return true;
    default:
        return false;
    }
}

enum class SemaphoreOperation : u32
{
    AcquireEqual = 0x1,
    WriteLong = 0x2,
    AcquireGequal = 0x4,
    AcquireMask = 0x8,
};

bool IsPullerAcquire(u32 method, u32 argument)
{
    switch (static_cast<BufferMethods>(method))
    {
    case BufferMethods::SemaphoreOperation:
        return static_cast<SemaphoreOperation>(argument & 0xF) != SemaphoreOperation::WriteLong;
    case BufferMethods::SemaphoreAcquire:
        return true;
    case BufferMethods::SyncpointOperation:
        return (argument & 1) == 0;
    default:
        return false;
    }
}
} // namespace

bool DmaPusher::IsAcquireMet(u32 method, u32 argument) const
{
    const auto reg = [this](BufferMethods register_method) {
        return front_puller_regs[static_cast<u32>(register_method)];
    };
    const auto semaphore_word = [&](u32 & word) {
        const GPUVAddr address = (static_cast<GPUVAddr>(reg(BufferMethods::SemaphoreAddressHigh) & 0xFF) << 32) |
                                 reg(BufferMethods::SemaphoreAddressLow);
        if (memory_manager.GetPointer(address) == nullptr)
        {
            return false;
        }
        memory_manager.ReadBlockUnsafe(address, &word, sizeof(word));
        return true;
    };
    using Operation = SemaphoreOperation;
    const u32 sequence = reg(BufferMethods::SemaphoreSequencePayload);
    u32 word{};
    switch (static_cast<BufferMethods>(method))
    {
    case BufferMethods::SemaphoreOperation:
        if (!semaphore_word(word))
        {
            return false;
        }
        switch (static_cast<Operation>(argument & 0xF))
        {
        case Operation::AcquireEqual:
            return word == sequence;
        case Operation::AcquireGequal:
            return word >= sequence;
        case Operation::AcquireMask:
            return word == 0 || sequence != 0;
        default:
            return false;
        }
    case BufferMethods::SemaphoreAcquire:
        return semaphore_word(word) && word == argument;
    case BufferMethods::SyncpointOperation:
        return gpu.Host1x().GetSyncpointManager().IsReadyHost((argument >> 8) & 0xFFFFFF,
                                                              reg(BufferMethods::SyncpointPayload));
    default:
        return false;
    }
}

DmaPusher::BackWrites DmaPusher::PullerWrites(u32 method, std::span<const u32> arguments) const
{
    // The producer that releases an acquire may have written the entries after it: decode
    // fetches nothing more before the GPU thread ran the acquire
    if (std::ranges::any_of(arguments, [method](u32 argument) { return IsPullerAcquire(method, argument); }))
    {
        return {};
    }
    const GPUVAddr semaphore =
        (GPUVAddr{front_puller_regs[static_cast<u32>(BufferMethods::SemaphoreAddressHigh)]} << 32) |
        front_puller_regs[static_cast<u32>(BufferMethods::SemaphoreAddressLow)];
    switch (static_cast<BufferMethods>(method))
    {
    case BufferMethods::SemaphoreOperation:
    {
        // Without acquires, a long release that reports the payload and a timestamp
        return {BackWrites::Kind::Range, semaphore, 16};
    }
    case BufferMethods::SemaphoreRelease:
        return {BackWrites::Kind::Range, semaphore, sizeof(u32)};
    case BufferMethods::Nop:
    case BufferMethods::SemaphoreAddressHigh:
    case BufferMethods::SemaphoreAddressLow:
    case BufferMethods::SemaphoreSequencePayload:
    case BufferMethods::SyncpointPayload:
    case BufferMethods::WrcacheFlush:
    case BufferMethods::RefCnt:
    case BufferMethods::SyncpointOperation:
    case BufferMethods::WaitForIdle:
    case BufferMethods::NonStallInterrupt:
    case BufferMethods::MemOpA:
    case BufferMethods::MemOpB:
    case BufferMethods::MemOpC:
    case BufferMethods::MemOpD:
    case BufferMethods::Yield:
        // They signal, increment syncpoints, invalidate or release fences. Released fences
        // write what their own push declared
        return {BackWrites::Kind::Nothing};
    default:
        return {};
    }
}

DmaPusher::BackWrites DmaPusher::EngineWrites(u32 subchannel_id, u32 method) const
{
    if (!subchannels[subchannel_id]->execution_mask[method])
    {
        // Only sets a register
        return {BackWrites::Kind::Nothing};
    }
    switch (subchannel_type[subchannel_id])
    {
    case Engines::EngineTypes::KeplerCompute:
        if (method == ComputeLaunch)
        {
            if (strict_dispatch_wait.load(std::memory_order_relaxed) ||
                Settings::IsStrictComputeOrderingEnabled())
            {
                return {};
            }
            // The dispatch marks its shader writes GPU modified only when it runs, see
            // DrainMaxwell3DMemoryWrites and DispatchRan
            queued_dispatch = true;
            posted_dispatch = ++launched_dispatches;
            return {BackWrites::Kind::Nothing};
        }
        [[fallthrough]];
    case Engines::EngineTypes::KeplerMemory:
        if (method == UploadExec)
        {
            return {BackWrites::Kind::Nothing};
        }
        if (method == UploadData)
        {
            return UploadWrites(subchannel_id);
        }
        return {};
    case Engines::EngineTypes::MaxwellDMA:
        return CopyWrites();
    case Engines::EngineTypes::Fermi2D:
        return BlitWrites();
    default:
        return {};
    }
}

DmaPusher::BackWrites DmaPusher::CopyWrites() const
{
    using Regs = Engines::MaxwellDMA::Regs;
    using LaunchDMA = Engines::MaxwellDMA::LaunchDMA;
    static_assert(sizeof(Regs) == sizeof(front_copy));
    Regs regs;
    std::memcpy(&regs, front_copy.data(), sizeof(regs));
    const LaunchDMA & launch = regs.launch_dma;
    // A remap writes up to 4 components of up to 4 bytes per element
    const u32 bytes_per_element = launch.remap_enable != 0
        ? (regs.remap_const.num_dst_components_minus_one + 1) * (regs.remap_const.component_size_minus_one + 1)
        : 1;
    const u64 line_bytes = u64{regs.line_length_in} * bytes_per_element;
    const GPUVAddr destination = regs.offset_out;
    BackWrites writes{BackWrites::Kind::Range, destination, 0};
    if (line_bytes == 0 || (launch.multi_line_enable && regs.line_count == 0))
    {
        writes.kind = BackWrites::Kind::Nothing;
    }
    else if (!launch.multi_line_enable)
    {
        // A block linear destination moves the bytes inside their 512 byte GOB
        constexpr u64 GobSize = 512;
        writes.address = destination & ~(GobSize - 1);
        writes.size = Common::AlignUp(destination + line_bytes, GobSize) - writes.address;
    }
    else if (launch.dst_memory_layout == LaunchDMA::MemoryLayout::PITCH)
    {
        if (regs.pitch_out < 0)
        {
            return {};
        }
        // A block linear source writes whole lines back
        const u64 pitch = static_cast<u32>(regs.pitch_out);
        writes.size = std::max(u64{regs.line_count - 1} * pitch + line_bytes, u64{regs.line_count} * pitch);
    }
    else
    {
        const Tegra::DMA::Parameters & dst = regs.dst_params;
        if (dst.layer != 0 || dst.block_size.width != 0)
        {
            return {};
        }
        const u64 width_bytes = std::max<u64>(u64{dst.width} * bytes_per_element,
                                              u64{dst.origin.x} * bytes_per_element + line_bytes);
        const u32 height = std::max<u32>(dst.height, dst.origin.y + regs.line_count);
        writes.size = BlockLinearSize(width_bytes, height, dst.depth, dst.block_size.height,
                                      dst.block_size.depth);
    }
    if (launch.semaphore_type.Value() != LaunchDMA::SemaphoreType::NONE)
    {
        writes.extra_address = regs.semaphore.address;
        writes.extra_size = 16;
        if (writes.kind == BackWrites::Kind::Nothing)
        {
            writes = {BackWrites::Kind::Range, regs.semaphore.address, 16};
        }
    }
    if (writes.kind == BackWrites::Kind::Range && writes.size > MaxDeclaredWrite)
    {
        return {};
    }
    return writes;
}

DmaPusher::BackWrites DmaPusher::BlitWrites() const
{
    using Regs = Engines::Fermi2D::Regs;
    static_assert(sizeof(Regs) == sizeof(front_blit));
    Regs regs;
    std::memcpy(&regs, front_blit.data(), sizeof(regs));
    const Engines::Fermi2D::Surface & dst = regs.dst;
    if (dst.layer != 0 || dst.block_width != 0)
    {
        return {};
    }
    // Cover the rectangle too when it leaves the surface
    const auto & args = regs.pixels_from_memory;
    const u64 width = std::max<s64>(s64{dst.width}, s64{args.dst_x0} + args.dst_width);
    const u64 height = std::max<s64>(s64{dst.height}, s64{args.dst_y0} + args.dst_height);
    u64 size;
    if (dst.linear == Engines::Fermi2D::MemoryLayout::Pitch)
    {
        size = u64{dst.pitch} * height * std::max<u32>(dst.depth, 1) + width * 16;
    }
    else if (height > 0xFFFFFFFF)
    {
        return {};
    }
    else
    {
        // Formats have at most 16 bytes per pixel
        size = BlockLinearSize(width * 16, static_cast<u32>(height), dst.depth, dst.block_height,
                               dst.block_depth);
    }
    if (size > MaxDeclaredWrite)
    {
        return {};
    }
    return {BackWrites::Kind::Range, dst.Address(), size};
}

u64 DmaPusher::BlockLinearSize(u64 line_bytes, u32 height, u32 depth, u32 block_height,
                               u32 block_depth)
{
    // GOBs are 64 bytes by 8 lines, blocks stack up to 32 GOBs in height and depth. Sizes past
    // MaxDeclaredWrite stop there, without overflow.
    const u64 gob_lines = u64{8} << std::min<u32>(block_height, 5);
    const u64 block_depth_size = u64{1} << std::min<u32>(block_depth, 5);
    const u64 plane = Common::AlignUp(std::min(line_bytes, MaxDeclaredWrite + 1), u64{64}) *
                      Common::AlignUp(u64{std::max<u32>(height, 1)}, gob_lines);
    return std::min(plane, MaxDeclaredWrite + 1) * Common::AlignUp(u64{std::max<u32>(depth, 1)}, block_depth_size);
}

DmaPusher::BackWrites DmaPusher::UploadWrites(u32 subchannel_id) const
{
    const bool compute = subchannel_type[subchannel_id] == Engines::EngineTypes::KeplerCompute;
    const std::array<u32, 13> & regs = front_upload[compute ? 0 : 1];
    const u32 line_length = regs[0];
    const u32 line_count = regs[1];
    if (line_length == 0 || line_count == 0)
    {
        return {BackWrites::Kind::Nothing};
    }
    if ((regs[UploadExec - UploadRegisters] & 1) == 0)
    {
        // The block linear size depends on the surface layout
        return {};
    }
    const GPUVAddr address = (GPUVAddr{regs[2]} << 32) | regs[3];
    const u64 pitch = regs[4];
    return {BackWrites::Kind::Range, address, u64{line_count - 1} * pitch + line_length};
}

void DmaPusher::TrackFrontRegister(u32 subchannel_id, u32 method, u32 argument) const
{
    switch (subchannel_type[subchannel_id])
    {
    case Engines::EngineTypes::KeplerCompute:
    case Engines::EngineTypes::KeplerMemory:
        if (method >= UploadRegisters && method <= UploadExec)
        {
            front_upload[subchannel_type[subchannel_id] == Engines::EngineTypes::KeplerCompute ? 0 : 1]
                        [method - UploadRegisters] = argument;
        }
        break;
    case Engines::EngineTypes::MaxwellDMA:
        if (method < front_copy.size())
        {
            front_copy[method] = argument;
        }
        break;
    case Engines::EngineTypes::Fermi2D:
        if (method < front_blit.size())
        {
            front_blit[method] = argument;
        }
        break;
    default:
        break;
    }
}

void DmaPusher::ApplyFrontSink(u32 subchannel_id) const
{
    // Each subchannel keeps its register writes until its engine runs a method, which applies
    // them to the engine: two subchannels may share one engine
    for (const auto & [method, argument] : front_sinks[subchannel_id])
    {
        TrackFrontRegister(subchannel_id, method, argument);
    }
}

void DmaPusher::UpdateUniformPrefetchAtAcquire(u32 method, u32 argument) const
{
    if (maxwell3d_engine == nullptr || !IsPullerAcquire(method, argument))
    {
        return;
    }
    const bool queued_work_can_rewrite =
        method != static_cast<u32>(BufferMethods::SyncpointOperation);
    if (queued_work_can_rewrite)
    {
        DrainMaxwell3D();
    }
    const bool met = (!queued_work_can_rewrite || gpu.BackQueue().IsIdle()) &&
                     IsAcquireMet(method, argument);
    if (met)
    {
        // Guest writes the acquire waited for can postdate the copies made so far
        maxwell3d_engine->RefreshUniformPrefetch();
    }
    else if (maxwell3d_engine->StopUniformPrefetch())
    {
        unmet_acquire_seen = true;
    }
}

void DmaPusher::ResumeUniformPrefetch() const
{
    if (maxwell3d_engine == nullptr)
    {
        return;
    }
    if (unmet_acquire_seen && IsThreaded())
    {
        // A copy must not read guest memory before the GPU thread ran the acquire
        DrainMaxwell3D();
        gpu.BackQueue().WaitIdle();
    }
    unmet_acquire_seen = false;
    maxwell3d_engine->StartUniformPrefetch();
}

void DmaPusher::CallMethod(u32 argument) const
{
    if (dma_state.method < non_puller_methods)
    {
        const Engines::Puller::MethodCall method_call{dma_state.method, argument, dma_state.subchannel, dma_state.method_count};
        // Binding changes the subchannels of this decode side, the other puller methods use
        // the rasterizer
        if (!IsThreaded() || dma_state.method == static_cast<u32>(BufferMethods::BindObject))
        {
            DrainMaxwell3D();
            puller.CallPullerMethod(method_call);
            return;
        }
        front_puller_regs[dma_state.method] = argument;
        UpdateUniformPrefetchAtAcquire(dma_state.method, argument);
        if (IsPullerRegisterOnly(dma_state.method))
        {
            // Order against Maxwell3D does not matter, only the puller triggers read these
            pending_puller.push_back(method_call);
            return;
        }
        PostBack([this, pending = std::move(pending_puller), method_call]() {
            for (const Engines::Puller::MethodCall & call : pending)
            {
                puller.CallPullerMethod(call);
            }
            puller.CallPullerMethod(method_call);
        }, PullerWrites(dma_state.method, std::span{&argument, 1}));
        pending_puller.clear();
    }
    else
    {
        auto subchannel = subchannels[dma_state.subchannel];
        if (!subchannel->execution_mask[dma_state.method]) [[likely]]
        {
            if (subchannel_type[dma_state.subchannel] == Engines::EngineTypes::Maxwell3D)
            {
                subchannel->PushMethod(dma_state.method, argument);
            }
            else
            {
                SinkFor(dma_state.subchannel).emplace_back(dma_state.method, argument);
            }
            return;
        }
        const GPUVAddr segment = dma_state.dma_get + dma_state.dma_word_offset;
        if (subchannel_type[dma_state.subchannel] != Engines::EngineTypes::Maxwell3D)
        {
            if (IsThreaded())
            {
                ApplyFrontSink(dma_state.subchannel);
                TrackFrontRegister(dma_state.subchannel, dma_state.method, argument);
                posted_dispatch = 0;
                const BackWrites writes = EngineWrites(dma_state.subchannel, dma_state.method);
                PostBack([this, subchannel, sink = std::move(front_sinks[dma_state.subchannel]), segment,
                          method = dma_state.method, argument,
                          is_last_call = dma_state.is_last_call, dispatch = posted_dispatch]() mutable {
                    if (!sink.empty())
                    {
                        subchannel->method_sink = std::move(sink);
                        subchannel->ConsumeSink();
                    }
                    subchannel->current_dma_segment = segment;
                    subchannel->CallMethod(method, argument, is_last_call);
                    if (dispatch != 0)
                    {
                        DispatchRan(dispatch);
                    }
                }, writes);
                front_sinks[dma_state.subchannel].clear();
                return;
            }
            DrainMaxwell3D();
        }
        if (!subchannel->method_sink.empty())
        {
            subchannel->ConsumeSink();
        }
        subchannel->current_dma_segment = dma_state.dma_get + dma_state.dma_word_offset;
        subchannel->CallMethod(dma_state.method, argument, dma_state.is_last_call);
    }
}

void DmaPusher::CallMultiMethod(const u32 * base_start, u32 num_methods) const
{
    if (dma_state.method < non_puller_methods)
    {
        if (!IsThreaded() || dma_state.method == static_cast<u32>(BufferMethods::BindObject))
        {
            DrainMaxwell3D();
            puller.CallMultiMethod(dma_state.method, dma_state.subchannel, base_start, num_methods, dma_state.method_count);
            return;
        }
        for (u32 i = 0; i < num_methods; ++i)
        {
            front_puller_regs[dma_state.method] = base_start[i];
            UpdateUniformPrefetchAtAcquire(dma_state.method, base_start[i]);
        }
        PostBack([this, payload = std::vector<u32>(base_start, base_start + num_methods),
                  method = dma_state.method, subchannel_id = dma_state.subchannel,
                  pending = dma_state.method_count, held = std::move(pending_puller)]() {
            for (const Engines::Puller::MethodCall & call : held)
            {
                puller.CallPullerMethod(call);
            }
            puller.CallMultiMethod(method, subchannel_id, payload.data(),
                                   static_cast<u32>(payload.size()), pending);
        }, PullerWrites(dma_state.method, std::span{base_start, num_methods}));
        pending_puller.clear();
    }
    else
    {
        auto subchannel = subchannels[dma_state.subchannel];
        if (subchannel_type[dma_state.subchannel] != Engines::EngineTypes::Maxwell3D)
        {
            if (IsThreaded())
            {
                ApplyFrontSink(dma_state.subchannel);
                if (num_methods != 0)
                {
                    TrackFrontRegister(dma_state.subchannel, dma_state.method, base_start[num_methods - 1]);
                }
                posted_dispatch = 0;
                const BackWrites writes = EngineWrites(dma_state.subchannel, dma_state.method);
                PostBack([this, subchannel, sink = std::move(front_sinks[dma_state.subchannel]),
                          segment = dma_state.dma_get + dma_state.dma_word_offset,
                          payload = std::vector<u32>(base_start, base_start + num_methods),
                          method = dma_state.method, pending = dma_state.method_count,
                          dispatch = posted_dispatch]() mutable {
                    if (!sink.empty())
                    {
                        subchannel->method_sink = std::move(sink);
                        subchannel->ConsumeSink();
                    }
                    subchannel->current_dma_segment = segment;
                    subchannel->CallMultiMethod(method, payload.data(),
                                                static_cast<u32>(payload.size()), pending);
                    if (dispatch != 0)
                    {
                        DispatchRan(dispatch);
                    }
                }, writes);
                front_sinks[dma_state.subchannel].clear();
                return;
            }
            DrainMaxwell3D();
        }
        if (!subchannel->method_sink.empty())
        {
            subchannel->ConsumeSink();
        }
        subchannel->current_dma_segment = dma_state.dma_get + dma_state.dma_word_offset;
        subchannel->CallMultiMethod(dma_state.method, base_start, num_methods, dma_state.method_count);
    }
}

void DmaPusher::BindRasterizer(VideoCore::RasterizerInterface * rasterizer_)
{
    rasterizer = rasterizer_;
    puller.BindRasterizer(rasterizer_);
}
} // namespace Tegra
