// SPDX-License-Identifier: GPL-2.0-or-later
#include "shader_profile_dialog.h"
#include "yuzu_video_core/shader_profile_publication.h"
#include <nxemu-core/notification.h>
#include <RmlUi/Core.h>
#include <chrono>
#include <algorithm>
#include <map>
#include <vector>
#include <common/path.h>
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/ui/i18n/translation.h>
#include <nxemu/ui/platform/file_dialogs.h>
#include <stdexcept>
#include <yuzu_common/fs/path_util.h>
namespace
{
namespace Profile = VideoCommon::ShaderProfile;
std::string Utf8(const std::filesystem::path & path)
{
    return Common::FS::PathToUTF8String(path);
}
std::string Escaped(const std::string & text)
{
    return Rml::StringUtilities::EncodeRml(text);
}
} // namespace
ShaderProfileDialog::ShaderProfileDialog(UiWindow & window, const std::filesystem::path & assets) :
    Dialog(window.Context(), assets / "shader_profile_dialog.rml"), m_window(window)
{
}
ShaderProfileDialog::~ShaderProfileDialog()
{
    CancelAndWait();
    RemoveStaging();
}
void ShaderProfileDialog::CancelAndWait()
{
    m_cancel = true;
    if (m_thread.joinable()) m_thread.join();
}
void ShaderProfileDialog::RemoveStaging()
{
    if (m_cache.empty()) return;
    std::error_code error;
    std::filesystem::remove_all(m_cache / "exports", error);
}
void ShaderProfileDialog::Prepare(IVideo & video, std::string game, uint64_t title, std::filesystem::path cache, std::string label)
{
    if (Busy()) return;
    CancelAndWait();
    RemoveStaging();
    m_video = &video;
    m_game = std::move(game);
    m_title = title;
    m_cache = std::move(cache);
    m_gameLabel = std::move(label);
    m_root = Utf8(std::filesystem::u8path(m_game).parent_path());
    m_cacheFile = Utf8(m_cache / "vulkan.nxc");
    m_versions.clear();
    m_unidentified = 0;
    try {
        auto versions = Profile::ListRecordedVersions(m_cache, m_title, m_gameLabel);
        m_versions = std::move(versions.entries);
        m_unidentified = versions.unavailable;
    } catch (const std::exception &) {
    }
    m_outputs.clear();
    m_exportNotes.clear();
    m_saved.clear();
    m_cancel = false;
    m_finished = false;
    m_success = false;
    m_error.clear();
    m_phase.clear();
    m_currentLabel.clear();
    m_done = m_total = m_foundPrograms = m_totalPrograms = 0;
    Show();
}
std::string ShaderProfileDialog::GroupLabel(const std::vector<size_t> & versions) const
{
    std::string list;
    for (const auto index : versions) list += (list.empty() ? "" : ", ") + m_versions[index].short_label;
    return m_gameLabel + " - " + list;
}
void ShaderProfileDialog::OnOpen()
{
    SetText("profile-title", Tr("Export shaders"));
    SetText("profile-game", Escaped(m_gameLabel));
    SetText("profile-status", "");
    if (m_versions.empty()) {
        ShowStep(Step::Done);
        SetText("profile-detail", m_unidentified ? Tr("These shaders cannot be exported: the game version they belong to is unknown.")
            : Tr("Nothing has been recorded for this game yet. Shaders are not recorded while mods or cheats are active."));
        return;
    }
    Start();
}
void ShaderProfileDialog::ShowStep(Step step)
{
    m_step = step;
    auto * start = Document()->GetElementById("profile-start");
    if (step == Step::Running) start->SetAttribute("disabled", "");
    else start->RemoveAttribute("disabled");
    std::string detail;
    switch (step) {
    case Step::Running:
        SetText("profile-start", Tr("Save"));
        return;
    case Step::Save:
    case Step::Done:
        for (const auto & output : m_outputs) detail += (detail.empty() ? "" : "<br/>") + Escaped(output.name);
        for (const auto & note : m_exportNotes) detail += "<br/>" + Escaped(note);
        if (!m_saved.empty()) detail += "<br/><br/>" + Tr("Saved in %1", {Escaped(m_saved)});
        SetText("profile-start", Tr(step == Step::Save ? "Save" : "Close"));
        break;
    }
    SetText("profile-detail", detail);
}
bool ShaderProfileDialog::ProfileProgress(const char * phase, uint64_t done, uint64_t total)
{
    std::scoped_lock lock(m_mutex);
    if (std::string_view(phase) == "programs") {
        m_foundPrograms = done;
        m_totalPrograms = total;
        return !m_cancel.load();
    }
    m_phase = phase;
    m_done = done;
    m_total = total;
    return !m_cancel.load();
}
void ShaderProfileDialog::Start()
{
    if (Busy()) return;
    if (m_thread.joinable()) m_thread.join();
    m_cancel = false;
    m_finished = false;
    m_success = false;
    m_error.clear();
    m_busy = true;
    m_started = std::chrono::steady_clock::now();
    ShowStep(Step::Running);
    SetText("profile-detail", "");
    SetText("profile-status", Tr("Identifying recorded game version"));
    try {
        m_thread = std::thread([this] { RunExport(); });
    } catch (const std::exception & error) {
        m_busy = false;
        m_finished = true;
        m_error = error.what();
    }
}
// Exports every version oldest first, each one merged into the file of the
// previous ones. When a merge would exceed the file limits, that version
// starts a new file instead, so every version is kept whole.
void ShaderProfileDialog::RunExport()
{
    bool success = false;
    std::string failure;
    std::vector<ExportOutput> outputs;
    std::vector<std::string> notes;
    try {
        const auto staging = m_cache / "exports";
        std::error_code error_code;
        std::filesystem::remove_all(staging, error_code);
        std::filesystem::create_directories(staging);
        const auto base = Utf8(std::filesystem::u8path(m_game).filename());
        size_t part{};
        const auto run = [&](size_t index, const std::filesystem::path & previous, const std::vector<size_t> & group, std::filesystem::path & staged) {
            if (m_cancel) throw std::runtime_error("Shader export cancelled");
            const auto & version = m_versions[index];
            {
                std::scoped_lock lock(m_mutex);
                m_currentLabel = version.short_label;
            }
            staged = staging / ("part-" + std::to_string(part++) + ".nxshader");
            const auto profile = Utf8(staged), recording = Utf8(version.recording), previous_path = Utf8(previous);
            const auto index_path = Utf8(version.directory / "export-index.json");
            auto names = group;
            names.push_back(index);
            const auto label = GroupLabel(names);
            ShaderProfileTask task{};
            task.compact = true;
            task.title_id = m_title;
            task.cache = m_cacheFile.c_str();
            task.recording = recording.c_str();
            task.game_root = m_root.c_str();
            task.base = base.c_str();
            task.update = "";
            task.profile = profile.c_str();
            task.index = index_path.c_str();
            task.label = label.c_str();
            task.previous = previous.empty() ? nullptr : previous_path.c_str();
            ShaderProfileResult result{};
            char error[1024]{};
            if (!m_video->ProcessShaderProfile(task, *this, result, error, sizeof(error)))
                throw std::runtime_error(error);
            return result;
        };
        ExportOutput group;
        std::filesystem::path previous;
        std::string first_failure;
        for (size_t index = 0; index < m_versions.size(); ++index) {
            std::filesystem::path staged;
            bool too_large{};
            std::string failure;
            try {
                const auto result = run(index, previous, group.versions, staged);
                too_large = !previous.empty() && result.dropped_versions;
            } catch (const std::exception & error) {
                // A merge can also exceed the size of a file. If this version
                // exports alone, it starts a new file; otherwise the error is
                // its own and the retry below reports it.
                if (m_cancel) throw;
                if (previous.empty()) failure = error.what();
                else too_large = true;
            }
            if (too_large) {
                std::filesystem::remove(staged, error_code);
                try {
                    run(index, {}, {}, staged);
                    notes.push_back(Tr("%1 starts a new file: combined with the previous versions, the file would be too large.",
                        {m_versions[index].short_label}));
                    outputs.push_back(std::move(group));
                    group = {};
                } catch (const std::exception & error) {
                    if (m_cancel) throw;
                    failure = error.what();
                }
            }
            // A version that cannot be exported, such as one whose game files are not
            // beside the game, is left out and said so; the other versions still export.
            if (!failure.empty()) {
                std::filesystem::remove(staged, error_code);
                notes.push_back(Tr("%1 was not included: %2", {m_versions[index].short_label, failure}));
                if (first_failure.empty()) first_failure = m_versions[index].short_label + ": " + failure;
                continue;
            }
            group.versions.push_back(index);
            group.staged = staged;
            previous = staged;
        }
        if (!group.versions.empty()) outputs.push_back(std::move(group));
        if (outputs.empty()) throw std::runtime_error(first_failure);
        if (m_unidentified)
            notes.push_back(Tr("Some shaders were not included: the game version they belong to is unknown."));
        std::map<std::string, size_t> names;
        for (auto & output : outputs) {
            std::vector<const Profile::RecordingVersion *> versions;
            for (const auto index : output.versions) versions.push_back(&m_versions[index]);
            output.name = Profile::CombinedProfileFilename(m_gameLabel, versions, m_title);
            if (const auto count = ++names[output.name]; count > 1)
                output.name.insert(output.name.size() - 9, " (" + std::to_string(count) + ")");
        }
        success = true;
    } catch (const std::exception & error) {
        failure = error.what();
    }
    std::scoped_lock lock(m_mutex);
    m_success = success;
    m_error = std::move(failure);
    m_outputs = std::move(outputs);
    m_exportNotes = std::move(notes);
    m_finished = true;
    m_busy = false;
}
// Copies the exported files next to their destination, then replaces atomically.
bool ShaderProfileDialog::SaveOutputs()
{
    const auto filter = FileFilter({{Tr("NXEmu shaders"), "*.nxshader"}});
    std::vector<std::filesystem::path> targets;
    bool confirmed = false;
    if (m_outputs.size() == 1) {
        Path selected;
        if (!FileSave(m_window.NativeHandle(), m_root.c_str(), filter.c_str(), m_outputs.front().name.c_str(), selected)) return false;
        targets.push_back(std::filesystem::u8path(static_cast<const char *>(selected)));
#ifdef __APPLE__
        // NSSavePanel already confirms replacement.
        confirmed = true;
#endif
    } else {
        const std::string directory = static_cast<const char *>(BrowseForDirectory(m_window.NativeHandle(),
            Tr("Choose the folder for the %1 shader files", {std::to_string(m_outputs.size())}).c_str()));
        if (directory.empty()) return false;
        for (const auto & output : m_outputs) targets.push_back(std::filesystem::u8path(directory) / std::filesystem::u8path(output.name));
    }
    std::string existing;
    for (const auto & target : targets)
        if (std::filesystem::exists(target)) existing += (existing.empty() ? "" : "\n") + Utf8(target.filename());
    if (!existing.empty() && !confirmed && (!g_notify || g_notify->Query(
            (Tr("Replace these existing shader files?") + "\n\n" + existing).c_str(),
            Tr("Replace shader files").c_str()) != NotificationResponse::Yes))
        return false;
    try {
        for (size_t i = 0; i < targets.size(); ++i) {
            auto partial = targets[i];
            partial += ".partial";
            std::filesystem::copy_file(m_outputs[i].staged, partial, std::filesystem::copy_options::overwrite_existing);
            Profile::ReplaceCompletedExport(partial, targets[i]);
        }
    } catch (const std::exception & error) {
        SetText("profile-status", Escaped(error.what()));
        return false;
    }
    m_saved = Utf8(targets.front().parent_path());
    RemoveStaging();
    return true;
}
void ShaderProfileDialog::OnUpdate()
{
    if (!Busy() && m_thread.joinable()) m_thread.join();
    if (m_step != Step::Running) return;
    std::unique_lock lock(m_mutex);
    if (m_finished) {
        Document()->GetElementById("profile-progress-fill")->SetProperty("width", m_success ? "100%" : "0%");
        Document()->GetElementById("profile-progress-fill")->SetProperty("margin-left", "0%");
        if (!m_success) {
            const auto error = m_error;
            lock.unlock();
            RemoveStaging();
            ShowStep(Step::Done);
            SetText("profile-detail", "");
            SetText("profile-status", "<div class=\"profile-error\"><div class=\"profile-error-title\">" +
                Tr("The shaders could not be exported") +
                "</div><div class=\"profile-error-text\">" + Escaped(error) + "</div></div>");
            return;
        }
        lock.unlock();
        SetText("profile-status", "");
        // Ask where to save once the files and their names are known.
        ShowStep(Step::Save);
        if (SaveOutputs()) ShowStep(Step::Done);
        return;
    }
    const char * status = m_phase == "identify-content" ? TrMark("Identifying the game version") : m_phase == "read-cache" ? TrMark("Reading your shader cache")
        : m_phase == "assemble" ? TrMark("Preparing the recipes") : m_phase == "compress" ? TrMark("Compressing the shaders")
        : m_phase == "write-profile" ? TrMark("Saving the shaders") : m_phase == "verify-index" ? TrMark("Checking earlier results")
        : m_phase == "raw" ? TrMark("Searching the game files") : m_phase == "verify" ? TrMark("Checking the shaders") : TrMark("Searching the game resources");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_started).count();
    // Title and elapsed time, then what is measured, then the shaders found so far.
    const auto seconds = elapsed / 1000;
    const auto clock = std::to_string(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60);
    std::string title = Tr(status);
    if (!m_currentLabel.empty()) title = Escaped(m_currentLabel) + " · " + title;
    std::string measure;
    if (m_phase == "raw") measure = Tr("%1 of %2 MiB read", {std::to_string(m_done / (1024 * 1024)), std::to_string(m_total / (1024 * 1024))});
    else if (m_phase == "resources") measure = Tr("%1 resources examined", {std::to_string(m_done)});
    else if (m_total) measure = Tr("%1 of %2", {std::to_string(m_done), std::to_string(m_total)});
    else measure = Tr("This can take a few minutes.");
    std::string detail = "<div class=\"profile-status-head\"><span class=\"profile-status-title\">" + title +
        "</span><span class=\"profile-status-time\">" + clock + "</span></div><div class=\"profile-status-line\">" + measure + "</div>";
    if (m_totalPrograms) {
        const auto percent = 100 * std::min(m_foundPrograms, m_totalPrograms) / m_totalPrograms;
        detail += "<div class=\"profile-status-found\">" + Tr("%1 of %2 shaders found (%3 %)",
            {std::to_string(m_foundPrograms), std::to_string(m_totalPrograms), std::to_string(percent)}) + "</div>";
    }
    SetText("profile-status", detail);
    const auto total = m_total;
    const auto done = m_done;
    auto * fill = Document()->GetElementById("profile-progress-fill");
    fill->SetProperty("width", total ? std::to_string(100 * std::min(done, total) / total) + "%" : "25%");
    fill->SetProperty("margin-left", total ? "0%" : std::to_string((elapsed / 30) % 76) + "%");
}
void ShaderProfileDialog::OnCloseRequested()
{
    if (Busy()) {
        Cancel();
        SetText("profile-status", Tr("Cancelling"));
        return;
    }
    RemoveStaging();
    Close();
}
void ShaderProfileDialog::OnCommand(const std::string & command)
{
    if (command == "close") {
        OnCloseRequested();
        return;
    }
    if (Busy() || command != "start") return;
    switch (m_step) {
    case Step::Save:
        if (SaveOutputs()) ShowStep(Step::Done);
        break;
    case Step::Done:
        RemoveStaging();
        Close();
        break;
    default:
        break;
    }
}
