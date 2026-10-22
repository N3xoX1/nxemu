#pragma once
#include <RmlUi/Core/FileInterface.h>
#include <filesystem>
#include <string>

namespace UiPack
{
bool Open(const std::filesystem::path & packFile, const std::filesystem::path & root);
bool ReadFile(const std::filesystem::path & path, std::string & data);
bool Exists(const std::filesystem::path & path);
} // namespace UiPack

class UiPackFileInterface final : public Rml::FileInterface
{
public:
    explicit UiPackFileInterface(Rml::FileInterface & delegate) : m_delegate(delegate) {}

    Rml::FileHandle Open(const Rml::String & path) override;
    void Close(Rml::FileHandle file) override;
    size_t Read(void * buffer, size_t size, Rml::FileHandle file) override;
    bool Seek(Rml::FileHandle file, long offset, int origin) override;
    size_t Tell(Rml::FileHandle file) override;
    size_t Length(Rml::FileHandle file) override;

private:
    struct File;
    Rml::FileInterface & m_delegate;
};
