// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "yuzu_audio_core/renderer/audio_device.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"

namespace AudioCore {
class OutputListeners;
}

namespace Core::Timing {
struct EventType;
}

namespace Kernel {
class KReadableEvent;
}

namespace Service::Audio {

using AudioCore::Renderer::AudioDevice;

class IAudioDevice final : public ServiceFramework<IAudioDevice> {

public:
    explicit IAudioDevice(Core::System& system_, u64 applet_resource_user_id, u32 revision,
                          u32 device_num);
    ~IAudioDevice() override;

private:
    Result ListAudioDeviceName(
        OutArray<AudioDevice::AudioDeviceName, BufferAttr_HipcMapAlias> out_names,
        Out<s32> out_count);
    Result SetAudioDeviceOutputVolume(
        InArray<AudioDevice::AudioDeviceName, BufferAttr_HipcMapAlias> name, f32 volume);
    Result GetAudioDeviceOutputVolume(
        Out<f32> out_volume, InArray<AudioDevice::AudioDeviceName, BufferAttr_HipcMapAlias> name);
    Result GetActiveAudioDeviceName(
        OutArray<AudioDevice::AudioDeviceName, BufferAttr_HipcMapAlias> out_name);
    Result ListAudioDeviceNameAuto(
        OutArray<AudioDevice::AudioDeviceName, BufferAttr_HipcAutoSelect> out_names,
        Out<s32> out_count);
    Result SetAudioDeviceOutputVolumeAuto(
        InArray<AudioDevice::AudioDeviceName, BufferAttr_HipcAutoSelect> name, f32 volume);
    Result GetAudioDeviceOutputVolumeAuto(
        Out<f32> out_volume, InArray<AudioDevice::AudioDeviceName, BufferAttr_HipcAutoSelect> name);
    Result GetActiveAudioDeviceNameAuto(
        OutArray<AudioDevice::AudioDeviceName, BufferAttr_HipcAutoSelect> out_name);
    Result QueryAudioDeviceSystemEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result QueryAudioDeviceInputEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result QueryAudioDeviceOutputEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result GetActiveChannelCount(Out<u32> out_active_channel_count);
    Result ListAudioOutputDeviceName(
        OutArray<AudioDevice::AudioDeviceName, BufferAttr_HipcMapAlias> out_names,
        Out<s32> out_count);
    Result SetAudioDeviceOutputVolumeAutoTuneEnabled(bool enabled);
    Result IsAudioDeviceOutputVolumeAutoTuneEnabled(Out<bool> out_enabled);
    Result AcquireDeviceNotification(OutCopyHandle<Kernel::KReadableEvent> out_event, u64 device);
    Result ReleaseDeviceNotification(u64 device);
    Result IsActiveOutputDeviceEstimatedLowLatency(Out<bool> out_low_latency);

    KernelHelpers::ServiceContext service_context;
    std::unique_ptr<AudioCore::Renderer::AudioDevice> impl;
    Kernel::KEvent* event;
    /// Signals event on the timing thread, which may touch kernel objects
    std::shared_ptr<Core::Timing::EventType> output_changed_event;
    std::weak_ptr<AudioCore::OutputListeners> output_listeners;
    u64 output_listener_id{};
    Kernel::KEvent* notification_event;
    bool switch2_caller;
};

} // namespace Service::Audio
