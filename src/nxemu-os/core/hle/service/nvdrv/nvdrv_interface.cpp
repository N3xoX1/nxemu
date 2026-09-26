// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: 2021 Skyline Team and Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_common/logging/log.h"
#include "yuzu_common/nvdata.h"
#include "yuzu_common/scope_exit.h"
#include "yuzu_common/string_util.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/hle/service/nvdrv/core/syncpoint_wait.h"
#include "core/hle/service/nvdrv/nvdrv_interface.h"

namespace Service::Nvidia {

void NVDRV::Open(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NVDRV, "called");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);

    if (!is_initialized) {
        rb.Push<DeviceFD>(0);
        rb.PushEnum(NvResult::NotInitialized);

        LOG_ERROR(Service_NVDRV, "NvServices is not initialized!");
        return;
    }

    const auto& buffer = ctx.ReadBuffer();
    const std::string device_name(Common::StringFromBuffer(buffer));

    if (device_name == "/dev/nvhost-prof-gpu") {
        rb.Push<DeviceFD>(0);
        rb.PushEnum(NvResult::NotSupported);

        LOG_WARNING(Service_NVDRV, "/dev/nvhost-prof-gpu cannot be opened in production");
        return;
    }

    DeviceFD fd = nvdrv->Open(device_name, session_id);

    rb.Push<DeviceFD>(fd);
    rb.PushEnum(fd != INVALID_NVDRV_FD ? NvResult::Success : NvResult::FileOperationFailed);
}

void NVDRV::ServiceError(HLERequestContext& ctx, NvResult result) {
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushEnum(result);
}

void NVDRV::Ioctl1(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto fd = rp.Pop<DeviceFD>();
    const auto command = rp.PopRaw<Ioctl>();
    LOG_DEBUG(Service_NVDRV, "called fd={}, ioctl=0x{:08X}", fd, command.raw);

    if (!is_initialized) {
        ServiceError(ctx, NvResult::NotInitialized);
        LOG_ERROR(Service_NVDRV, "NvServices is not initialized!");
        return;
    }

    auto pending = deferred_ioctls.find(&ctx);
    if (pending != deferred_ioctls.end()) {
        const bool signalled = pending->second.wait->IsSignalled();
        const auto fd_status = nvdrv->VerifyFD(fd);
        if (!signalled && fd_status == NvResult::Success) {
            ctx.SetIsDeferred();
            return;
        }
    }

    // Deferred requests keep their first input snapshot and their original deadline.
    output_buffer.resize_destructive(ctx.GetWriteBufferSize(0));
    const auto input_buffer = pending == deferred_ioctls.end()
                                  ? ctx.ReadBuffer(0)
                                  : std::span<const u8>{pending->second.input};
    const auto nv_result = nvdrv->Ioctl1(fd, command, input_buffer, output_buffer);
    if (pending != deferred_ioctls.end()) {
        // Also cancel the timer/callback on errors, such as a descriptor closed by another session.
        deferred_ioctls.erase(pending);
    } else if (nv_result == NvResult::Timeout) {
        const auto wait = nvdrv->GetSyncpointWait(fd, command, input_buffer);
        if (wait && wait->timeout_ms != 0) {
            DeferredIoctl deferred{
                std::vector<u8>{input_buffer.begin(), input_buffer.end()},
                std::make_unique<NvCore::SyncpointWait>(system.GetVideo(), system.CoreTiming(),
                    nvdrv->GetDeferralEvent(), wait->id, wait->threshold, wait->timeout_ms)};
            deferred_ioctls.emplace(&ctx, std::move(deferred));
            ctx.SetIsDeferred();
            return;
        }
    }

    if (command.is_out != 0) {
        ctx.WriteBuffer(output_buffer);
    }

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushEnum(nv_result);
}

void NVDRV::Ioctl2(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto fd = rp.Pop<DeviceFD>();
    const auto command = rp.PopRaw<Ioctl>();
    LOG_DEBUG(Service_NVDRV, "called fd={}, ioctl=0x{:08X}", fd, command.raw);

    if (!is_initialized) {
        ServiceError(ctx, NvResult::NotInitialized);
        LOG_ERROR(Service_NVDRV, "NvServices is not initialized!");
        return;
    }

    const auto input_buffer = ctx.ReadBuffer(0);
    const auto input_inlined_buffer = ctx.ReadBuffer(1);
    output_buffer.resize_destructive(ctx.GetWriteBufferSize(0));

    const auto nv_result =
        nvdrv->Ioctl2(fd, command, input_buffer, input_inlined_buffer, output_buffer);
    if (command.is_out != 0) {
        ctx.WriteBuffer(output_buffer);
    }

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushEnum(nv_result);
}

void NVDRV::Ioctl3(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto fd = rp.Pop<DeviceFD>();
    const auto command = rp.PopRaw<Ioctl>();
    LOG_DEBUG(Service_NVDRV, "called fd={}, ioctl=0x{:08X}", fd, command.raw);

    if (!is_initialized) {
        ServiceError(ctx, NvResult::NotInitialized);
        LOG_ERROR(Service_NVDRV, "NvServices is not initialized!");
        return;
    }

    const auto input_buffer = ctx.ReadBuffer(0);
    output_buffer.resize_destructive(ctx.GetWriteBufferSize(0));
    inline_output_buffer.resize_destructive(ctx.GetWriteBufferSize(1));

    const auto nv_result =
        nvdrv->Ioctl3(fd, command, input_buffer, output_buffer, inline_output_buffer);
    if (command.is_out != 0) {
        ctx.WriteBuffer(output_buffer, 0);
        ctx.WriteBuffer(inline_output_buffer, 1);
    }

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushEnum(nv_result);
}

void NVDRV::Close(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NVDRV, "called");

    if (!is_initialized) {
        ServiceError(ctx, NvResult::NotInitialized);
        LOG_ERROR(Service_NVDRV, "NvServices is not initialized!");
        return;
    }

    IPC::RequestParser rp{ctx};
    const auto fd = rp.Pop<DeviceFD>();
    const auto result = nvdrv->Close(fd);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushEnum(result);
}

void NVDRV::Initialize(HLERequestContext& ctx) {
    LOG_WARNING(Service_NVDRV, "(STUBBED) called");
    IPC::ResponseBuilder rb{ctx, 3};
    SCOPE_EXIT {
        rb.Push(ResultSuccess);
        rb.PushEnum(NvResult::Success);
    };

    if (is_initialized) {
        // No need to initialize again
        return;
    }

    IPC::RequestParser rp{ctx};
    const auto process_handle{ctx.GetCopyHandle(0)};
    // The transfer memory is lent to nvdrv as a work buffer since nvdrv is
    // unable to allocate as much memory on its own. For HLE it's unnecessary to handle it
    [[maybe_unused]] const auto transfer_memory_handle{ctx.GetCopyHandle(1)};
    [[maybe_unused]] const auto transfer_memory_size = rp.Pop<u32>();

    auto& container = nvdrv->GetContainer();
    auto process = ctx.GetObjectFromHandle<Kernel::KProcess>(process_handle);
    session_id = container.OpenSession(process.GetPointerUnsafe());

    is_initialized = true;
}

void NVDRV::QueryEvent(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto fd = rp.Pop<DeviceFD>();
    const auto event_id = rp.Pop<u32>();

    if (!is_initialized) {
        ServiceError(ctx, NvResult::NotInitialized);
        LOG_ERROR(Service_NVDRV, "NvServices is not initialized!");
        return;
    }

    Kernel::KEvent* event = nullptr;
    NvResult result = nvdrv->QueryEvent(fd, event_id, event);

    if (result == NvResult::Success) {
        IPC::ResponseBuilder rb{ctx, 3, 1};
        rb.Push(ResultSuccess);
        auto& readable_event = event->GetReadableEvent();
        rb.PushCopyObjects(readable_event);
        rb.PushEnum(NvResult::Success);
    } else {
        LOG_ERROR(Service_NVDRV, "Invalid event request!");
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.PushEnum(result);
    }
}

void NVDRV::SetAruid(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    pid = rp.Pop<u64>();
    LOG_WARNING(Service_NVDRV, "(STUBBED) called, pid=0x{:X}", pid);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushEnum(NvResult::Success);
}

void NVDRV::SetGraphicsFirmwareMemoryMarginEnabled(HLERequestContext& ctx) {
    LOG_WARNING(Service_NVDRV, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void NVDRV::GetStatus(HLERequestContext& ctx) {
    LOG_WARNING(Service_NVDRV, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushEnum(NvResult::Success);
}

void NVDRV::DumpGraphicsMemoryInfo(HLERequestContext& ctx) {
    // According to SwitchBrew, this has no inputs and no outputs, so effectively does nothing on
    // retail hardware.
    LOG_DEBUG(Service_NVDRV, "called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

NVDRV::NVDRV(Core::System& system_, std::shared_ptr<Module> nvdrv_, const char* name)
    : ServiceFramework{system_, name}, nvdrv{std::move(nvdrv_)} {
    static const FunctionInfo functions[] = {
        {0, &NVDRV::Open, "Open"},
        {1, &NVDRV::Ioctl1, "Ioctl"},
        {2, &NVDRV::Close, "Close"},
        {3, &NVDRV::Initialize, "Initialize"},
        {4, &NVDRV::QueryEvent, "QueryEvent"},
        {5, nullptr, "MapSharedMem"},
        {6, &NVDRV::GetStatus, "GetStatus"},
        {7, nullptr, "SetAruidForTest"},
        {8, &NVDRV::SetAruid, "SetAruid"},
        {9, &NVDRV::DumpGraphicsMemoryInfo, "DumpGraphicsMemoryInfo"},
        {10, nullptr, "InitializeDevtools"},
        {11, &NVDRV::Ioctl2, "Ioctl2"},
        {12, &NVDRV::Ioctl3, "Ioctl3"},
        {13, &NVDRV::SetGraphicsFirmwareMemoryMarginEnabled,
         "SetGraphicsFirmwareMemoryMarginEnabled"},
    };
    RegisterHandlers(functions);
}

NVDRV::~NVDRV() {
    // Unregister GPU callbacks and drain timer callbacks before releasing session resources.
    deferred_ioctls.clear();
    if (is_initialized) {
        auto& container = nvdrv->GetContainer();
        container.CloseSession(session_id);
    }
}

} // namespace Service::Nvidia
