// Production DMA methods at NXEmu 26fef25b, frozen for differential tests.
// SPDX-License-Identifier: GPL-2.0-or-later
void BaselineDma::ProcessCommands(std::span<const CommandHeader> commands)
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

void BaselineDma::SetState(const CommandHeader & command_header)
{
    dma_state.method = command_header.method;
    dma_state.subchannel = command_header.subchannel;
    dma_state.method_count = command_header.method_count;
}

void BaselineDma::CallMethod(u32 argument) const
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

void BaselineDma::CallMultiMethod(const u32 * base_start, u32 num_methods) const
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
