// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_audio_core/renderer/memory/pool_mapper.h"
#include "yuzu_audio_core/renderer/sink/circular_buffer_sink_info.h"

namespace AudioCore::Renderer {

CircularBufferSinkInfo::CircularBufferSinkInfo() {
    state.fill(0);
    parameter.fill(0);
    type = Type::CircularBufferSink;

    auto state_{reinterpret_cast<CircularBufferState*>(state.data())};
    state_->address_info.Setup(0, 0);
}

void CircularBufferSinkInfo::CleanUp() {
    // The state is a CircularBufferState: there is no upsampler to free.
    parameter.fill(0);
    type = Type::Invalid;
}

void CircularBufferSinkInfo::Update(BehaviorInfo::ErrorInfo& error_info, OutStatus& out_status,
                                    const InParameter& in_params, const PoolMapper& pool_mapper) {
    const auto buffer_params{
        reinterpret_cast<const CircularBufferInParameter*>(&in_params.circular_buffer)};
    auto current_params{reinterpret_cast<CircularBufferInParameter*>(parameter.data())};
    auto current_state{reinterpret_cast<CircularBufferState*>(state.data())};

    if (in_use == buffer_params->in_use && !buffer_unmapped) {
        error_info.error_code = ResultSuccess;
        error_info.address = CpuAddr(0);
        out_status.writeOffset = current_state->last_pos2;
        return;
    }

    node_id = in_params.node_id;
    in_use = in_params.in_use;

    if (in_use) {
        buffer_unmapped =
            !pool_mapper.TryAttachBuffer(error_info, current_state->address_info,
                                         buffer_params->cpu_address, buffer_params->size);
        *current_params = *buffer_params;
    } else {
        *current_params = *buffer_params;
    }
    out_status.writeOffset = current_state->last_pos2;
}

void CircularBufferSinkInfo::UpdateForCommandGeneration() {
    if (in_use) {
        auto params{reinterpret_cast<CircularBufferInParameter*>(parameter.data())};
        auto state_{reinterpret_cast<CircularBufferState*>(state.data())};

        const auto pos{state_->current_pos};
        state_->last_pos2 = state_->last_pos;
        state_->last_pos = pos;

        state_->current_pos += static_cast<s32>(params->input_count * params->sample_count *
                                                GetSampleFormatByteSize(SampleFormat::PcmInt16));
        if (params->size > 0) {
            state_->current_pos %= params->size;
        }
    }
}

} // namespace AudioCore::Renderer
