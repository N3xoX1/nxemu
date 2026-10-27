#pragma once
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/ui/host/ui_theme.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>

// This document has no images, catalogue, system fonts or guest dependencies.
// It is rendered with the same device/context that the full interface uses.
class StartupView final : public Rml::EventListener
{
public:
    explicit StartupView(UiWindow & window) : m_window(window) {}
    StartupView(const StartupView &) = delete;
    StartupView & operator=(const StartupView &) = delete;
    ~StartupView() { Close(); }
    bool Create()
    {
        const auto source = UiThemeFileInterface::Resolve(R"(
<rml><head><style>
body { position:absolute; left:0; top:0; width:100%; height:100%; margin:0;
 font-family:LatoLatin; font-size:16dp; color:var(--text); background-color:var(--canvas); }
#bar { height:40dp; background-color:var(--chrome); padding:12dp 20dp; box-sizing:border-box; }
#close { position:absolute; right:14dp; top:5dp; width:34dp; height:28dp;
 color:var(--text); background-color:var(--surface); border:1dp var(--border); }
#content { margin:55dp 40dp; }
h1 { font-size:28dp; margin-bottom:18dp; }
#status { color:var(--text-muted); margin-bottom:30dp; }
.placeholder { display:inline-block; width:135dp; height:180dp; margin-right:20dp;
 background-color:var(--raised); border-radius:10dp; }
</style></head><body>
<div id="bar" data-window="drag">NXEmu</div><button id="close">X</button>
<div id="content"><h1>Library</h1><p id="status">Starting NXEmu...</p>
<div class="placeholder"/><div class="placeholder"/><div class="placeholder"/>
<div class="placeholder"/></div></body></rml>)", m_window.IsLightTheme());
        m_document = m_window.Context()->LoadDocumentFromMemory(source);
        if (!m_document) return false;
        m_document->GetElementById("close")->AddEventListener("click", this);
        m_document->Show();
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
        m_window.Show();
        if (SDL_GetWindowFlags(m_window.Handle()) & SDL_WINDOW_MINIMIZED)
            return true;
        return m_window.Render(nullptr);
    }
    void ProcessEvent(Rml::Event &) override { m_cancelled = true; }
private:
    UiWindow & m_window;
    Rml::ElementDocument * m_document = nullptr;
    bool m_cancelled = false;
};

