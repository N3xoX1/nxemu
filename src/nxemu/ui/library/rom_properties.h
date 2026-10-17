#pragma once
#include <cstdint>
#include <nxemu-module-spec/base.h>
#include <string>
#include <vector>

nxinterface IRomInfo;

// What a game file says about itself. Images come encoded, as the file holds
// them, and are empty when it has none.
namespace RomProperties
{
std::string Title(IRomInfo & info);
// The base release is kept separately so disabling an update restores the
// version from the game's own control data.
void Release(IRomInfo & info, std::string & version, std::string & publisher, std::string & baseVersion);
std::string ActiveUpdateVersion(IRomInfo & info);
std::vector<uint8_t> Icon(IRomInfo & info);
// Shown in the corners of the loading screen.
std::vector<uint8_t> Logo(IRomInfo & info);
std::vector<uint8_t> Banner(IRomInfo & info);
} // namespace RomProperties
