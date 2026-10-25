#pragma once

#include <RmlUi_Renderer_GL3.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class UiRenderer final : public RenderInterface_GL3
{
public:
    std::string AddMemoryImage(std::vector<uint8_t> encoded);
    void RemoveMemoryImage(const std::string & source);

    Rml::TextureHandle LoadTexture(Rml::Vector2i & dimensions, const Rml::String & source) override;

private:
    std::unordered_map<std::string, std::vector<uint8_t>> m_memoryImages;
    uint64_t m_nextMemoryImage = 0;
};
