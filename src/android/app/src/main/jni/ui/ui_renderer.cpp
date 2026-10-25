#include "ui_renderer.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FileInterface.h>
#include <stb_image.h>

namespace
{
    constexpr const char * kMemoryScheme = "nxmem:";

    std::vector<uint8_t> ReadFile(const std::string & source)
    {
        std::vector<uint8_t> data;
        Rml::FileInterface * files = Rml::GetFileInterface();
        const Rml::FileHandle file = files->Open(source);
        if (!file)
        {
            return data;
        }
        data.resize(files->Length(file));
        data.resize(files->Read(data.data(), data.size(), file));
        files->Close(file);
        return data;
    }
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

Rml::TextureHandle UiRenderer::LoadTexture(Rml::Vector2i & dimensions, const Rml::String & source)
{
    std::vector<uint8_t> file;
    const std::vector<uint8_t> * encoded = &file;
    if (const auto itr = m_memoryImages.find(source); itr != m_memoryImages.end())
    {
        encoded = &itr->second;
    }
    else
    {
        file = ReadFile(source);
    }
    if (encoded->empty())
    {
        return {};
    }

    int width = 0, height = 0, channels = 0;
    stbi_uc * pixels = stbi_load_from_memory(encoded->data(), static_cast<int>(encoded->size()), &width, &height, &channels, 4);
    if (pixels == nullptr)
    {
        return {};
    }

    const size_t size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    for (size_t i = 0; i < size; i += 4)
    {
        const unsigned alpha = pixels[i + 3];
        for (size_t c = 0; c < 3; c++)
        {
            pixels[i + c] = static_cast<stbi_uc>(pixels[i + c] * alpha / 255);
        }
    }
    dimensions = {width, height};
    const Rml::TextureHandle texture = GenerateTexture({reinterpret_cast<const Rml::byte *>(pixels), size}, dimensions);
    stbi_image_free(pixels);
    return texture;
}
