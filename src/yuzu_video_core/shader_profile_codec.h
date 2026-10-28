// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <string_view>
namespace VideoCommon::ShaderProfile {
// Bounded compression envelope; the runtime separately requires NXSHBIN1.
// Unpack also accepts raw bytes for offline diagnostic tools.
std::string Pack(std::string_view json, int level = 3);
std::string Unpack(std::string_view bytes);
// One record of an append-only local file. PackFrame returns an empty string when
// compression would not save space; the caller then stores the bytes as they are.
// UnpackFrame is false unless bytes are exactly one frame of at most max_size bytes.
std::string PackFrame(std::string_view raw, int level = 1);
bool UnpackFrame(std::string_view bytes, size_t max_size, std::string& out);
} // namespace VideoCommon::ShaderProfile
