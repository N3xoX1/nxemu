#include "ui_renderer.h"

#include <RmlUi/Core/Core.h>

namespace
{
    constexpr const char * kMemoryScheme = "nxmem:";
}

std::string UiRenderer::AddMemoryImage(std::vector<uint8_t> encoded)
{
    std::string source = kMemoryScheme + std::to_string(m_nextMemoryImage++);
    m_memoryImages.emplace(source, std::move(encoded));
    return source;
}

void UiRenderer::RemoveMemoryImage(const std::string & source)
{
    if (m_memoryImages.erase(source) != 0)
    {
        Rml::ReleaseTexture(source);
    }
}
