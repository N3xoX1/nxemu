// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "yuzu_common/concepts.h"
#include "core/hle/kernel/k_port.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/result.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Kernel {
class KAutoObject;
class KClientPort;
class KernelCore;
class KPort;
class SessionRequestHandler;
} // namespace Kernel

namespace Service::SM {

class Controller;

/// Interface to "sm:" service
class SM final : public ServiceFramework<SM> {
public:
    explicit SM(ServiceManager& service_manager_, Core::System& system_);
    ~SM() override;

private:
    void Initialize(HLERequestContext& ctx);
    void GetServiceCmif(HLERequestContext& ctx);
    void GetServiceTipc(HLERequestContext& ctx);
    void RegisterServiceCmif(HLERequestContext& ctx);
    void RegisterServiceTipc(HLERequestContext& ctx);
    void UnregisterService(HLERequestContext& ctx);

    Result GetServiceImpl(Kernel::KAutoObject** out_client_session, HLERequestContext& ctx);
    void RegisterServiceImpl(HLERequestContext& ctx, std::string name, s32 max_session_count,
                             bool is_light);

    ServiceManager& service_manager;
    Kernel::KernelCore& kernel;
};

class ServiceManager {
public:
    explicit ServiceManager(Kernel::KernelCore& kernel_);
    ~ServiceManager();

    Result RegisterService(Kernel::KServerPort** out_server_port, std::string name,
                           s32 max_sessions, SessionRequestHandlerFactory handler_factory,
                           bool is_light = false);
    Result UnregisterService(const std::string& name);
    Result GetServicePort(Kernel::KClientPort** out_client_port, const std::string& name);

    template <Common::DerivedFrom<SessionRequestHandler> T>
    std::shared_ptr<T> GetService(const std::string& service_name, bool block = false) const {
        SessionRequestHandlerFactory factory;

        {
            std::unique_lock lk{lock};
            auto service = registered_services.find(service_name);
            if (service == registered_services.end()) {
                if (!block) {
                    LOG_DEBUG(Service, "Can't find service: {}", service_name);
                    return nullptr;
                }
                if (!WaitForRegistration(lk, service_name)) {
                    return nullptr;
                }
                service = registered_services.find(service_name);
            }

            // Copy the factory under the lock, then invoke it outside the lock.
            factory = service->second;
        }

        return std::static_pointer_cast<T>(factory());
    }

    /// Ends the waits of host threads for services that will not register: a blocking
    /// GetService on a host thread then returns nullptr.
    void Close();

    void InvokeControlRequest(HLERequestContext& context);

    void SetDeferralEvent(Kernel::KEvent* deferral_event_) {
        deferral_event = deferral_event_;
    }

private:
    bool WaitForRegistration(std::unique_lock<std::mutex>& lk, const std::string& name) const;

    std::shared_ptr<SM> sm_interface;
    std::unique_ptr<Controller> controller_interface;

    /// Map of registered services, retrieved using GetServicePort.
    mutable std::mutex lock;
    std::unordered_map<std::string, SessionRequestHandlerFactory> registered_services;
    std::unordered_map<std::string, Kernel::KClientPort*> service_ports;
    mutable std::condition_variable registration_cv;
    mutable size_t host_waiters{};
    bool closed{};

    /// Kernel context
    Kernel::KernelCore& kernel;
    Kernel::KEvent* deferral_event{};
};

/// Runs SM services.
void LoopProcess(Core::System& system);

} // namespace Service::SM
