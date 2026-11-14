// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later


#include "yuzu_video_core/dma_pusher.h"

#include "video_settings.h"
#include "yuzu_common/cityhash.h"
#include "yuzu_common/settings.h"
#include "yuzu_common/yuzu_assert.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/gpu.h"
#include "yuzu_video_core/gpu_thread.h"
#include "yuzu_video_core/guest_memory.h"
#include "yuzu_video_core/memory_manager.h"
#include "yuzu_video_core/rasterizer_interface.h"

namespace Tegra
{
constexpr u32 MacroRegistersStart = 0xE00;
constexpr u32 ComputeInline = 0x6D;

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

    if (maxwell3d_engine != nullptr)
    {
        static_cast<Engines::Maxwell3D *>(maxwell3d_engine)->UniformPrefetchSubmission();
    }

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

    PostBack([this]() {
        gpu.FlushCommands();
        gpu.OnCommandListEnd();
    });
}

bool DmaPusher::IsThreaded() const
{
    return gpu.BackQueue().IsThreaded();
}

void DmaPusher::PostBack(std::function<void()> && work) const
{
    DrainMaxwell3D();
    gpu.BackQueue().Push(std::move(work));
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
    DrainMaxwell3DMemoryWrites();

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
            auto * const subchannel = subchannels[dma_state.subchannel];
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
                });
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
    });
}

void DmaPusher::WaitForArmedMemoryOperationSync()
{
    ASSERT(memory_operation_sync_state == MemoryOperationSyncState::Armed);

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

void DmaPusher::DrainMaxwell3DMemoryWrites() const
{
    // With the decode thread this would be a sync point at nearly every GP entry. Push
    // buffers written by inline constant buffer data or I2M are not handled there.
    if (maxwell3d_engine != nullptr && !IsThreaded())
    {
        auto * const maxwell3d = static_cast<Engines::Maxwell3D *>(maxwell3d_engine);
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

/// Puller methods that can make the GPU wait for the guest CPU: the guest may write memory
/// that the next methods read after the wait. Everything but the release kinds.
bool IsPullerWait(u32 method, u32 argument)
{
    switch (static_cast<BufferMethods>(method))
    {
    case BufferMethods::SemaphoreOperation:
        return (argument & 0xF) != 2; // not WriteLong
    case BufferMethods::SyncpointOperation:
        return (argument & 1) == 0; // Acquire
    case BufferMethods::SemaphoreRelease:
    case BufferMethods::RefCnt:
    case BufferMethods::NonStallInterrupt:
        return false;
    default:
        return true;
    }
}
} // namespace

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
        if (IsPullerRegisterOnly(dma_state.method))
        {
            // Order against Maxwell3D does not matter, only the puller triggers read these
            pending_puller.push_back(method_call);
            return;
        }
        if (maxwell3d_engine != nullptr && IsPullerWait(dma_state.method, argument))
        {
            static_cast<Engines::Maxwell3D *>(maxwell3d_engine)->UniformPrefetchBarrier();
        }
        PostBack([this, pending = std::move(pending_puller), method_call]() {
            for (const Engines::Puller::MethodCall & call : pending)
            {
                puller.CallPullerMethod(call);
            }
            puller.CallPullerMethod(method_call);
        });
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
                PostBack([subchannel, sink = std::move(front_sinks[dma_state.subchannel]), segment,
                          method = dma_state.method, argument,
                          is_last_call = dma_state.is_last_call]() mutable {
                    if (!sink.empty())
                    {
                        subchannel->method_sink = std::move(sink);
                        subchannel->ConsumeSink();
                    }
                    subchannel->current_dma_segment = segment;
                    subchannel->CallMethod(method, argument, is_last_call);
                });
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
        PostBack([this, payload = std::vector<u32>(base_start, base_start + num_methods),
                  method = dma_state.method, subchannel_id = dma_state.subchannel,
                  pending = dma_state.method_count, held = std::move(pending_puller)]() {
            for (const Engines::Puller::MethodCall & call : held)
            {
                puller.CallPullerMethod(call);
            }
            puller.CallMultiMethod(method, subchannel_id, payload.data(),
                                   static_cast<u32>(payload.size()), pending);
        });
        pending_puller.clear();
    }
    else
    {
        auto subchannel = subchannels[dma_state.subchannel];
        if (subchannel_type[dma_state.subchannel] != Engines::EngineTypes::Maxwell3D)
        {
            if (IsThreaded())
            {
                PostBack([subchannel, sink = std::move(front_sinks[dma_state.subchannel]),
                          segment = dma_state.dma_get + dma_state.dma_word_offset,
                          payload = std::vector<u32>(base_start, base_start + num_methods),
                          method = dma_state.method, pending = dma_state.method_count]() mutable {
                    if (!sink.empty())
                    {
                        subchannel->method_sink = std::move(sink);
                        subchannel->ConsumeSink();
                    }
                    subchannel->current_dma_segment = segment;
                    subchannel->CallMultiMethod(method, payload.data(),
                                                static_cast<u32>(payload.size()), pending);
                });
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
