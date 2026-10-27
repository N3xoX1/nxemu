#pragma once
#include <nxemu-module-spec/operating_system.h>
class HostInputService;
#include <string>

namespace InputText
{
std::string Binding(const HostInputService & inputs, const IParamPackage & params);
std::string StickDirection(const HostInputService & inputs, const IParamPackage & stick, const char * direction);
} // namespace InputText
