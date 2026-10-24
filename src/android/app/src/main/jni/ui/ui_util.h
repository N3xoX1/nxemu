#pragma once

#include <common/json.h>
#include <string>
#include <string_view>
#include <vector>

namespace UiUtil
{
std::string Escape(std::string_view text);
std::string UrlDecode(std::string_view text);
std::string FormatGameFolder(const std::string & uri);
JsonValue ParseJson(const std::string & text);
std::vector<std::string> StringArray(const JsonValue & value);

bool ReadBool(const char * key);
int ReadInt(const char * key);
std::string ReadString(const char * key);
void SetBool(const char * key, bool value);
void SetInt(const char * key, int value);
void SetString(const char * key, const std::string & value);
void SaveSettings();
std::vector<std::string> ConfiguredFolders();
} // namespace UiUtil
