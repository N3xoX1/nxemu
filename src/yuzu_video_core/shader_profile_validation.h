// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_shader_recompiler/shader_info.h"
#include <array>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <string_view>
#include "yuzu_common/cityhash.h"
#include "yuzu_video_core/shader_profile_resources.h"
#include "yuzu_video_core/shader_profile_text.h"

namespace VideoCommon::ShaderProfile
{
inline bool ValidResourceName(std::string_view name) {
    return !name.empty() && name != "." && name != ".." &&
        name.find_first_of("/\\:\0", 0, 4) == std::string_view::npos;
}
// A file directly inside root. The name has no separator, so it cannot leave
// root; a symbolic link the user put there (a game kept on another disk) is
// followed. A missing file throws no_such_file_or_directory.
inline std::filesystem::path LocalResourcePath(const std::filesystem::path& root, std::string_view name) {
    if (!ValidResourceName(name)) throw std::runtime_error("Invalid local resource name");
    auto path = root / std::filesystem::u8path(name);
    const auto status = std::filesystem::status(path);
    if (!std::filesystem::exists(status))
        throw std::filesystem::filesystem_error("Local resource not found", path,
            std::make_error_code(std::errc::no_such_file_or_directory));
    if (!std::filesystem::is_regular_file(status)) throw std::runtime_error("Local resource is not a file");
    return path;
}
// The hashes the emulator can give these program bytes: Analyze hashes up to the
// terminal instruction, CFG hashes the full span. Zero marks an absent variant.
inline std::array<u64, 2> ProgramHashCandidates(std::string_view code) {
    std::array<u64, 2> result{Common::CityHash64(code.data(), code.size()), 0};
    if (code.size() >= 8) {
        u64 word{};
        for (size_t i=0;i<8;++i) word |= u64(static_cast<unsigned char>(code[code.size()-8+i])) << (8*i);
        if (word == 0xE2400FFFFF87000FULL || word == 0xE2400FFFFF07000FULL)
            result[1] = Common::CityHash64(code.data(), code.size()-8);
    }
    return result;
}
inline bool MatchesProgramHash(std::string_view code, u64 hash) {
    if (!hash) return false;
    const auto candidates = ProgramHashCandidates(code);
    return candidates[0] == hash || candidates[1] == hash;
}
inline bool ValidEnvironmentMapValue(size_t map, u32 value)
{
    if (map == 0) return value < Shader::NUM_TEXTURE_TYPES;
    if (map == 1) return value <= static_cast<u32>(Shader::TexturePixelFormat::D32_FLOAT_S8_UINT);
    if (map == 3) return value <= static_cast<u32>(Shader::ReplaceConstant::DrawID);
    return true;
}
} // namespace VideoCommon::ShaderProfile
