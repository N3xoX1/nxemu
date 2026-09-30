// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <vector>

#include "yuzu_common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/nfc/common/device.h"
#include "core/hle/service/nfc/common/device_manager.h"
#include "core/hle/service/nfc/nfc_types.h"
#include "core/hle/service/nfp/nfp_interface.h"
#include "core/hle/service/nfp/nfp_result.h"
#include "core/hle/service/nfp/nfp_types.h"
#include "yuzu_hid_core/hid_types.h"

namespace Service::NFP {

Interface::Interface(Core::System& system_, const char* name)
    : NfcInterface{system_, name, NFC::BackendType::Nfp} {}

Interface::~Interface() = default;

void Interface::InitializeSystem(HLERequestContext& ctx) {
    Initialize(ctx);
}

void Interface::InitializeDebug(HLERequestContext& ctx) {
    Initialize(ctx);
}

void Interface::FinalizeSystem(HLERequestContext& ctx) {
    Finalize(ctx);
}

void Interface::FinalizeDebug(HLERequestContext& ctx) {
    Finalize(ctx);
}

void Interface::Mount(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    const auto model_type{rp.PopEnum<ModelType>()};
    const auto mount_target{rp.PopEnum<MountTarget>()};
    LOG_INFO(Service_NFP, "called, device_handle={}, model_type={}, mount_target={}", device_handle,
             model_type, mount_target);

    auto result = GetManager()->Mount(device_handle, model_type, mount_target);
    result = TranslateResultToServiceError(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void Interface::Unmount(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    LOG_INFO(Service_NFP, "called, device_handle={}", device_handle);

    auto result = GetManager()->Unmount(device_handle);
    result = TranslateResultToServiceError(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void Interface::OpenApplicationArea(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    const auto access_id{rp.Pop<u32>()};
    LOG_INFO(Service_NFP, "called, device_handle={}, access_id={}", device_handle, access_id);

    auto result = GetManager()->OpenApplicationArea(device_handle, access_id);
    result = TranslateResultToServiceError(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void Interface::GetApplicationArea(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    const auto requested_size = ctx.GetWriteBufferSize();
    const auto data_size = std::min<std::size_t>(requested_size, GetManager()->GetApplicationAreaSize());
    LOG_INFO(Service_NFP, "called, device_handle={}, buffer_size={}", device_handle,
             requested_size);

    std::vector<u8> data(data_size);
    auto result = GetManager()->GetApplicationArea(device_handle, data);
    result = TranslateResultToServiceError(result);

    if (result.IsError()) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result);
        return;
    }

    ctx.WriteBuffer(data);
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(result);
    rb.Push(static_cast<u32>(data_size));
}

void Interface::SetApplicationArea(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    const auto data{ctx.ReadBuffer()};
    LOG_INFO(Service_NFP, "called, device_handle={}, data_size={}", device_handle, data.size());

    auto result = GetManager()->SetApplicationArea(device_handle, data);
    result = TranslateResultToServiceError(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void Interface::Flush(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    LOG_INFO(Service_NFP, "called, device_handle={}", device_handle);

    auto result = GetManager()->Flush(device_handle);
    result = TranslateResultToServiceError(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void Interface::Restore(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    LOG_INFO(Service_NFP, "called, device_handle={}", device_handle);

    auto result = GetManager()->Restore(device_handle);
    result = TranslateResultToServiceError(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void Interface::CreateApplicationArea(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    const auto access_id{rp.Pop<u32>()};
    const auto data{ctx.ReadBuffer()};
    LOG_INFO(Service_NFP, "called, device_handle={}, data_size={}, access_id={}", device_handle,
             access_id, data.size());

    auto result = GetManager()->CreateApplicationArea(device_handle, access_id, data);
    result = TranslateResultToServiceError(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void Interface::GetRegisterInfo(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    LOG_INFO(Service_NFP, "called, device_handle={}", device_handle);

    RegisterInfo register_info{};
    auto result = GetManager()->GetRegisterInfo(device_handle, register_info);
    result = TranslateResultToServiceError(result);

    if (result.IsSuccess()) {
        ctx.WriteBuffer(register_info);
    }

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void Interface::GetCommonInfo(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    LOG_INFO(Service_NFP, "called, device_handle={}", device_handle);

    CommonInfo common_info{};
    auto result = GetManager()->GetCommonInfo(device_handle, common_info);
    result = TranslateResultToServiceError(result);

    if (result.IsSuccess()) {
        ctx.WriteBuffer(common_info);
    }

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void Interface::GetModelInfo(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    LOG_INFO(Service_NFP, "called, device_handle={}", device_handle);

    ModelInfo model_info{};
    auto result = GetManager()->GetModelInfo(device_handle, model_info);
    result = TranslateResultToServiceError(result);

    if (result.IsSuccess()) {
        ctx.WriteBuffer(model_info);
    }

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void Interface::GetApplicationAreaSize(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    LOG_DEBUG(Service_NFP, "called, device_handle={}", device_handle);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(GetManager()->GetApplicationAreaSize());
}

void Interface::RecreateApplicationArea(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    const auto access_id{rp.Pop<u32>()};
    const auto data{ctx.ReadBuffer()};
    LOG_INFO(Service_NFP, "called, device_handle={}, data_size={}, access_id={}", device_handle,
             access_id, data.size());

    auto result = GetManager()->RecreateApplicationArea(device_handle, access_id, data);
    result = TranslateResultToServiceError(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void Interface::Format(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    LOG_INFO(Service_NFP, "called, device_handle={}", device_handle);

    auto result = GetManager()->Format(device_handle);
    result = TranslateResultToServiceError(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void Interface::GetAdminInfo(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    LOG_INFO(Service_NFP, "called, device_handle={}", device_handle);

    AdminInfo admin_info{};
    auto result = GetManager()->GetAdminInfo(device_handle, admin_info);
    result = TranslateResultToServiceError(result);

    if (result.IsSuccess()) {
        ctx.WriteBuffer(admin_info);
    }

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void Interface::GetRegisterInfoPrivate(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    LOG_INFO(Service_NFP, "called, device_handle={}", device_handle);

    RegisterInfoPrivate register_info{};
    auto result = GetManager()->GetRegisterInfoPrivate(device_handle, register_info);
    result = TranslateResultToServiceError(result);

    if (result.IsSuccess()) {
        ctx.WriteBuffer(register_info);
    }

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void Interface::SetRegisterInfoPrivate(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    const auto register_info_buffer{ctx.ReadBuffer()};
    LOG_INFO(Service_NFP, "called, device_handle={}, buffer_size={}", device_handle,
             register_info_buffer.size());

    if (register_info_buffer.size() < sizeof(RegisterInfoPrivate)) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultInvalidArgument);
        return;
    }

    RegisterInfoPrivate register_info{};
    memcpy(&register_info, register_info_buffer.data(), sizeof(RegisterInfoPrivate));
    auto result = GetManager()->SetRegisterInfoPrivate(device_handle, register_info);
    result = TranslateResultToServiceError(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void Interface::DeleteRegisterInfo(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    LOG_INFO(Service_NFP, "called, device_handle={}", device_handle);

    auto result = GetManager()->DeleteRegisterInfo(device_handle);
    result = TranslateResultToServiceError(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void Interface::DeleteApplicationArea(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    LOG_INFO(Service_NFP, "called, device_handle={}", device_handle);

    auto result = GetManager()->DeleteApplicationArea(device_handle);
    result = TranslateResultToServiceError(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void Interface::ExistsApplicationArea(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    LOG_INFO(Service_NFP, "called, device_handle={}", device_handle);

    bool has_application_area = false;
    auto result = GetManager()->ExistsApplicationArea(device_handle, has_application_area);
    result = TranslateResultToServiceError(result);

    if (result.IsError()) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result);
        return;
    }

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(result);
    rb.Push(has_application_area);
}

void Interface::GetAll(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::SetAll(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::FlushDebug(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::BreakTag(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::ReadBackupData(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::WriteBackupData(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::WriteNtf(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

} // namespace Service::NFP
