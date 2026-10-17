#include "ui_theme.h"
#include <RmlUi/Core/Log.h>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>

namespace
{
// The shared UI palette. Semantic tokens keep component styles independent
// of the selected theme. Artwork and the in-game performance graphs keep
// their own colours.
constexpr struct { const char * name; const char * dark; const char * light; } kColours[] = {
    {"canvas", "#070b14", "#f3f5f9"},
    {"chrome", "#0a0f1a", "#e9edf4"},
    {"surface", "#0f1a2b", "#ffffff"},
    {"veil", "#070b14e1", "#f3f5f9e6"},
    {"badge", "#0f1a2be1", "#fffffff2"},
    {"raised", "#18233a", "#f1f4f9"},
    {"hover", "#1b2a44", "#e2eaf5"},
    {"selection", "#16305a", "#dceaff"},
    {"border", "#24344f", "#cbd5e1"},
    {"border-strong", "#3d5a8a", "#8fa3bf"},
    {"text", "#e6ebf2", "#172033"},
    {"text-secondary", "#b4bfce", "#354760"},
    {"text-muted", "#8f9bb0", "#52637a"},
    {"text-disabled", "#6f7d92", "#6b778b"},
    {"accent", "#2f7df6", "#1766d9"},
    {"accent-hover", "#4a90ff", "#0b55c1"},
    {"accent-text", "#8fb8ff", "#1758aa"},
    {"accent-pressed", "#1458ad", "#0d56b8"},
    {"accent-pressed-hover", "#1c6acb", "#084b9f"},
    {"accent-soft", "#2f7df640", "#1766d926"},
    {"on-accent", "#ffffff", "#ffffff"},
    {"success", "#4ade80", "#18703d"},
    {"success-fill", "#4ade80b0", "#18703db0"},
    {"accent-fill", "#2f7df6b0", "#1766d9b0"},
    {"success-surface", "#153f2a", "#e1f3e7"},
    {"warning", "#f0b35a", "#8a500b"},
    {"warning-surface", "#43330f", "#fff3d9"},
    {"warning-border", "#80542d", "#d4a254"},
    {"danger", "#ff9d9d", "#b52e3f"},
    {"danger-surface", "#3b202b", "#fce6eb"},
    {"danger-button", "#b3403f", "#b3403f"},
    {"danger-hover", "#872735", "#942f3e"},
    {"violet", "#c9b4ff", "#6840a1"},
    {"violet-surface", "#271c4a", "#eee7fa"},
};
}

struct UiThemeFileInterface::File
{
    Rml::FileHandle delegate = 0;
    std::string source;
    size_t position = 0;
};

bool UiThemeFileInterface::SetLight(bool light)
{
    if (m_light == light) return false;
    m_light = light;
    return true;
}

std::string UiThemeFileInterface::Resolve(std::string_view source, bool light)
{
    // Append once instead of shifting the rest of the stylesheet for every
    // substitution. Views also avoid allocating a string for each token.
    std::string resolved;
    resolved.reserve(source.size());
    size_t cursor = 0;
    size_t at;
    while ((at = source.find("var(--", cursor)) != std::string_view::npos)
    {
        const size_t end = source.find(')', at + 6);
        if (end == std::string_view::npos) break;
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
            resolved.append(light ? colour->light : colour->dark);
        }
        cursor = end + 1;
    }
    resolved.append(source.substr(cursor));
    return resolved;
}

Rml::FileHandle UiThemeFileInterface::Open(const Rml::String & path)
{
    auto file = std::make_unique<File>();
    if (path.ends_with(".rcss"))
    {
        if (!m_delegate.LoadFile(path, file->source)) return 0;
        file->source = Resolve(file->source, m_light);
    }
    else
    {
        file->delegate = m_delegate.Open(path);
        if (file->delegate == 0) return 0;
    }
    return reinterpret_cast<Rml::FileHandle>(file.release());
}

void UiThemeFileInterface::Close(Rml::FileHandle handle)
{
    std::unique_ptr<File> file(reinterpret_cast<File *>(handle));
    if (file->delegate) m_delegate.Close(file->delegate);
}

size_t UiThemeFileInterface::Read(void * buffer, size_t size, Rml::FileHandle handle)
{
    auto & file = *reinterpret_cast<File *>(handle);
    if (file.delegate) return m_delegate.Read(buffer, size, file.delegate);
    const size_t count = std::min(size, file.source.size() - file.position);
    if (count) std::memcpy(buffer, file.source.data() + file.position, count);
    file.position += count;
    return count;
}

bool UiThemeFileInterface::Seek(Rml::FileHandle handle, long offset, int origin)
{
    auto & file = *reinterpret_cast<File *>(handle);
    if (file.delegate) return m_delegate.Seek(file.delegate, offset, origin);
    if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) return false;
    const auto base = static_cast<int64_t>(origin == SEEK_CUR ? file.position : origin == SEEK_END ? file.source.size() : 0);
    const int64_t position = base + offset;
    if (position < 0 || static_cast<uint64_t>(position) > file.source.size()) return false;
    file.position = static_cast<size_t>(position);
    return true;
}

size_t UiThemeFileInterface::Tell(Rml::FileHandle handle)
{
    auto & file = *reinterpret_cast<File *>(handle);
    return file.delegate ? m_delegate.Tell(file.delegate) : file.position;
}

size_t UiThemeFileInterface::Length(Rml::FileHandle handle)
{
    auto & file = *reinterpret_cast<File *>(handle);
    return file.delegate ? m_delegate.Length(file.delegate) : file.source.size();
}
