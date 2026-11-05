// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string_view>
namespace Shader::ScaleTrace
{
inline thread_local std::uint64_t shader_hash{};
inline const char * Directory()
{
    static const char * path = std::getenv("NXEMU_SCALE_TRACE");
    return path;
}
inline std::filesystem::path File(std::uint64_t hash, std::string_view suffix)
{
    std::ostringstream name;
    name << "fragment-" << std::hex << hash << suffix;
    return std::filesystem::path(Directory()) / name.str();
}
inline void Text(std::uint64_t hash, std::string_view suffix, std::string_view text)
{
    if (!Directory()) return;
    std::ofstream(File(hash, suffix)) << text;
}
inline void Spirv(std::uint64_t hash, std::span<const std::uint32_t> code)
{
    if (!Directory()) return;
    std::ofstream file(File(hash, ".spv"), std::ios::binary);
    file.write(reinterpret_cast<const char *>(code.data()), code.size_bytes());
}
} // namespace Shader::ScaleTrace
