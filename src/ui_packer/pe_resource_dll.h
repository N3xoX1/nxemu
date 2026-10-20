#pragma once
#include <stdint.h>
#include <string>
#include <vector>

struct PeResource
{
    bool typeIsString = false;
    uint32_t typeId = 0;
    std::string typeName;

    bool nameIsString = true;
    uint32_t nameId = 0;
    std::string name;

    uint16_t language = 0x400; // MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT)
    uint32_t codePage = 1252;
    std::vector<uint8_t> data;
};

bool BuildPeResourceDll(const uint8_t * baseDll, size_t baseDllSize, const std::vector<PeResource> & resources, std::vector<uint8_t> & outImage);
