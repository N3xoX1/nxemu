// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Network {
class RoomNetwork;
}

namespace Service::NIFM {

void LoopProcess(Core::System& system);

class IGeneralService final : public ServiceFramework<IGeneralService> {
public:
    explicit IGeneralService(Core::System& system_);
    ~IGeneralService() override;

private:
    void GetClientId(HLERequestContext& ctx);
    void CreateScanRequest(HLERequestContext& ctx);
    void CreateRequest(HLERequestContext& ctx);
    void GetCurrentNetworkProfile(HLERequestContext& ctx);
    void RemoveNetworkProfile(HLERequestContext& ctx);
    void GetCurrentIpAddress(HLERequestContext& ctx);
    void CreateTemporaryNetworkProfile(HLERequestContext& ctx);
    void GetCurrentIpConfigInfo(HLERequestContext& ctx);
    void SetWirelessCommunicationEnabled(HLERequestContext& ctx);
    void IsWirelessCommunicationEnabled(HLERequestContext& ctx);
    void GetInternetConnectionStatus(HLERequestContext& ctx);
    void IsEthernetCommunicationEnabled(HLERequestContext& ctx);
    void IsAnyInternetRequestAccepted(HLERequestContext& ctx);
    void IsAnyForegroundRequestAccepted(HLERequestContext& ctx);

    Network::RoomNetwork& network;
};

} // namespace Service::NIFM
