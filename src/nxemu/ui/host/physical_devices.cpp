#include "physical_devices.h"
#include <SDL.h>
#include <array>

namespace
{
struct ScopedJoystickInit final
{
    ScopedJoystickInit() : acquired(SDL_InitSubSystem(SDL_INIT_JOYSTICK) == 0) {}
    ~ScopedJoystickInit()
    {
        if (acquired) { SDL_QuitSubSystem(SDL_INIT_JOYSTICK); }
    }
    ScopedJoystickInit(const ScopedJoystickInit &) = delete;
    ScopedJoystickInit & operator=(const ScopedJoystickInit &) = delete;
    bool acquired;
};
// Device indices can move when controllers are unplugged. Keep the SDL
// joystick list fixed while collecting a single snapshot.
struct ScopedJoystickLock final
{
    ScopedJoystickLock() { SDL_LockJoysticks(); }
    ~ScopedJoystickLock() { SDL_UnlockJoysticks(); }
    ScopedJoystickLock(const ScopedJoystickLock &) = delete;
    ScopedJoystickLock & operator=(const ScopedJoystickLock &) = delete;
};
} // namespace

std::optional<std::vector<HostPhysicalDevice>> EnumerateHostPhysicalDevices()
{
    // Balanced SDL reference counting: the OS input backend still manages its
    // own SDL driver and must not inherit a joystick subsystem owned by UI.
    ScopedJoystickInit subsystem;
    if (!subsystem.acquired) { return std::nullopt; }

    ScopedJoystickLock lock;
    const int count = SDL_NumJoysticks();
    if (count < 0) { return std::nullopt; }
    std::vector<HostPhysicalDevice> devices;
    devices.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index)
    {
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
        if (id < 0) { continue; } // Device removed during enumeration.
        const char * name = SDL_JoystickNameForIndex(index);
        std::array<char, 33> guid{};
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid.data(),
                                  static_cast<int>(guid.size()));
        devices.push_back({name != nullptr ? name : "Unknown controller", guid.data(),
                           static_cast<int>(id)});
    }
    return devices;
}
