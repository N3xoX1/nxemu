#pragma once
#include <nxemu-module-spec/operating_system.h>
#include <string>

// How the interface names what an input is bound to.
namespace InputText
{
// The key, the button or the axis a button is bound to; empty when it is
// bound to nothing.
std::string Binding(const IOperatingSystem & os, const IParamPackage & params);
// The same for one direction of a stick: "up", "down", "left" or "right".
std::string StickDirection(const IOperatingSystem & os, const IParamPackage & stick, const char * direction);
} // namespace InputText
