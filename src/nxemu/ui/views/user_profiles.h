#pragma once
#include <nxemu-module-spec/operating_system.h>
#include <string>
#include <vector>

struct UserProfile
{
    HostProfileInfo info;
    std::string name;
    std::string picture;
};

namespace UserProfiles
{
std::vector<UserProfile> List(IOperatingSystem & os);
int Active(size_t count);
void SetActive(int index);
} // namespace UserProfiles
