#include "input_service.h"
#include <nxemu-module-spec/operating_system.h>
#include <memory>
#include <utility>

namespace
{
struct ModuleListRelease
{
    void operator()(IButtonMappingList * list) const noexcept
    {
        if (list != nullptr) { list->Release(); }
    }
    void operator()(IParamPackageList * list) const noexcept
    {
        if (list != nullptr) { list->Release(); }
    }
};

template <size_t N>
bool CopyMappings(IButtonMappingList * raw, std::array<InputParams, N> & destination)
{
    const std::unique_ptr<IButtonMappingList, ModuleListRelease> list(raw);
    if (!list) { return false; }
    for (uint32_t i = 0; i < list->GetCount(); ++i)
    {
        const uint32_t index = list->GetIndex(i);
        if (index < N)
        {
            destination[index] = InputParams::Copy(list->GetParamPackage(i));
        }
    }
    return true;
}
} // namespace

void HostInputService::Attach(IOperatingSystem & os)
{
    Detach();
    m_os = &os;
    RefreshDevices();
}

void HostInputService::Detach() noexcept
{
    StopMapping();
    m_os = nullptr;
    // The last device list is a host-owned copy, never a module-owned pointer.
}

bool HostInputService::RefreshPhysicalDevices()
{
    // Do not initialize or hold an input driver across a guest-session reset.
    // Physical device discovery is intentionally independent from mapping.
    try
    {
        auto devices = EnumerateHostPhysicalDevices();
        if (!devices) { return false; }
        m_physicalDevices = std::move(*devices);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool HostInputService::RefreshDevices()
{
    if (m_os == nullptr)
    {
        return false;
    }
    try
    {
        const std::unique_ptr<IParamPackageList, ModuleListRelease> devices(m_os->GetInputDevices());
        if (!devices) { return false; }
        std::vector<InputParams> updated;
        updated.reserve(devices->GetCount());
        for (uint32_t i = 0; i < devices->GetCount(); ++i)
        {
            InputParams device = InputParams::Copy(devices->GetParamPackage(i));
            if (device.Get("engine") != "mouse")
            {
                updated.push_back(std::move(device));
            }
        }
        m_devices.swap(updated);
        return true;
    }
    catch (...)
    {
        return false; // Keep the last valid frontend-owned device snapshot.
    }
}

ButtonNames HostInputService::ButtonName(const IParamPackage & params) const
{
    if (m_os == nullptr) { return ButtonNames::Invalid; }
    try { return m_os->GetButtonName(params); }
    catch (...) { return ButtonNames::Invalid; }
}

bool HostInputService::BeginMapping(PollingInputType type)
{
    if (m_os == nullptr) { return false; }
    StopMapping();
    try
    {
        m_os->BeginMapping(type);
        m_mapping = true;
        return true;
    }
    catch (...)
    {
        // A backend may have begun polling before it reported failure.
        try { m_os->StopMapping(); } catch (...) {}
        m_mapping = false;
        return false;
    }
}

void HostInputService::StopMapping() noexcept
{
    if (m_mapping && m_os != nullptr)
    {
        // Detach must not let backend failures escape into module destruction.
        try { m_os->StopMapping(); } catch (...) {}
    }
    m_mapping = false;
}

InputParams HostInputService::NextInput() const
{
    if (!m_mapping || m_os == nullptr) { return {}; }
    try { return InputParams::Take(m_os->GetNextInput()); }
    catch (...) { return {}; }
}

std::optional<HostDeviceMappings> HostInputService::ReadDeviceMappings(const InputParams & device) const
{
    if (m_os == nullptr) { return std::nullopt; }
    // An exception in a plugin must not escape into the RmlUI event handler,
    // nor cause a partially copied configuration to be applied to the pad.
    try
    {
        HostDeviceMappings result;
        if (!CopyMappings(m_os->GetButtonMappingForDevice(device), result.buttons) ||
            !CopyMappings(m_os->GetAnalogMappingForDevice(device), result.sticks) ||
            !CopyMappings(m_os->GetMotionMappingForDevice(device), result.motions))
        {
            return std::nullopt;
        }
        return result;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::vector<InputParams> HostInputService::MappedDevices(NpadIdType player) const
{
    if (m_os == nullptr) { return {}; }
    try
    {
        const std::unique_ptr<IParamPackageList, ModuleListRelease> list(
            m_os->GetEmulatedController(player).GetMappedDevicesPtr());
        if (!list) { return {}; }
        std::vector<InputParams> result;
        result.reserve(list->GetCount());
        for (uint32_t i = 0; i < list->GetCount(); ++i)
        {
            result.push_back(InputParams::Copy(list->GetParamPackage(i)));
        }
        return result;
    }
    catch (...)
    {
        return {};
    }
}
