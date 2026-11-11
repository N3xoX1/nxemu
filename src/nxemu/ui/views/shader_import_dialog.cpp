// SPDX-License-Identifier: GPL-2.0-or-later
#include "shader_import_dialog.h"
#include "load_stage.h"
#include <RmlUi/Core.h>
#include <algorithm>
#include <nxemu-core/notification.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu/ui/emulation/emulator_session.h>
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/ui/i18n/translation.h>

namespace
{
std::string Escaped(const std::string & text)
{
    return Rml::StringUtilities::EncodeRml(text);
}

// The reader's reasons, in words for players; anything else as it is.
std::string ImportFailure(const std::string & error)
{
    return error == "Shader file for a different game" ? Tr("These shaders are for a different game.")
        : error == "Shader file made by a newer version" ? Tr("This shader file was made by a newer version of NXEmu.")
        : error == "Expected a compressed NXShader profile" ? Tr("This is not an NXEmu shader file.")
        : error.starts_with("No compatible shader recipes") ? Tr("None of these shaders match your copy of the game.")
        : error;
}
} // namespace

ShaderImportDialog::ShaderImportDialog(UiWindow & window, EmulatorSession & session, const std::filesystem::path & assets) :
    Dialog(window.Context(), assets / "shader_import_dialog.rml"),
    m_session(session)
{
}

bool ShaderImportDialog::Start(const std::string & game, const std::string & title, const std::string & file)
{
    if (Busy())
    {
        return false;
    }
    // Progress reported before this belongs to an earlier load.
    m_startTick = SettingsStore::GetInstance().GetInt(NXCoreSetting::DiskCacheLoadTick);
    m_step = Step::Running;
    m_title = title;
    m_error.clear();
    m_finished = false;
    m_cancelled = false;
    m_started = std::chrono::steady_clock::now();
    m_timeEstimate.Reset();
    // Never import out of sight: nothing could show the result or cancel it. The window
    // opens before the game loads, so a window that cannot open never starts a load.
    Show();
    if (!IsVisible())
    {
        m_step = Step::Idle;
        if (g_notify != nullptr)
        {
            g_notify->DisplayError(Tr("The import window could not be opened, so the shaders were not imported.").c_str(), Tr("Import shaders").c_str());
        }
        return false;
    }
    if (!m_session.ImportShaders(game, file))
    {
        m_step = Step::Idle;
        Close();
        return false;
    }
    return true;
}

void ShaderImportDialog::Finished(const std::string & error)
{
    if (m_step != Step::Running)
    {
        return;
    }
    m_finished = true;
    m_error = error;
    m_step = Step::Stopping;
}

void ShaderImportDialog::OnOpen()
{
    SetText("import-game", Escaped(m_title));
    SetText("import-detail", Tr("The shaders are checked against your copy of the game, then built. The game does not start."));
    SetText("import-status", "");
    SetText("import-close", Tr("Cancel"));
    ShowProgress();
}

void ShaderImportDialog::OnUpdate()
{
    switch (m_step)
    {
    case Step::Running:
        // Stopped without finishing: cancelled, or the load failed and the session said why.
        if (!m_session.IsRunning())
        {
            m_step = Step::Stopping;
            break;
        }
        ShowProgress();
        break;
    case Step::Stopping:
        // The result shows once the game is unloaded, so the window never sees it end.
        if (m_session.IsRunning() || m_session.CurrentPhase() != EmulatorSession::Phase::Idle)
        {
            break;
        }
        if (!m_finished || (m_cancelled && m_error == "Shader import cancelled"))
        {
            m_step = Step::Idle;
            Close();
            break;
        }
        m_step = Step::Done;
        ShowResult();
        break;
    default:
        break;
    }
}

void ShaderImportDialog::OnCloseRequested()
{
    switch (m_step)
    {
    case Step::Running:
        m_cancelled = true;
        m_session.Stop();
        SetText("import-status", Tr("Cancelling"));
        if (Rml::Element * button = Document()->GetElementById("import-close"))
        {
            button->SetAttribute("disabled", "");
        }
        break;
    case Step::Stopping:
        break;
    default:
        m_step = Step::Idle;
        Close();
        break;
    }
}

void ShaderImportDialog::ShowProgress()
{
    if (m_cancelled)
    {
        return;
    }
    SettingsStore & settings = SettingsStore::GetInstance();
    const bool reported = settings.GetInt(NXCoreSetting::DiskCacheLoadTick) != m_startTick;
    const LoadStage stage = reported ? static_cast<LoadStage>(settings.GetInt(NXCoreSetting::DiskCacheLoadStage)) : LoadStage::Game;
    const bool counted = stage == LoadStage::Shaders || stage == LoadStage::ReadProfile || stage == LoadStage::ImportProfile || stage == LoadStage::BuildProfile;
    const int total = counted ? settings.GetInt(NXCoreSetting::DiskCacheLoadTotal) : 0;
    const int current = std::clamp(settings.GetInt(NXCoreSetting::DiskCacheLoadCurrent), 0, std::max(total, 0));

    const char * label = stage == LoadStage::ReadProfile ? TrMark("Reading and verifying shaders")
        : stage == LoadStage::ImportProfile ? TrMark("Importing shaders")
        : stage == LoadStage::BuildProfile ? TrMark("Compiling imported shaders")
        : stage == LoadStage::Shaders ? TrMark("Preparing shaders")
        : stage == LoadStage::Launch ? TrMark("Finishing...")
                                     : TrMark("Reading the game files");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_started).count();
    const auto seconds = elapsed / 1000;
    const std::string clock = std::to_string(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60);
    std::string measure = total > 0 ? Tr("%1 of %2", {std::to_string(current), std::to_string(total)}) : Tr("This can take a few minutes.");
    if (const auto estimate = counted ? m_timeEstimate.Update(static_cast<int>(stage), current, total) : std::nullopt)
    {
        const auto left = estimate->count();
        measure += " · " + (left < 60 ? Tr("About %1 s remaining", {std::to_string(5 * ((left + 4) / 5))})
                                      : Tr("About %1 min remaining", {std::to_string((left + 59) / 60)}));
    }
    else if (!counted)
    {
        m_timeEstimate.Reset();
    }
    SetText("import-status", "<div class=\"profile-status-head\"><span class=\"profile-status-title\">" + Tr(label) +
        "</span><span class=\"profile-status-time\">" + clock + "</span></div><div class=\"profile-status-line\">" + measure + "</div>");

    Rml::Element * fill = Document()->GetElementById("profile-progress-fill");
    const bool finishing = stage == LoadStage::Launch;
    fill->SetProperty("width", finishing ? "100%" : total > 0 ? std::to_string(100 * current / total) + "%" : "25%");
    fill->SetProperty("margin-left", finishing || total > 0 ? "0%" : std::to_string((elapsed / 30) % 76) + "%");
}

void ShaderImportDialog::ShowResult()
{
    Rml::Element * fill = Document()->GetElementById("profile-progress-fill");
    fill->SetProperty("width", m_error.empty() ? "100%" : "0%");
    fill->SetProperty("margin-left", "0%");
    if (Rml::Element * button = Document()->GetElementById("import-close"))
    {
        button->RemoveAttribute("disabled");
        button->SetClass("primary", true);
    }
    SetText("import-close", Tr("Close"));
    if (m_error.empty())
    {
        SetText("import-detail", Tr("The shaders were imported and built. The game uses them the next time it starts."));
        SetText("import-status", "");
        return;
    }
    SetText("import-detail", "");
    SetText("import-status", "<div class=\"profile-error\"><div class=\"profile-error-title\">" + Tr("The shaders could not be imported.") +
        "</div><div class=\"profile-error-text\">" + Escaped(ImportFailure(m_error)) + "</div></div>");
}
