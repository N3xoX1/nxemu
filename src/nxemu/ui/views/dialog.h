#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace Rml
{
class Context;
class ElementDocument;
} // namespace Rml

// A panel shown over the window, which keeps the keyboard and the mouse until
// it closes. Its document is loaded when it opens and unloaded when it
// closes: a closed dialog costs nothing.
//
// In the document, an element with a "data-command" attribute is a button:
// clicking it gives its command to OnCommand(). "close" closes the dialog, as
// the Escape key does.
class Dialog
{
public:
    // The dialog takes the whole window when it is narrower than narrowBelow,
    // in dp: the document then has the class "narrow".
    Dialog(Rml::Context * context, std::filesystem::path file, float narrowBelow = 620.0f);
    // Below which the pages of the settings take the whole window, in dp.
    static constexpr float kSettingsNarrowBelow = 1024.0f;
    // Below which a dialog has the class "tight": it fits, with less room
    // around what it shows.
    static constexpr float kTightBelow = 1200.0f;
    virtual ~Dialog();

    void Show();
    bool IsVisible() const { return m_document != nullptr; }
    // Advances the dialog by one frame. What the user did in it since the
    // last one takes effect here, not while RmlUi dispatches the event.
    void Update();
    void RefreshTranslation();

protected:
    Rml::ElementDocument * Document() const { return m_document; }
    void Close();
    // How wide the window is, in dp.
    float WindowWidth() const;
    // Shows text in the element with the id, when it is not what it shows.
    void SetText(const char * id, const std::string & text);

    virtual void OnOpen() {}
    virtual void OnUpdate() {}
    virtual void OnCommand(const std::string & /*command*/) {}
    // The user asked to close the dialog.
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
