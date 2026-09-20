// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_video_core/dma_pusher.h"

#include "video_settings.h"
#include "yuzu_common/cityhash.h"
#include "yuzu_common/settings.h"
#include "yuzu_common/yuzu_assert.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/gpu.h"
#include "yuzu_video_core/guest_memory.h"
#include "yuzu_video_core/memory_manager.h"
#include "yuzu_video_core/rasterizer_interface.h"

namespace Tegra
{

constexpr u32 MacroRegistersStart = 0xE00;
[[maybe_unused]] constexpr u32 ComputeInline = 0x6D;

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

    dma_state.is_last_call = true;

    while (gpu.IsPoweredOn())
    {
        if (!Step())
        {
            break;
        }
    }
    gpu.FlushCommands();
    gpu.OnCommandListEnd();
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

    // SYNC_WAIT applies to the current GP entry, including zero-length entries.
    if (header.sync && Settings::IsSyncMemoryOperationsEnabled())
    {
        SynchronizeMemoryOperations();
    }

    if (header.size > 0 && Settings::IsGPULevelHigh() &&
        dma_state.method >= MacroRegistersStart && subchannels[dma_state.subchannel])
    {
        subchannels[dma_state.subchannel]->current_dirty =
            memory_manager.IsMemoryDirty(dma_state.dma_get, header.size * sizeof(u32));
    }

    if (header.size > 0)
    {
        const bool use_safe = Settings::UseSafeDMAReads();
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
    rasterizer->WaitForFence();
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
                while (index < commands.size() && dma_state.method_count != 0)
                {
                    if (subchannel->execution_mask[dma_state.method])
                    {
                        dma_state.dma_word_offset = static_cast<u32>(index * sizeof(u32));
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
                    subchannel->method_sink.emplace_back(dma_state.method, commands[index].argument);
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

void DmaPusher::CallMethod(u32 argument) const
{
    if (dma_state.method < non_puller_methods)
    {
        puller.CallPullerMethod(Engines::Puller::MethodCall{dma_state.method, argument, dma_state.subchannel, dma_state.method_count,});
    }
    else
    {
        auto subchannel = subchannels[dma_state.subchannel];
        if (!subchannel->execution_mask[dma_state.method]) [[likely]]
        {
            subchannel->method_sink.emplace_back(dma_state.method, argument);
            return;
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
        puller.CallMultiMethod(dma_state.method, dma_state.subchannel, base_start, num_methods, dma_state.method_count);
    }
    else
    {
        auto subchannel = subchannels[dma_state.subchannel];
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
