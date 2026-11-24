// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_video_core/shader_profile_export.h"
#include <map>
#include <functional>
#include <set>
namespace VideoCommon::ShaderProfile
{
// Offline only: select the local base/update whose program headers match the
// recorded context. Never guess from a display version or the newest filename.
void ResolveRecordedSources(ExportOptions & options);
// Offline plaintext-container backend. Virtual BKTR ranges are resolved to
// verified physical spans; GPU addresses are never used as file offsets.
void Discover(const ExportOptions & options, JsonValue & profile,
              const std::map<std::string, std::string> & codes, ExportReport & report);
// After an import: where the imported file found each program (code digest -> locator, as the
// file stores it), so exporting the version verifies them instead of searching the game again.
// Known programs keep their locator; Discover still checks every one before using it.
void SeedDiscoveryIndex(const std::filesystem::path & index, const std::map<std::string, JsonValue> & references);
// Every shader program of one game version (the base, patched by the update when one is
// given) found from its files alone: no recording (shader_profile_harvest.h). Switch 1 programs
// are keyed by ProgramKey, Switch 2 ones by "nx2:" and the SHA-256 of control and code.
struct HarvestedProgram
{
    std::string code, control, origin, constants;
    u8 platform{}, stage{};
};
struct HarvestReport
{
    std::map<std::string, HarvestedProgram> programs;
    std::set<std::vector<std::string>> groups; // program keys drawn together, in stage order
    u64 files{}, read_bytes{}, decoded_bytes{}, containers{}, occurrences{}, by_content{};
    std::vector<std::string> errors;
};
// leaf, when set, also sees every searched resource (decoded, out of its archives) with its origin.
HarvestReport HarvestPrograms(const ExportOptions & options,
                              const std::function<void(std::string_view, const std::string &)> & leaf = {});
} // namespace VideoCommon::ShaderProfile
