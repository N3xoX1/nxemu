#include "ui_files.h"

#include <android/asset_manager.h>
#include <cstdio>
#include <RmlUi/Core/Log.h>

namespace
{
    constexpr struct
    {
        const char * name;
        const char * light;
        const char * dark;
    } kColours[] = {
        {"bg", "#ffffff", "#121212"},
        {"text", "#111111", "#e8e8e8"},
        {"muted", "#4a555c", "#b4bcc2"},
        {"line", "#80808033", "#80808033"},
        {"card", "#8080801a", "#8080801a"},
        {"band", "#f2f5f7", "#1a2226"},
        {"accent", "#4fc3f7", "#4fc3f7"},
        {"accent-text", "#00344a", "#00344a"},
        {"select", "#7ef6ff", "#7ef6ff"},
        {"select-fill", "#7ef6ff29", "#7ef6ff29"},
        {"done", "#66bb6a", "#66bb6a"},
        {"done-text", "#0b2e10", "#0b2e10"},
        {"knob", "#ffffff", "#ffffff"},
    };

    bool IsStyled(const std::string & path)
    {
        return path.ends_with(".rcss") || path.ends_with(".rml");
    }
}

UiFiles::UiFiles(AAssetManager * assets) :
    m_assets(assets)
{
}

bool UiFiles::SetDark(bool dark)
{
    if (m_dark == dark)
    {
        return false;
    }
    m_dark = dark;
    m_generation++;
    return true;
}

Rml::FileHandle UiFiles::Open(const Rml::String & path)
{
    auto file = std::make_unique<File>();
    if (!ReadWhole(path, file->data))
    {
        return 0;
    }
    if (IsStyled(path))
    {
        file->data = Resolve(file->data);
    }
    return reinterpret_cast<Rml::FileHandle>(file.release());
}

void UiFiles::Close(Rml::FileHandle file)
{
    delete reinterpret_cast<File *>(file);
}

size_t UiFiles::Read(void * buffer, size_t size, Rml::FileHandle handle)
{
    File & file = *reinterpret_cast<File *>(handle);
    const size_t count = std::min(size, file.data.size() - file.position);
    std::copy_n(file.data.data() + file.position, count, static_cast<char *>(buffer));
    file.position += count;
    return count;
}

bool UiFiles::Seek(Rml::FileHandle handle, long offset, int origin)
{
    File & file = *reinterpret_cast<File *>(handle);
    long base = 0;
    if (origin == SEEK_CUR)
    {
        base = static_cast<long>(file.position);
    }
    else if (origin == SEEK_END)
    {
        base = static_cast<long>(file.data.size());
    }
    const long position = base + offset;
    if (position < 0 || position > static_cast<long>(file.data.size()))
    {
        return false;
    }
    file.position = static_cast<size_t>(position);
    return true;
}

size_t UiFiles::Tell(Rml::FileHandle handle)
{
    return reinterpret_cast<File *>(handle)->position;
}

size_t UiFiles::Length(Rml::FileHandle handle)
{
    return reinterpret_cast<File *>(handle)->data.size();
}

bool UiFiles::LoadFile(const Rml::String & path, Rml::String & out_data)
{
    if (!ReadWhole(path, out_data))
    {
        return false;
    }
    if (IsStyled(path))
    {
        out_data = Resolve(out_data);
    }
    return true;
}

bool UiFiles::ReadWhole(const std::string & path, std::string & out) const
{
    if (path.starts_with('/'))
    {
        FILE * file = std::fopen(path.c_str(), "rb");
        if (file == nullptr)
        {
            return false;
        }
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        out.resize(size > 0 ? static_cast<size_t>(size) : 0);
        const size_t read = std::fread(out.data(), 1, out.size(), file);
        std::fclose(file);
        return read == out.size();
    }

    AAsset * asset = AAssetManager_open(m_assets, path.c_str(), AASSET_MODE_BUFFER);
    if (asset == nullptr)
    {
        return false;
    }
    const auto * data = static_cast<const char *>(AAsset_getBuffer(asset));
    const off_t size = AAsset_getLength(asset);
    if (data != nullptr)
    {
        out.assign(data, static_cast<size_t>(size));
    }
    AAsset_close(asset);
    return data != nullptr;
}

std::string UiFiles::Resolve(std::string_view source) const
{
    std::string resolved;
    resolved.reserve(source.size());
    size_t cursor = 0;
    size_t at;
    while ((at = source.find("var(--", cursor)) != std::string_view::npos)
    {
        const size_t end = source.find(')', at + 6);
        if (end == std::string_view::npos)
        {
            break;
        }
        resolved.append(source.substr(cursor, at - cursor));
        const std::string_view name = source.substr(at + 6, end - at - 6);
        const auto colour = std::find_if(std::begin(kColours), std::end(kColours), [&](const auto & item) { return name == item.name; });
        if (colour == std::end(kColours))
        {
            Rml::Log::Message(Rml::Log::LT_ERROR, "Unknown UI palette colour: %.*s", static_cast<int>(name.size()), name.data());
            resolved.append(source.substr(at, end - at + 1));
        }
        else
        {
            resolved.append(m_dark ? colour->dark : colour->light);
        }
        cursor = end + 1;
    }
    resolved.append(source.substr(cursor));
    return resolved;
}
