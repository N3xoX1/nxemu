#pragma once
#include <optional>
#include <string>
#include <vector>

// Frontend-only, read-only hardware identity. This is NOT an emulated HID
// mapping: SDL's GUID/instance IDs are not interchangeable with the guest
// input subsystem's serialized controller identifiers.
struct HostPhysicalDevice
{
    std::string name;
    std::string guid;
    int instance_id = -1;
};

// Enumerate devices without opening them or leaving an SDL subsystem owned
// by the frontend. A failed enumeration returns nullopt (not an empty list).
// Call on the frontend/SDL event thread, never from the emulation worker.
std::optional<std::vector<HostPhysicalDevice>> EnumerateHostPhysicalDevices();
