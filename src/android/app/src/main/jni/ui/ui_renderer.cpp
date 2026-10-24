#include "ui_renderer.h"

#include <RmlUi/Core/Core.h>

void UiRenderer::RemoveMemoryImage(const std::string & source)
{
    if (m_memoryImages.erase(source) != 0)
    {
        Rml::ReleaseTexture(source);
    }
}
