#pragma once
#include "ui/emulation/emulator_session.h"
#include "ui/host/ui_window.h"
#include <RmlUi/Core/ElementDocument.h>
#include <filesystem>

class UiWindow;

class MainWindow final : private EmulatorSession::Listener
{
public:
    MainWindow(UiWindow & window, EmulatorSession & session, std::filesystem::path assetDir);
    ~MainWindow();

    bool Create();
    void RequestQuit();
    bool QuitRequested() const { return m_quit; }

    void HandleEvent(const SDL_Event & event);
    void Update();
    bool Still() const;
    const PixelRect * GameArea() const;

private:
    MainWindow(const MainWindow &) = delete;
    MainWindow & operator=(const MainWindow &) = delete;

    // EmulatorSession::Listener
    void OnModulesReleasing() override;
    void OnModulesReady() override;
    void OnGameStarted(const std::string & path) override;
    void OnSessionError(const std::string & message) override;

    void RunPendingWork();
    bool AttachVideo();

    UiWindow & m_window;
    EmulatorSession & m_session;
    const std::filesystem::path m_assetDir;
    Rml::ElementDocument * m_document = nullptr;
    bool m_quit = false;
};
