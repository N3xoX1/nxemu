// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>

#include "yuzu_common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/ptm/psm.h"

namespace Service::PTM {

class IPsmSession final : public ServiceFramework<IPsmSession> {
public:
    explicit IPsmSession(Core::System& system_)
        : ServiceFramework{system_, "IPsmSession"}, service_context{system_, "IPsmSession"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IPsmSession::BindStateChangeEvent, "BindStateChangeEvent"},
            {1, &IPsmSession::UnbindStateChangeEvent, "UnbindStateChangeEvent"},
            {2, &IPsmSession::SetChargerTypeChangeEventEnabled, "SetChargerTypeChangeEventEnabled"},
            {3, &IPsmSession::SetPowerSupplyChangeEventEnabled, "SetPowerSupplyChangeEventEnabled"},
            {4, &IPsmSession::SetBatteryVoltageStateChangeEventEnabled, "SetBatteryVoltageStateChangeEventEnabled"},
        };
        // clang-format on

        RegisterHandlers(functions);

        state_change_event = service_context.CreateEvent("IPsmSession::state_change_event");
    }

    ~IPsmSession() override {
        service_context.CloseEvent(state_change_event);
    }

    void SignalChargerTypeChanged() {
        if (should_signal && should_signal_charger_type) {
            state_change_event->Signal();
        }
    }

    void SignalPowerSupplyChanged() {
        if (should_signal && should_signal_power_supply) {
            state_change_event->Signal();
        }
    }

    void SignalBatteryVoltageStateChanged() {
        if (should_signal && should_signal_battery_voltage) {
            state_change_event->Signal();
        }
    }

private:
    void BindStateChangeEvent(HLERequestContext& ctx) {
        LOG_DEBUG(Service_PTM, "called");

        should_signal = true;

        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(state_change_event->GetReadableEvent());
    }

    void UnbindStateChangeEvent(HLERequestContext& ctx) {
        LOG_DEBUG(Service_PTM, "called");

        should_signal = false;

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetChargerTypeChangeEventEnabled(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto state = rp.Pop<bool>();
        LOG_DEBUG(Service_PTM, "called, state={}", state);

        should_signal_charger_type = state;

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetPowerSupplyChangeEventEnabled(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto state = rp.Pop<bool>();
        LOG_DEBUG(Service_PTM, "called, state={}", state);

        should_signal_power_supply = state;

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetBatteryVoltageStateChangeEventEnabled(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto state = rp.Pop<bool>();
        LOG_DEBUG(Service_PTM, "called, state={}", state);

        should_signal_battery_voltage = state;

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    KernelHelpers::ServiceContext service_context;

    bool should_signal_charger_type{};
    bool should_signal_power_supply{};
    bool should_signal_battery_voltage{};
    bool should_signal{};
    Kernel::KEvent* state_change_event;
};

PSM::PSM(Core::System& system_) : ServiceFramework{system_, "psm"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &PSM::GetBatteryChargePercentage, "GetBatteryChargePercentage"},
        {1, &PSM::GetChargerType, "GetChargerType"},
        {2, nullptr, "EnableBatteryCharging"},
        {3, nullptr, "DisableBatteryCharging"},
        {4, nullptr, "IsBatteryChargingEnabled"},
        {5, nullptr, "AcquireControllerPowerSupply"},
        {6, nullptr, "ReleaseControllerPowerSupply"},
        {7, &PSM::OpenSession, "OpenSession"},
        {8, nullptr, "EnableEnoughPowerChargeEmulation"},
        {9, nullptr, "DisableEnoughPowerChargeEmulation"},
        {10, nullptr, "EnableFastBatteryCharging"},
        {11, nullptr, "DisableFastBatteryCharging"},
        {12, &PSM::GetBatteryVoltageState, "GetBatteryVoltageState"},
        {13, nullptr, "GetRawBatteryChargePercentage"},
        {14, nullptr, "IsEnoughPowerSupplied"},
        {15, &PSM::GetBatteryAgePercentage, "GetBatteryAgePercentage"},
        {16, nullptr, "GetBatteryChargeInfoEvent"},
        {17, &PSM::GetBatteryChargeInfoFields, "GetBatteryChargeInfoFields"},
        {18, nullptr, "GetBatteryChargeCalibratedEvent"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

PSM::~PSM() = default;

void PSM::GetBatteryChargePercentage(HLERequestContext& ctx) {
    LOG_DEBUG(Service_PTM, "called");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(battery_charge_percentage);
}

void PSM::GetChargerType(HLERequestContext& ctx) {
    LOG_DEBUG(Service_PTM, "called");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushEnum(charger_type);
}

void PSM::OpenSession(HLERequestContext& ctx) {
    LOG_DEBUG(Service_PTM, "called");

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IPsmSession>(system);
}

void PSM::GetBatteryVoltageState(HLERequestContext& ctx) {
    LOG_DEBUG(Service_PTM, "called");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushEnum(BatteryVoltageState::Normal);
}

void PSM::GetBatteryAgePercentage(HLERequestContext& ctx) {
    LOG_DEBUG(Service_PTM, "called");

    // The ABI does not specify this scalar's scale; retain the existing compatibility value.
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.PushRaw<f64>(1.0);
}

struct BatteryChargeInfoFields {
    u32 input_current_limit;
    u32 boost_mode_current_limit;
    u32 fast_charge_current_limit;
    u32 charge_voltage_limit;
    u32 charger_type;
    u8 hi_z_mode;
    u8 battery_charging;
    u8 padding_16[2];
    u32 vdd50_state;
    u32 temperature_celcius;
    u32 battery_charge_percentage;
    u32 battery_charge_milli_voltage;
    u32 battery_age_percentage;
    u32 charger_input_voltage_limit_mirror;
    u32 usb_power_role;
    u32 usb_charger_type;
    u32 charger_input_voltage_limit;
    u32 charger_input_current_limit;
    u8 fast_battery_charging;
    u8 controller_power_supply;
    u8 otg_request;
    u8 reserved;
    u8 unknown_44[0x10];
};
static_assert(sizeof(BatteryChargeInfoFields) == 0x54);

void PSM::GetBatteryChargeInfoFields(HLERequestContext& ctx) {
    LOG_DEBUG(Service_PTM, "called");

    // This is the 17.0.0+ ABI. Percentages in this structure are expressed in
    // per-cent-mille (100% == 100000), unlike GetBatteryChargePercentage().
    constexpr u32 FullPercentagePcm = 100000;
    constexpr u32 NominalBatteryVoltageMv = 4200;
    constexpr u32 ChargerInputVoltageMv = 5000;
    constexpr u32 ChargerInputCurrentMa = 3000;

    BatteryChargeInfoFields fields{};
    fields.input_current_limit = ChargerInputCurrentMa;
    fields.fast_charge_current_limit = ChargerInputCurrentMa;
    fields.charge_voltage_limit = NominalBatteryVoltageMv;
    fields.charger_type = static_cast<u32>(charger_type);
    fields.temperature_celcius = 25000;
    fields.battery_charge_percentage = FullPercentagePcm;
    fields.battery_charge_milli_voltage = NominalBatteryVoltageMv;
    fields.battery_age_percentage = FullPercentagePcm;
    fields.charger_input_voltage_limit_mirror = ChargerInputVoltageMv;
    fields.charger_input_voltage_limit = ChargerInputVoltageMv;
    fields.charger_input_current_limit = ChargerInputCurrentMa;

    IPC::ResponseBuilder rb{
        ctx, 2 + static_cast<u32>(sizeof(BatteryChargeInfoFields) / sizeof(u32))};
    rb.Push(ResultSuccess);
    rb.PushRaw(fields);
}

} // namespace Service::PTM
