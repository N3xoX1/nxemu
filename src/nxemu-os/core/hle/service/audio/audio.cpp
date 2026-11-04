// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/core.h"
#include "core/hle/service/audio/audio.h"
#include "core/hle/service/audio/audio_controller.h"
#include "core/hle/service/audio/audio_in_manager.h"
#include "core/hle/service/audio/audio_out_manager.h"
#include "core/hle/service/audio/audio_renderer_manager.h"
#include "core/hle/service/audio/final_output_recorder_manager.h"
#include "core/hle/service/audio/final_output_recorder_manager_for_applet.h"
#include "core/hle/service/audio/hardware_opus_decoder_manager.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"
#include "core/hle/service/set/system_settings_server.h"
#include "core/hle/service/sm/sm.h"

namespace Service::Audio {

void LoopProcess(Core::System& system) {
    // The audio controller needs set:sys: stop when it will not register.
    if (!system.ServiceManager().GetService<Set::ISystemSettingsServer>("set:sys", true)) {
        return;
    }

    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("audctl", std::make_shared<IAudioController>(system));
    server_manager->RegisterNamedService("audin:u", std::make_shared<IAudioInManager>(system));
    server_manager->RegisterNamedService("audout:u", std::make_shared<IAudioOutManager>(system));
    server_manager->RegisterNamedService(
        "audrec:a", std::make_shared<IFinalOutputRecorderManagerForApplet>(system));
    server_manager->RegisterNamedService("audrec:u",
                                         std::make_shared<IFinalOutputRecorderManager>(system));
    server_manager->RegisterNamedService("audren:u",
                                         std::make_shared<IAudioRendererManager>(system));
    server_manager->RegisterNamedService("hwopus",
                                         std::make_shared<IHardwareOpusDecoderManager>(system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::Audio
