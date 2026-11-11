#pragma once
#include "dialog.h"
#include "loading_time_estimate.h"
#include <chrono>
#include <string>

class EmulatorSession;
class UiWindow;

// Imports a shader file into a game's cache and builds it, showing the progress. The game is
// loaded in the background only to read its shaders: it never runs, and the window stays as it is.
class ShaderImportDialog final : public Dialog
{
public:
    ShaderImportDialog(UiWindow & window, EmulatorSession & session, const std::filesystem::path & assets);

    // False when the import could not start; the session has said why.
    bool Start(const std::string & game, const std::string & title, const std::string & file);
    // The session finished the import; error is empty when the shaders were imported.
    void Finished(const std::string & error);
    // From the start until the session has stopped again.
    bool Busy() const { return m_step == Step::Running || m_step == Step::Stopping; }

private:
    enum class Step { Idle, Running, Stopping, Done };

    void OnOpen() override;
    void OnUpdate() override;
    void OnCloseRequested() override;

    void ShowProgress();
    void ShowResult();

    EmulatorSession & m_session;
    Step m_step = Step::Idle;
    std::string m_title;
    std::string m_error;
    bool m_finished = false;
    bool m_cancelled = false;
    int m_startTick = 0;
    std::chrono::steady_clock::time_point m_started;
    LoadingTimeEstimate m_timeEstimate;
};
