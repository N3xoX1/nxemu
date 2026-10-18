#pragma once
#include <nxemu-module-spec/operating_system.h>
#include <string>

namespace InputText
{
std::string Binding(const IOperatingSystem & os, const IParamPackage & params);
std::string StickDirection(const IOperatingSystem & os, const IParamPackage & stick, const char * direction);
} // namespace InputText
