#pragma once

#include <RmlUi/Core/FileInterface.h>

struct AAssetManager;

class UiFiles final : public Rml::FileInterface
{
public:
    explicit UiFiles(AAssetManager * assets);

    bool SetDark(bool dark);
    int Generation() const { return m_generation; }

    // Rml::FileInterface
    Rml::FileHandle Open(const Rml::String & path) override;
    void Close(Rml::FileHandle file) override;
    size_t Read(void * buffer, size_t size, Rml::FileHandle file) override;
    bool Seek(Rml::FileHandle file, long offset, int origin) override;
    size_t Tell(Rml::FileHandle file) override;
    size_t Length(Rml::FileHandle file) override;
    bool LoadFile(const Rml::String & path, Rml::String & out_data) override;

private:
    struct File
    {
        std::string data;
        size_t position = 0;
    };

    bool ReadWhole(const std::string & path, std::string & out) const;
    std::string Resolve(std::string_view source) const;

    AAssetManager * m_assets = nullptr;
    bool m_dark = false;
    int m_generation = 0;
};
