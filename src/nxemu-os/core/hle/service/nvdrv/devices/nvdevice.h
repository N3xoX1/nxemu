// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <vector>

#include "yuzu_common/common_types.h"
#include "yuzu_common/nvdata.h"
#include "core/hle/service/nvdrv/core/container.h"

namespace Core {
class System;
}

namespace Kernel {
class KEvent;
}

namespace Service::Nvidia::Devices {

class nvhost_gpu;

struct SyncpointWaitParams {
    u32 id;
    u32 threshold;
    u32 timeout_ms;
};

/// Represents an abstract nvidia device node. It is to be subclassed by concrete device nodes to
/// implement the ioctl interface.
class nvdevice {
public:
    explicit nvdevice(Core::System& system_) : system{system_} {}
    virtual ~nvdevice() = default;

    // Only nvhost-ctrl recognizes these commands; other devices may reuse their numbers.
    virtual std::optional<SyncpointWaitParams> GetSyncpointWait(
        Ioctl command, std::span<const u8> input) const {
        return std::nullopt;
    }

    // Checked channel access without RTTI (disabled by the Windows build).
    virtual nvhost_gpu* AsGpuChannel() {
        return nullptr;
    }

    /**
     * Handles an ioctl1 request.
     * @param command The ioctl command id.
     * @param input A buffer containing the input data for the ioctl.
     * @param output A buffer where the output data will be written to.
     * @returns The result code of the ioctl.
     */
    virtual NvResult Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input,
                            std::span<u8> output) = 0;

    /**
     * Handles an ioctl2 request.
     * @param command The ioctl command id.
     * @param input A buffer containing the input data for the ioctl.
     * @param inline_input A buffer containing the input data for the ioctl which has been inlined.
     * @param output A buffer where the output data will be written to.
     * @returns The result code of the ioctl.
     */
    virtual NvResult Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                            std::span<const u8> inline_input, std::span<u8> output) = 0;

    /**
     * Handles an ioctl3 request.
     * @param command The ioctl command id.
     * @param input A buffer containing the input data for the ioctl.
     * @param output A buffer where the output data will be written to.
     * @param inline_output A buffer where the inlined output data will be written to.
     * @returns The result code of the ioctl.
     */
    virtual NvResult Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input,
                            std::span<u8> output, std::span<u8> inline_output) = 0;

    /**
     * Called once a device is opened
     * @param fd The device fd
     */
    virtual void OnOpen(NvCore::SessionId session_id, DeviceFD fd) = 0;

    /**
     * Called once a device is closed
     * @param fd The device fd
     */
    virtual void OnClose(DeviceFD fd) = 0;

    virtual Kernel::KEvent* QueryEvent(u32 event_id) {
        return nullptr;
    }

protected:
    Core::System& system;
};

} // namespace Service::Nvidia::Devices
