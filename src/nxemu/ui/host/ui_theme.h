#pragma once
#include <RmlUi/Core/FileInterface.h>
#include <string>
#include <string_view>

// RmlUi does not implement CSS custom properties. Resolve our palette tokens
// when RCSS files are opened; parsed styles need no extra work while rendering.
// The delegate stays owned by RmlUi and is used until Rml::Shutdown().
class UiThemeFileInterface final : public Rml::FileInterface
{
public:
    explicit UiThemeFileInterface(Rml::FileInterface & delegate) : m_delegate(delegate) {}
    bool SetLight(bool light);
    static std::string Resolve(std::string_view source, bool light);

    Rml::FileHandle Open(const Rml::String & path) override;
    void Close(Rml::FileHandle file) override;
    size_t Read(void * buffer, size_t size, Rml::FileHandle file) override;
    bool Seek(Rml::FileHandle file, long offset, int origin) override;
    size_t Tell(Rml::FileHandle file) override;
    size_t Length(Rml::FileHandle file) override;

private:
    struct File;
    Rml::FileInterface & m_delegate;
    bool m_light = false;
};
