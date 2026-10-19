#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace GameCache
{
std::filesystem::path Directory(const std::filesystem::path & root, uint64_t programId);
bool Clear(const std::filesystem::path & root, uint64_t programId, std::string & error);
} // namespace GameCache
