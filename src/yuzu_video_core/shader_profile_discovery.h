// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_video_core/shader_profile_export.h"
#include <map>
namespace VideoCommon::ShaderProfile
{
// Offline only: select the local base/update whose program headers match the
// recorded context. Never guess from a display version or the newest filename.
void ResolveRecordedSources(ExportOptions & options);
// Offline plaintext-container backend. Virtual BKTR ranges are resolved to
// verified physical spans; GPU addresses are never used as file offsets.
void Discover(const ExportOptions & options, JsonValue & profile,
              const std::map<std::string, std::string> & codes, ExportReport & report);
} // namespace VideoCommon::ShaderProfile
