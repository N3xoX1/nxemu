// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: 2021 Skyline Team and Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

#include "yuzu_common/common_types.h"
#include "yuzu_common/nvdata.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/devices/nvdevice.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Kernel {
class KEvent;
}

namespace Service::Nvidia {

namespace NvCore {
class Container;
class SyncpointManager;
} // namespace NvCore

namespace Devices {
class nvdevice;
class nvhost_ctrl;
} // namespace Devices

class Module;

class EventInterface {
public:
    explicit EventInterface(Module& module_);
    ~EventInterface();

    Kernel::KEvent* CreateEvent(std::string name);

    void FreeEvent(Kernel::KEvent* event);

private:
    Module& module;
    std::mutex guard;
    std::list<Devices::nvhost_ctrl*> on_signal;
};

class Module final {
public:
    explicit Module(Core::System& system_, Kernel::KEvent* deferral_event_);
    ~Module();

    /// Returns a pointer to one of the available devices, identified by its name.
    template <typename T>
    std::shared_ptr<T> GetDevice(DeviceFD fd) {
        auto itr = open_files.find(fd);
        if (itr == open_files.end())
            return nullptr;
        return std::static_pointer_cast<T>(itr->second);
    }

    NvResult VerifyFD(DeviceFD fd) const;

    std::optional<Devices::SyncpointWaitParams> GetSyncpointWait(
        DeviceFD fd, Ioctl command, std::span<const u8> input) const;

    Kernel::KEvent& GetDeferralEvent() const {
        return *deferral_event;
    }

    /// Opens a device node and returns a file descriptor to it.
    DeviceFD Open(const std::string& device_name, NvCore::SessionId session_id);

    /// Sends an ioctl command to the specified file descriptor.
    NvResult Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output);

    NvResult Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                    std::span<const u8> inline_input, std::span<u8> output);

    NvResult Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output,
                    std::span<u8> inline_output);

    /// Closes a device file descriptor and returns operation success.
    NvResult Close(DeviceFD fd);

    // The video registry keeps GMMUs alive until shutdown, including after their AS FD closes.
    void RetainNvMapPinUntilShutdown(u32 handle, std::shared_ptr<void> pin);

    NvResult QueryEvent(DeviceFD fd, u32 event_id, Kernel::KEvent*& event);

    NvCore::Container& GetContainer() {
        return container;
    }

private:
    friend class EventInterface;

    /// Manages syncpoints on the host
    NvCore::Container container;

    /// Id to use for the next open file descriptor.
    DeviceFD next_fd = 1;

    using FilesContainerType = std::unordered_map<DeviceFD, std::shared_ptr<Devices::nvdevice>>;
    /// Mapping of file descriptors to the devices they reference.
    FilesContainerType open_files;
    std::unordered_map<u32, std::shared_ptr<void>> retained_nvmap_pins;

    KernelHelpers::ServiceContext service_context;

    EventInterface events_interface;
    // Owns the writable reference returned by ServerManager::ManageDeferral.
    Kernel::KEvent* deferral_event;

    std::unordered_map<std::string, std::function<FilesContainerType::iterator(DeviceFD)>> builders;
};

void LoopProcess(Core::System& system);

} // namespace Service::Nvidia
