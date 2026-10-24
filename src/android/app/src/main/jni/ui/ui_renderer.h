#pragma once

#include <RmlUi_Renderer_GL3.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class UiRenderer final : public RenderInterface_GL3
{
public:
    void RemoveMemoryImage(const std::string & source);

private:
    std::unordered_map<std::string, std::vector<uint8_t>> m_memoryImages;
};
