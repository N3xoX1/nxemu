#include "main_window.h"
#include "ui/host/ui_window.h"
#include <RmlUi/Core/Context.h>
#include <nxemu-core/notification.h>

MainWindow::MainWindow(UiWindow & window, EmulatorSession & session, std::filesystem::path assetDir) :
    m_window(window),
    m_session(session),
    m_assetDir(std::move(assetDir))
{
    m_session.SetListener(this);
}

MainWindow::~MainWindow()
{
    m_session.SetListener(nullptr);
    m_window.DetachVideo();
}

bool MainWindow::Create()
{
    m_document = m_window.Context()->LoadDocument((m_assetDir / "main_window.rml").string());
    if (m_document == nullptr)
    {
        return false;
    }
    m_document->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
    return true;
}

void MainWindow::RequestQuit()
{
    if (!m_session.IsRunning())
    {
        m_quit = true;
    }
}

void MainWindow::HandleEvent(const SDL_Event & event)
{
    m_window.HandleEvent(event);
}

void MainWindow::Update()
{
    RunPendingWork();
    m_window.Context()->Update();
}

bool MainWindow::Still() const
{
    return m_window.Context()->GetNextUpdateDelay() > 0;
}

void MainWindow::OnModulesReleasing()
{
    m_window.DetachVideo();
}

void MainWindow::OnModulesReady()
{
    AttachVideo();
}

void MainWindow::OnGameStarted(const std::string & /*path*/)
{
}

void MainWindow::OnSessionError(const std::string & message)
{
    g_notify->DisplayError(message.c_str(), "NXEmu");
}

void MainWindow::RunPendingWork()
{
}

const PixelRect * MainWindow::GameArea() const
{
    return nullptr;
}

bool MainWindow::AttachVideo()
{
    if (m_session.IsValid() && m_window.AttachVideo(m_session.Modules().Video()))
    {
        return true;
    }
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "NXEmu", m_window.Error().c_str(), m_window.Handle());
    m_quit = true;
    return false;
}
