#pragma once
#include <filesystem>
#include <vector>

struct SystemFont
{
    std::filesystem::path path;
    int faceIndex = 0;
};

// Installed fonts for Cyrillic, Chinese, Japanese and Korean. Files stay on
// the host; no system font is copied into the application's resources.
std::vector<SystemFont> SystemFallbackFonts();
