// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_audio_core/audio_core.h"
#include "core/core.h"
#include "nxemu-os/os_settings.h"
#include "yuzu_audio_core/sink/sink_details.h"
#include "yuzu_common/settings.h"

namespace AudioCore
{

AudioCore::AudioCore(Core::System & system) :
    audio_manager{std::make_shared<AudioManager>()}
{
    CreateSinks();
    // Must be created after the sinks
    adsp = std::make_unique<ADSP::ADSP>(system, *output_sink);
}

AudioCore ::~AudioCore()
{
    Shutdown();
}

void AudioCore::CreateSinks()
{
    output_sink = Sink::CreateSinkFromID(osSettings.sink_id, osSettings.audio_output_device_id);
    input_sink = Sink::CreateSinkFromID(osSettings.sink_id, osSettings.audio_input_device_id);
}

void AudioCore::Shutdown()
{
    audio_manager->Shutdown();
}

AudioManager & AudioCore::GetAudioManager()
{
    return *audio_manager;
}

std::weak_ptr<AudioManager> AudioCore::GetAudioManagerWeak() const
{
    return audio_manager;
}

Sink::Sink & AudioCore::GetOutputSink()
{
    return *output_sink;
}

Sink::Sink & AudioCore::GetInputSink()
{
    return *input_sink;
}

ADSP::ADSP & AudioCore::ADSP()
{
    return *adsp;
}

} // namespace AudioCore
