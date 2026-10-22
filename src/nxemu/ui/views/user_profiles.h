#pragma once
#include <nxemu-module-spec/operating_system.h>
#include <string>
#include <vector>

// A user of the emulated console. Each has saves of its own; games are given
// the name and the picture of the one they are started as.
struct UserProfile
{
    HostProfileInfo info;
    std::string name;
    std::string picture; // the file of its picture, empty when it has none
};

namespace UserProfiles
{
// The users of the console, in its order.
std::vector<UserProfile> List(IOperatingSystem & os);
// The one of count games start as, by its place in the list.
int Active(size_t count);
void SetActive(int index);
} // namespace UserProfiles
