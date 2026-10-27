#pragma once
#include <nxemu/ui/host/ui_window.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/StringUtilities.h>
#include <yuzu_common/fs/path_util.h>

// This document uses only the bundled logo and fonts, with no catalogue,
// system fonts or guest dependencies.
// It is rendered with the same device/context that the full interface uses.
class StartupView final : public Rml::EventListener
{
public:
    explicit StartupView(UiWindow & window) : m_window(window) {}
    StartupView(const StartupView &) = delete;
    StartupView & operator=(const StartupView &) = delete;
    ~StartupView() { Close(); }
    bool Create(const std::filesystem::path & assetDir)
    {
        constexpr const char * source = R"(
<rml><head><style>
body { position:absolute; left:0; top:0; width:100%; height:100%; margin:0;
 font-family:LatoLatin; font-size:14dp; color:#e6ebf2; background-color:#0a101b; }
#bar { height:40dp; }
#close { position:absolute; right:14dp; top:5dp; width:34dp; height:28dp;
 color:#8493a8; background-color:transparent; border:0; font-size:20dp; }
#close:hover { color:#ffffff; background-color:#263246; border-radius:5dp; }
#content { position:absolute; left:0; top:0; width:100%; height:100%;
 display:flex; flex-direction:column; justify-content:center; align-items:center; }
#logo { width:220dp; height:220dp; margin-bottom:36dp; }
#status { color:#9aa9bc; margin:0 0 20dp; }
#track { width:200dp; height:3dp; overflow:hidden; border-radius:2dp; background-color:#1d2c40; }
#pulse { width:42%; height:100%; background-color:#00d4ee; border-radius:2dp;
 animation:0.85s cubic-in-out infinite alternate startup-pulse; }
@keyframes startup-pulse {
 from { opacity:0.45; transform:translateX(0%); }
 to { opacity:1; transform:translateX(138%); }
}
#bar, #close { z-index:1; }
</style></head><body>
<div id="bar" data-window="drag"/><button id="close">&#215;</button>
<div id="content"><img id="logo" src="images/nxemu-logo.png"/>
<p id="status">Starting NXEmu...</p>
<div id="track"><div id="pulse"/></div></div></body></rml>)";
        // Match RmlUi's file stream URL escaping (notably Windows drive letters).
        // The source URL also resolves the logo through nxemu.lang in packaged builds.
        m_document = m_window.Context()->LoadDocumentFromMemory(source,
            Rml::StringUtilities::Replace(
                Common::FS::PathToUTF8String((assetDir / "startup.rml").generic_u8string()), ':', '|'));
        if (!m_document) return false;
        m_document->GetElementById("close")->AddEventListener("click", this);
        m_document->Show(Rml::ModalFlag::Modal);
        return true;
    }
    void Close()
    {
        if (!m_document) return;
        m_document->GetElementById("close")->RemoveEventListener("click", this);
        m_document->Close();
        m_document = nullptr;
    }
    bool Cancelled() const { return m_cancelled; }
    bool Pump(const char * status = nullptr)
    {
        // Documents created during setup may call Show(). Keep their partially
        // built contents covered, including while their layout and icons settle.
        m_document->PullToFront();
        if (status) m_document->GetElementById("status")->SetInnerRML(status);
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE))
                m_cancelled = true;
            else
                m_window.HandleEvent(event);
        }
        if (m_cancelled) return false;
        m_window.SyncNativeState();
        m_window.Context()->Update();
        if (SDL_GetWindowFlags(m_window.Handle()) & SDL_WINDOW_MINIMIZED)
            return true;
        return m_window.Render(nullptr, true);
    }
    void ProcessEvent(Rml::Event &) override { m_cancelled = true; }
private:
    UiWindow & m_window;
    Rml::ElementDocument * m_document = nullptr;
    bool m_cancelled = false;
};

