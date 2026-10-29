#pragma once
#include <filesystem>
#include <string>

namespace InterfaceLanguage
{
std::string Choice();
std::string Code();
void SetChoice(const std::string & code);
void Use(const std::string & code, const std::filesystem::path & assetDir);
void AdoptSystemLanguage();
} // namespace InterfaceLanguage
