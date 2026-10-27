#include "profile_service.h"
#include <nxemu-module-spec/operating_system.h>
#include <nxemu-module-spec/profile_storage.h>
#include <yuzu_common/fs/path_util.h>
#include <yuzu_common/uuid.h>
#include <filesystem>
#include <cstring>
#include <algorithm>
#include <utility>

static_assert(sizeof(HostProfileId) == HOST_PROFILE_UUID_SIZE);

void HostProfileService::Attach(IOperatingSystem & os)
{
    m_os = &os;
    Refresh();
}

void HostProfileService::Detach() noexcept
{
    // The cached snapshot remains available while the modules are reloaded.
    // Mutations are disabled until the new OS instance is attached.
    m_os = nullptr;
}

bool HostProfileService::RefreshOffline()
{
    // Never replace an attached OS snapshot with potentially stale disk data.
    if (m_os != nullptr)
    {
        return false;
    }
    const auto nand_dir = Common::FS::GetYuzuPath(Common::FS::YuzuPath::NANDDir);
    const auto raw = HostProfileStorage::Read(HostProfileStorage::FilePath(nand_dir));
    if (!raw)
    {
        return false;
    }
    std::vector<UserProfile> updated;
    updated.reserve(HostProfileStorage::MaxUsers);
    const auto avatar_dir = nand_dir / "system/save/8000000000000010/su/avators";
    for (const auto & user : raw->users)
    {
        const Common::UUID uuid{user.uuid};
        if (uuid.IsInvalid())
        {
            continue;
        }
        UserProfile profile{};
        profile.uuid = user.uuid;
        const auto end = std::find(user.username.begin(), user.username.end(), 0);
        profile.name.assign(user.username.begin(), end);
        const auto avatar = avatar_dir / (uuid.FormattedString() + ".jpg");
        std::error_code ec;
        if (std::filesystem::is_regular_file(avatar, ec) && !ec)
        {
            profile.picture = Common::FS::PathToUTF8String(avatar);
        }
        updated.push_back(std::move(profile));
    }
    m_profiles.swap(updated);
    return true;
}

void HostProfileService::Refresh()
{
    if (m_os == nullptr)
    {
        return;
    }
    std::vector<UserProfile> updated;
    const uint32_t count = m_os->GetProfileCount();
    updated.reserve(count);
    for (uint32_t index = 0; index < count; ++index)
    {
        HostProfileInfo info{};
        if (!m_os->GetProfile(index, &info))
        {
            continue;
        }
        UserProfile profile{};
        std::memcpy(profile.uuid.data(), info.uuid, HOST_PROFILE_UUID_SIZE);
        profile.name = info.username;
        char path[1024]{};
        if (m_os->GetProfileImagePath(info.uuid, path, sizeof(path)))
        {
            profile.picture = path;
        }
        updated.push_back(std::move(profile));
    }
    m_profiles.swap(updated);
}

bool HostProfileService::Create(const HostProfileId & uuid, const char * name)
{
    if (m_os == nullptr || !m_os->CreateProfile(uuid.data(), name, nullptr))
    {
        return false;
    }
    Refresh();
    return true;
}

bool HostProfileService::Rename(const HostProfileId & uuid, const char * name)
{
    if (m_os == nullptr || !m_os->RenameProfile(uuid.data(), name))
    {
        return false;
    }
    Refresh();
    return true;
}

bool HostProfileService::Remove(const HostProfileId & uuid)
{
    if (m_os == nullptr || !m_os->RemoveProfile(uuid.data()))
    {
        return false;
    }
    Refresh();
    return true;
}

bool HostProfileService::SetImage(const HostProfileId & uuid, const uint8_t * data, uint32_t size)
{
    if (m_os == nullptr || !m_os->SetProfileImage(uuid.data(), data, size))
    {
        return false;
    }
    Refresh();
    return true;
}
