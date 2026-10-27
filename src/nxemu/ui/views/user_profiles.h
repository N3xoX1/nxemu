#pragma once
#include <nxemu/ui/host/profile_service.h>
#include <cstddef>

namespace UserProfiles
{
int Active(size_t count);
void SetActive(int index);
} // namespace UserProfiles
