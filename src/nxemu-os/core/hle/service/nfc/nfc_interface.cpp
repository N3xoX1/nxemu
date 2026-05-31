// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/nfc/common/device_manager.h"
#include "core/hle/service/nfc/nfc_interface.h"
#include "core/hle/service/nfc/nfc_types.h"
#include "core/hle/service/nfp/nfp_result.h"
#include "core/hle/service/set/system_settings_server.h"
#include "core/hle/service/sm/sm.h"
#include "yuzu_hid_core/hid_types.h"

namespace Service::NFC {

NfcInterface::NfcInterface(Core::System& system_, const char* name, BackendType service_backend)
    : ServiceFramework{system_, name}, service_context{system_, service_name},
      backend_type{service_backend} {
    m_set_sys =
        system.ServiceManager().GetService<Service::Set::ISystemSettingsServer>("set:sys", true);
}

NfcInterface ::~NfcInterface() = default;

void NfcInterface::Initialize(HLERequestContext& ctx) {
    LOG_INFO(Service_NFC, "called");

    auto manager = GetManager();
    auto result = manager->Initialize();

    if (result.IsSuccess()) {
        state = State::Initialized;
    } else {
        manager->Finalize();
    }

    IPC::ResponseBuilder rb{ctx, 2, 0};
    rb.Push(result);
}

void NfcInterface::Finalize(HLERequestContext& ctx) {
    LOG_INFO(Service_NFC, "called");

    UNIMPLEMENTED();
}

void NfcInterface::GetState(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NFC, "called");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushEnum(state);
}

void NfcInterface::IsNfcEnabled(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NFC, "called");

    bool is_enabled{};
    const auto result = m_set_sys->GetNfcEnableFlag(&is_enabled);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(result);
    rb.Push(is_enabled);
}

void NfcInterface::ListDevices(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NFC, "called");

    UNIMPLEMENTED();
}

void NfcInterface::GetDeviceState(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void NfcInterface::GetNpadId(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void NfcInterface::AttachAvailabilityChangeEvent(HLERequestContext& ctx) {
    LOG_INFO(Service_NFC, "called");

    auto manager = GetManager();

    IPC::ResponseBuilder rb{ctx, 2, 1};
    rb.Push(ResultSuccess);
    rb.PushCopyObjects(manager->AttachAvailabilityChangeEvent());
}

void NfcInterface::StartDetection(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void NfcInterface::StopDetection(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void NfcInterface::GetTagInfo(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void NfcInterface::AttachActivateEvent(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void NfcInterface::AttachDeactivateEvent(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void NfcInterface::SetNfcEnabled(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto is_enabled{rp.Pop<bool>()};
    LOG_DEBUG(Service_NFC, "called, is_enabled={}", is_enabled);

    const auto result = m_set_sys->SetNfcEnableFlag(is_enabled);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void NfcInterface::ReadMifare(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void NfcInterface::WriteMifare(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void NfcInterface::SendCommandByPassThrough(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

std::shared_ptr<DeviceManager> NfcInterface::GetManager() {
    if (device_manager == nullptr) {
        device_manager = std::make_shared<DeviceManager>(system, service_context);
    }
    return device_manager;
}

BackendType NfcInterface::GetBackendType() const {
    return backend_type;
}

Result NfcInterface::TranslateResultToServiceError(Result result) const {
    UNIMPLEMENTED();
    R_SUCCEED();
}

Result NfcInterface::TranslateResultToNfp(Result result) const {
    UNIMPLEMENTED();
    R_SUCCEED();
}

Result NfcInterface::TranslateResultToMifare(Result result) const {
    UNIMPLEMENTED();
    R_SUCCEED();
}

} // namespace Service::NFC
