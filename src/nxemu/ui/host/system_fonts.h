#pragma once
#include <filesystem>
#include <vector>

struct SystemFont
{
    std::filesystem::path path;
    int faceIndex = 0;
};

std::vector<SystemFont> SystemFallbackFonts();
