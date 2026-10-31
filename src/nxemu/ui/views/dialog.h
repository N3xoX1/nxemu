#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace Rml
{
class Context;
class ElementDocument;
} // namespace Rml

class UiDialogWindow;
class UiWindow;

class Dialog
{
public:
    Dialog(Rml::Context * context, std::filesystem::path file, float narrowBelow = 620.0f);
    // Size in dp; a height of 0 fits the window to the panel and fixes its size.
    Dialog(UiWindow & window, std::filesystem::path file, float narrowBelow, int windowWidth, int windowHeight);
    static constexpr float kSettingsNarrowBelow = 1024.0f;
    static constexpr float kTightBelow = 1200.0f;
    static constexpr int kSettingsWidth = 1180;
    static constexpr int kSettingsHeight = 700;
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
    UiDialogWindow * const m_dialogWindow = nullptr;
    const std::filesystem::path m_file;
    const float m_narrowBelow;
    const int m_windowWidth = 0;
    const int m_windowHeight = 0;
    Rml::ElementDocument * m_document = nullptr;
    bool m_inDialogWindow = false;
    std::vector<std::string> m_commands; // clicked since the last frame
};
