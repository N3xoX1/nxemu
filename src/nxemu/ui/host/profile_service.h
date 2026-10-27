#pragma once
#include <nxemu-module-spec/base.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

nxinterface IOperatingSystem;

// Frontend-only data model: no guest OS types or module-owned objects escape.
using HostProfileId = std::array<uint8_t, 16>;

struct UserProfile
{
    HostProfileId uuid{};
    std::string name;
    std::string picture;
};

// Owned by EmulatorSession and always accessed on the frontend thread.
// The OS module remains the single storage backend until the repository is
// extracted; a detached service is read-only, with a stable cached snapshot.
class HostProfileService final
{
public:
    HostProfileService() = default;
    HostProfileService(const HostProfileService &) = delete;
    HostProfileService & operator=(const HostProfileService &) = delete;

    void Attach(IOperatingSystem & os);
    void Detach() noexcept;
    bool Available() const noexcept { return m_os != nullptr; }
    // Read profiles without creating or loading the guest OS module. Read-only.
    bool RefreshOffline();
    void Refresh();
    const std::vector<UserProfile> & List() const noexcept { return m_profiles; }

    bool Create(const HostProfileId & uuid, const char * name);
    bool Rename(const HostProfileId & uuid, const char * name);
    bool Remove(const HostProfileId & uuid);
    bool SetImage(const HostProfileId & uuid, const uint8_t * data, uint32_t size);

private:
    IOperatingSystem * m_os = nullptr; // borrowed strictly between Attach and Detach
    std::vector<UserProfile> m_profiles;
};
