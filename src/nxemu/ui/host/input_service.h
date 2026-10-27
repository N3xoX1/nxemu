#pragma once
#include <nxemu/ui/input/input_params.h>
#include "physical_devices.h"
#include <array>
#include <common/json.h>
#include <optional>
#include <vector>
#include <cstdint>

// Owned, fully copied mapping parameters. No module-owned package or list is
// kept by the frontend after a call returns.
struct HostDeviceMappings
{
    std::array<InputParams, static_cast<size_t>(NativeButtonValues::NumButtons)> buttons{};
    std::array<InputParams, static_cast<size_t>(NativeAnalogValues::NumAnalogs)> sticks{};
    std::array<InputParams, static_cast<size_t>(NativeMotionValues::NumMotions)> motions{};
};

// These are the three groups in the existing controller configuration schema.
enum class HostBindingGroup : uint8_t { Button, Analog, Motion };

// Frontend-owned lifecycle boundary for host input discovery and mapping.
// One owner (EmulatorSession), accessed on its frontend thread. The emulated
// OS remains the backend until InputSubsystem can be separated from CoreSystem.
// No OS-owned parameter package or device list escapes this service.
class HostInputService final
{
public:
    HostInputService() = default;
    ~HostInputService() { Detach(); }
    HostInputService(const HostInputService &) = delete;
    HostInputService & operator=(const HostInputService &) = delete;

    void Attach(IOperatingSystem & os);
    void Detach() noexcept;
    bool Available() const noexcept { return m_os != nullptr; }

    // Retain the last successful snapshot during module reloads.
    bool RefreshDevices();
    const std::vector<InputParams> & Devices() const noexcept { return m_devices; }
    // Read-only physical enumeration independent of the emulated HID backend.
    // On failure the previous snapshot is left intact.
    bool RefreshPhysicalDevices();
    const std::vector<HostPhysicalDevice> & PhysicalDevices() const noexcept { return m_physicalDevices; }
    ButtonNames ButtonName(const IParamPackage & params) const;

    bool BeginMapping(PollingInputType type);
    void StopMapping() noexcept;
    InputParams NextInput() const;

    // Read-only snapshots. Called only on the frontend thread, while the OS is
    // attached. They do not cache OS-owned handles across a module reset.
    std::optional<HostDeviceMappings> ReadDeviceMappings(const InputParams & device) const;
    std::vector<InputParams> MappedDevices(NpadIdType player) const;

    // Read the persisted *overrides* even before the guest OS exists.
    // Missing fields mean "use the OS default", not an empty mapping.
    JsonValue StoredPlayerOverrides(uint32_t player) const;
    std::optional<std::string> StoredBinding(uint32_t player, HostBindingGroup group,
                                             uint32_t index) const;

    // Only possible while detached. The OS module remains the sole writer
    // during a session. Existing nxemu-os keys and unrelated fields survive.
    // A true return means updated in SettingsStore; disk I/O is best effort.
    bool SetStoredBindingOffline(uint32_t player, HostBindingGroup group,
                                 uint32_t index, const std::string & serialized);

    // Commit all changed players as one SettingsStore update. Existing OS
    // settings and unmodified players are not rewritten.
    bool SaveOfflinePlayers(const std::array<JsonValue, 8> & players,
                            const std::array<bool, 8> & changed);
    static const char * BindingKey(HostBindingGroup group, uint32_t index);

private:
    IOperatingSystem * m_os = nullptr; // Borrowed only between Attach/Detach
    std::vector<InputParams> m_devices;
    std::vector<HostPhysicalDevice> m_physicalDevices;
    bool m_mapping = false;
};
