#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace Rml
{
class Context;
class ElementDocument;
} // namespace Rml

class Dialog
{
public:
    Dialog(Rml::Context * context, std::filesystem::path file, float narrowBelow = 620.0f);
    static constexpr float kSettingsNarrowBelow = 1024.0f;
    static constexpr float kTightBelow = 1200.0f;
    virtual ~Dialog();

    void Show();
    bool IsVisible() const { return m_document != nullptr; }
    void Update();
    void RefreshTranslation();

protected:
    Rml::ElementDocument * Document() const { return m_document; }
    void Close();
    float WindowWidth() const;
    void SetText(const char * id, const std::string & text);

    virtual void OnOpen() {}
    virtual void OnUpdate() {}
    virtual void OnCommand(const std::string & /*command*/) {}
    virtual void OnCloseRequested() { Close(); }

private:
    Dialog(const Dialog &) = delete;
    Dialog & operator=(const Dialog &) = delete;

    Rml::Context * const m_context;
    const std::filesystem::path m_file;
    const float m_narrowBelow;
    Rml::ElementDocument * m_document = nullptr;
    std::vector<std::string> m_commands; // clicked since the last frame
};
