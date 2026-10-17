#pragma once
#include <cstdint>
#include <nxemu-module-spec/base.h>
#include <string>
#include <vector>

nxinterface IRomInfo;

namespace RomProperties
{
std::string Title(IRomInfo & info);
std::string Version(IRomInfo & info);
std::vector<uint8_t> Icon(IRomInfo & info);
} // namespace RomProperties
