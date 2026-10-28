// SPDX-License-Identifier: GPL-2.0-or-later
#include "shader_profile_dialog.h"
#include "yuzu_video_core/shader_profile_location.h"
#include "yuzu_video_core/shader_profile_layout.h"
#include "yuzu_video_core/shader_profile_publication.h"
#include "yuzu_video_core/shader_profile_text.h"
#include <nxemu-core/notification.h>
#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <functional>
#include <vector>
#include <common/json.h>
#include <common/path.h>
#include <fstream>
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/ui/i18n/translation.h>
#include <nxemu/ui/platform/file_dialogs.h>
#include <stdexcept>
#include <yuzu_common/fs/fs.h>
#include <yuzu_common/fs/path_util.h>
#ifdef _WIN32
#include <windows.h>
#endif
namespace
{
namespace Profile = VideoCommon::ShaderProfile;
std::string Utf8(const std::filesystem::path & path)
{
    return Common::FS::PathToUTF8String(path);
}
void AtomicWrite(const std::filesystem::path & path, std::string_view bytes)
{
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), bytes.size());
    out.flush();
    if (!out) throw std::runtime_error("Cannot write shader profile metadata");
    out.close();
#ifdef _WIN32
    if (!MoveFileExW(std::filesystem::path(temporary).c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot publish shader profile metadata");
#else
    std::filesystem::rename(temporary, path);
#endif
}
std::string ReadSmall(const std::filesystem::path & path, size_t limit)
{
    const auto size = std::filesystem::file_size(path);
    if (size > limit) throw std::runtime_error("Shader profile size limit");
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read shader profile");
    std::string bytes(static_cast<size_t>(size), '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(size));
    if (!in || in.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Shader profile changed during reading");
    return bytes;
}
std::string Escaped(const std::string & text)
{
    return Rml::StringUtilities::EncodeRml(text);
}
// "1.0 — Full game", or whichever part is present.
// The game versions a profile was recorded with. Other versions can still use
// the shaders they share with them, so this only informs.
std::string TargetsText(const ShaderProfileResult & result)
{
    std::string list;
    for (const char * at = result.targets; *at;) {
        const char * end = std::strchr(at, '\n');
        const std::string line = end ? std::string(at, end) : std::string(at);
        list += (list.empty() ? "" : ", ") + Escaped(line);
        at = end ? end + 1 : at + line.size();
    }
    if (list.empty()) return Tr("Recorded with %1 game versions.", {std::to_string(result.versions)});
    return Tr("Made for: %1.", {list});
}
std::string Description(const std::string & revision, const std::string & note)
{
    return revision.empty() ? note : note.empty() ? revision : revision + " — " + note;
}
std::string Megabytes(uint64_t bytes)
{
    const auto tenths = (bytes * 10 + 512 * 1024) / (1024 * 1024);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " MB";
}
std::filesystem::path ProfileDirectory()
{
    return Profile::ProfileDirectory(Common::FS::GetYuzuPath(Common::FS::YuzuPath::YuzuDir));
}
// Profiles installed for this title: the one selected by an import first, then
// the profiles named after the title in the profile folder, newest first.
std::vector<std::filesystem::path> InstalledProfiles(const std::filesystem::path & cache, uint64_t title)
{
    const auto directory = ProfileDirectory();
    std::vector<std::filesystem::path> result;
    std::error_code error;
    try {
        const auto selection_path = Profile::ImportSelectionPath(cache);
        if (std::filesystem::exists(selection_path, error)) {
            const auto bytes = ReadSmall(selection_path, 16384);
            JsonValue selection;
            JsonReader reader;
            if (reader.Parse(bytes.data(), bytes.data() + bytes.size(), selection) && selection["location"].asString() == "nxshader") {
                const auto file = selection["file"].asString();
                if (!file.empty() && file.find_first_of("/\\:") == std::string::npos) {
                    const auto path = directory / std::filesystem::u8path(file);
                    if (std::filesystem::is_regular_file(path, error)) result.push_back(path);
                }
            }
        }
    } catch (const std::exception &) {}
    const auto id = Profile::ProfileTitleId(title);
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> named;
    for (auto it = std::filesystem::directory_iterator(directory, error); !error && it != std::filesystem::directory_iterator{}; it.increment(error)) {
        const auto name = it->path().filename().string();
        if (it->path().extension() != ".nxshader" || name.find(id) == std::string::npos || !it->is_regular_file(error)) continue;
        if (named.size() < 64) named.emplace_back(it->last_write_time(error), it->path());
    }
    std::sort(named.begin(), named.end(), std::greater<>{});
    for (const auto & [time, path] : named)
        if (std::find(result.begin(), result.end(), path) == result.end()) result.push_back(path);
    return result;
}
} // namespace
ShaderProfileDialog::ShaderProfileDialog(UiWindow & window, const std::filesystem::path & assets, std::function<void(const std::string &)> start) :
    Dialog(window.Context(), assets / "shader_profile_dialog.rml"), m_window(window), m_startGame(std::move(start))
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
void ShaderProfileDialog::Prepare(IVideo & video, bool importing, std::string game, uint64_t title, std::filesystem::path cache, std::string label)
{
    if (Busy()) return;
    CancelAndWait();
    RemoveStaging();
    m_video = &video;
    m_importing = importing;
    m_game = std::move(game);
    m_title = title;
    m_cache = std::move(cache);
    m_gameLabel = std::move(label);
    m_root = Utf8(std::filesystem::u8path(m_game).parent_path());
    m_cacheFile = Utf8(m_cache / "vulkan.nxc");
    // A first profile is version 1.0 of a complete recording; the author can change both.
    m_revision = "1.0";
    m_note.clear();
    m_fullGame = true;
    m_versions.clear();
    m_shares.clear();
    m_unavailableVersions = 0;
    m_installed.clear();
    m_installedInfo.clear();
    m_installedResult = {};
    m_complete = false;
    m_groupingFor.clear();
    m_outputs.clear();
    m_exportNotes.clear();
    m_saved.clear();
    if (!m_importing) {
        try {
            auto versions = Profile::ListRecordedVersions(m_cache, m_title, m_gameLabel);
            m_versions = std::move(versions.entries);
            m_unavailableVersions = versions.unavailable;
            for (size_t i = 1; i < m_versions.size(); ++i)
                m_shares.push_back(Profile::SharedPipelinePercent(m_versions[i - 1], m_versions[i]));
        } catch (const std::exception &) {
            m_unavailableVersions = std::max<size_t>(m_unavailableVersions, 1);
        }
        // Offer to complete the installed profile only when it is readable. Pick
        // it as game startup does for the version played last: the profile
        // exported from it, else one covering it, else the selected or newest one.
        try {
            const auto latest = std::max_element(m_versions.begin(), m_versions.end(),
                [](const auto & a, const auto & b) { return a.recorded < b.recorded; });
            const std::string context = latest == m_versions.end() ? std::string{} : latest->context;
            int best = -1;
            for (const auto & installed : InstalledProfiles(m_cache, m_title)) {
                if (m_versions.empty()) break;
                ShaderProfileTask inspect{};
                inspect.importing = true;
                inspect.title_id = m_title;
                inspect.game_root = m_root.c_str();
                const auto profile = Utf8(installed);
                inspect.profile = profile.c_str();
                inspect.context = context.c_str();
                char error[1024]{};
                struct : IShaderProfileProgress {
                    bool ProfileProgress(const char *, uint64_t, uint64_t) override { return true; }
                } quiet;
                ShaderProfileResult result{};
                if (!video.ProcessShaderProfile(inspect, quiet, result, error, sizeof(error))) continue;
                if (static_cast<int>(result.context_match) <= best) continue;
                best = static_cast<int>(result.context_match);
                m_installed = installed;
                m_installedResult = result;
                if (best == 2) break;
            }
            if (!m_installed.empty()) {
                m_installedInfo = Tr("%1 — %2 recipes.", {Utf8(m_installed.filename()),
                    std::to_string(m_installedResult.graphics + m_installedResult.compute)}) + " " + TargetsText(m_installedResult);
                const auto description = Description(m_installedResult.revision, m_installedResult.note);
                if (!description.empty()) m_installedInfo += " " + description;
            }
        } catch (const std::exception &) {
            m_installed.clear();
        }
    }
    m_step = !m_importing && !m_installed.empty() ? Step::Mode : Step::Options;
    m_cancel = false;
    m_finished = false;
    m_success = false;
    m_result = {};
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
    SetText("profile-title", Tr(m_importing ? "Import shader profile" : "Export shader profile"));
    SetText("profile-game", Escaped(m_gameLabel));
    SetText("profile-share-title", Tr("Share my recordings"));
    SetText("profile-share-text", Tr("You played the game yourself. Export what you recorded."));
    SetText("profile-complete-title", Tr("Complete the installed profile"));
    SetText("profile-complete-text", Tr("Add what you recorded to the profile you imported, then share the result.") + "<br/>" + Escaped(m_installedInfo));
    SetText("profile-version-label", Tr("Game versions"));
    SetText("profile-about", Tr("About this profile"));
    SetText("profile-full-label", Tr("Game completed"));
    SetText("profile-full-hint", Tr("Turn on if you played the whole game while recording, not just a part of it."));
    SetText("profile-revision-label", Tr("Profile version"));
    SetText("profile-revision-hint", Tr("Your own number, to tell updates of this profile apart. It is not the game version."));
    SetText("profile-note-label", Tr("Description") + " <span>" + Tr("(optional)") + "</span>");
    SetText("profile-note-hint", Tr("Players see it when they import the profile."));
    if (auto * revision = Document()->GetElementById("profile-revision")) revision->SetAttribute("placeholder", Tr("For example: 1.0"));
    if (auto * note = Document()->GetElementById("profile-note")) note->SetAttribute("placeholder", Tr("For example: all tracks, 200cc included"));
    if (auto * select = rmlui_dynamic_cast<Rml::ElementFormControlSelect *>(Document()->GetElementById("profile-version"))) {
        select->RemoveAll();
        if (m_versions.size() > 1) select->Add(Tr("All recorded versions"), "all");
        for (size_t i = 0; i < m_versions.size(); ++i) select->Add(Escaped(m_versions[i].label), std::to_string(i));
        if (!m_versions.empty()) select->SetSelection(0);
    }
    ShowStep(m_step);
}
void ShaderProfileDialog::ShowStep(Step step)
{
    m_step = step;
    const auto display = [&](const char * id, bool shown, const char * visible = "block") {
        if (auto * element = Document()->GetElementById(id)) element->SetProperty("display", shown ? visible : "none");
    };
    display("profile-mode", step == Step::Mode);
    display("profile-fields", step == Step::Options && !m_importing);
    display("profile-versions", m_versions.size() > 1);
    display("profile-start", step != Step::Mode, "inline-block");
    auto * start = Document()->GetElementById("profile-start");
    if (step == Step::Running) start->SetAttribute("disabled", "");
    else start->RemoveAttribute("disabled");
    std::string detail;
    switch (step) {
    case Step::Mode:
        detail = Tr("What do you want to share?");
        break;
    case Step::Options:
        if (m_importing) {
            detail = Tr("Select a profile. NXEmu verifies it against your local game before prewarming at game startup.");
            SetText("profile-start", Tr("Choose profile"));
            break;
        }
        detail = Tr("Your recordings become a small file you can share. It lists the shaders to prepare; "
                    "their code is read from each player's own copy of the game.");
        if (m_versions.empty())
            detail += "<br/>" + Tr("No identified recording is available. Play with collection enabled, stop the game, then export.");
        else if (m_versions.size() == 1)
            detail += "<br/>" + Tr("Recorded version: %1", {Escaped(m_versions.front().label)});
        if (m_unavailableVersions)
            detail += "<br/>" + Tr("%1 recordings have missing or invalid version metadata and cannot be exported.", {std::to_string(m_unavailableVersions)});
        if (auto * revision = rmlui_dynamic_cast<Rml::ElementFormControlInput *>(Document()->GetElementById("profile-revision")))
            revision->SetValue(m_complete ? std::string(m_installedResult.revision) : m_revision);
        if (auto * note = rmlui_dynamic_cast<Rml::ElementFormControlInput *>(Document()->GetElementById("profile-note")))
            note->SetValue(m_complete ? std::string(m_installedResult.note) : m_note);
        if (m_complete) m_fullGame = m_installedResult.full_game;
        if (auto * full = Document()->GetElementById("profile-full-game")) full->SetClass("on", m_fullGame);
        m_groupingFor.clear();
        UpdateGrouping();
        SetText("profile-start", Tr("Export"));
        break;
    case Step::Running:
        return;
    case Step::Save:
    case Step::Done: {
        if (m_importing) {
            detail = Tr("Profile selected. Local shaders will be verified when the game starts.");
            const auto description = Description(m_result.revision, m_result.note);
            if (!description.empty()) detail += "<br/>" + Escaped(description);
            if (*m_result.label) detail += "<br/>" + Escaped(m_result.label);
            if (m_result.full_game) detail += "<br/>" + Tr("Game completed: recorded over the whole game.");
            detail += "<br/>" + Tr("%1 graphics recipes, %2 compute recipes.",
                {std::to_string(m_result.graphics), std::to_string(m_result.compute)});
            detail += "<br/>" + TargetsText(m_result);
            SetText("profile-start", Tr("Start game"));
            break;
        }
        detail = m_outputs.size() == 1 ? Tr("One shader profile is ready.") : Tr("%1 shader profiles are ready.", {std::to_string(m_outputs.size())});
        for (const auto & output : m_outputs) {
            std::string versions;
            for (const auto index : output.versions) versions += (versions.empty() ? "" : ", ") + m_versions[index].short_label;
            if (output.installed) versions = Tr("installed profile") + ", " + versions;
            detail += "<br/>• " + Escaped(output.name) + "<br/>&nbsp;&nbsp;" + Escaped(versions) + " — " +
                Tr("%1 graphics, %2 compute recipes, %3", {std::to_string(output.result.graphics), std::to_string(output.result.compute), Megabytes(output.result.bytes)});
        }
        for (const auto & note : m_exportNotes) detail += "<br/>" + Escaped(note);
        if (!m_saved.empty()) detail += "<br/><br/>" + Tr("Saved in %1", {Escaped(m_saved)});
        SetText("profile-start", Tr(step == Step::Save ? "Save" : "Close"));
        break;
    }
    }
    SetText("profile-detail", detail);
}
// Explains, before exporting, how the chosen versions end up in files.
void ShaderProfileDialog::UpdateGrouping()
{
    if (m_step != Step::Options || m_importing) return;
    const auto * select = rmlui_dynamic_cast<Rml::ElementFormControlSelect *>(Document()->GetElementById("profile-version"));
    const auto chosen = select ? select->GetValue() : std::string("0");
    if (chosen == m_groupingFor) return;
    m_groupingFor = chosen;
    std::string text;
    if (chosen == "all") {
        for (size_t i = 0; i < m_shares.size(); ++i)
            text += Tr("%1 and %2 share %3 % of their shaders.", {Escaped(m_versions[i].short_label),
                Escaped(m_versions[i + 1].short_label), std::to_string(m_shares[i])}) + "<br/>";
        text += Tr("They are saved in one file. A version only gets its own file if the file would become too large.");
    } else if (m_versions.size() > 1)
        text = Tr("Only this version is exported.");
    if (m_complete) text += (text.empty() ? "" : "<br/>") + Tr("Your recordings are added to the installed profile.");
    SetText("profile-grouping", text);
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
    std::vector<size_t> selected;
    if (m_importing) {
        Path chosen;
        const auto filter = FileFilter({{Tr("Shader profile"), "*.nxshader"}});
        if (!FileSelect(m_window.NativeHandle(), m_root.c_str(), filter.c_str(), true, chosen)) return;
        m_profile = static_cast<const char *>(chosen);
    } else {
        if (m_versions.empty()) return;
        const auto value = [&](const char * id) {
            auto * input = rmlui_dynamic_cast<Rml::ElementFormControlInput *>(Document()->GetElementById(id));
            return input ? input->GetValue() : std::string{};
        };
        m_revision = value("profile-revision");
        m_note = value("profile-note");
        if (!Profile::ValidProfileText(m_revision, Profile::MaxRevision) || !Profile::ValidProfileText(m_note, Profile::MaxNote)) {
            SetText("profile-status", Tr("The profile version (32 characters) or the description (1024 characters) is too long, or has a line break."));
            return;
        }
        const auto * select = rmlui_dynamic_cast<Rml::ElementFormControlSelect *>(Document()->GetElementById("profile-version"));
        const auto chosen = select ? select->GetValue() : std::string("0");
        for (size_t i = 0; i < m_versions.size(); ++i)
            if (chosen == "all" || chosen == std::to_string(i)) selected.push_back(i);
        if (selected.empty()) selected.push_back(0);
    }
    m_cancel = false;
    m_finished = false;
    m_success = false;
    m_result = {};
    m_error.clear();
    m_busy = true;
    m_started = std::chrono::steady_clock::now();
    ShowStep(Step::Running);
    SetText("profile-status", Tr("Identifying recorded game version"));
    try {
        m_thread = m_importing ? std::thread([this] { RunImport(); }) : std::thread([this, selected] { RunExport(selected); });
    } catch (const std::exception & error) {
        m_busy = false;
        m_finished = true;
        m_error = error.what();
    }
}
void ShaderProfileDialog::RunImport()
{
    bool success = false;
    std::string failure;
    ShaderProfileResult result{};
    try {
        const auto snapshot = ReadSmall(std::filesystem::u8path(m_profile), Profile::MaxProfileFileBytes);
        const auto pending = Profile::ImportPendingPath(m_cache);
        AtomicWrite(pending, snapshot);
        const auto profile = Utf8(pending);
        ShaderProfileTask task{};
        task.importing = true;
        task.title_id = m_title;
        task.game_root = m_root.c_str();
        task.profile = profile.c_str();
        char error[1024]{};
        if (!m_video->ProcessShaderProfile(task, *this, result, error, sizeof(error))) throw std::runtime_error(error);
        if (m_cancel) throw std::runtime_error("Shader profile operation cancelled");
        // Keep the shared name so profiles of several versions can be installed side by side.
        auto name = Utf8(std::filesystem::u8path(m_profile).filename());
        if (name.find(Profile::ProfileTitleId(m_title)) == std::string::npos) name = Profile::ProfileFilename(m_title);
        const auto directory = ProfileDirectory();
        std::filesystem::create_directories(directory);
        AtomicWrite(directory / std::filesystem::u8path(name), snapshot);
        std::error_code error_code;
        std::filesystem::remove(pending, error_code);
        JsonValue selection;
        selection["file"] = name;
        selection["location"] = "nxshader";
        selection["root"] = m_root;
        AtomicWrite(Profile::ImportSelectionPath(m_cache), JsonStyledWriter().write(selection));
        success = true;
    } catch (const std::exception & error) {
        failure = error.what();
    }
    std::scoped_lock lock(m_mutex);
    m_success = success;
    m_error = std::move(failure);
    m_result = result;
    m_finished = true;
    m_busy = false;
}
// Exports the selected versions oldest first, each one merged into the file of
// the previous ones. When a merge would exceed the profile limits, that version
// starts a new file instead, so every version is kept whole.
void ShaderProfileDialog::RunExport(std::vector<size_t> selected)
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
            if (m_cancel) throw std::runtime_error("Shader profile operation cancelled");
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
            task.revision = m_revision.c_str();
            task.note = m_note.c_str();
            task.full_game = m_fullGame;
            task.previous = previous.empty() ? nullptr : previous_path.c_str();
            ShaderProfileResult result{};
            char error[1024]{};
            if (!m_video->ProcessShaderProfile(task, *this, result, error, sizeof(error)))
                throw std::runtime_error(version.short_label + ": " + error);
            return result;
        };
        ExportOutput group;
        group.installed = m_complete;
        std::filesystem::path previous = m_complete ? m_installed : std::filesystem::path{};
        for (const auto index : selected) {
            std::filesystem::path staged;
            ShaderProfileResult result{};
            bool too_large{};
            try {
                result = run(index, previous, group.versions, staged);
                too_large = !previous.empty() && result.dropped_versions;
            } catch (const std::exception &) {
                // A merge can also exceed the size of a profile file. If this
                // version exports alone, it starts a new file; otherwise the
                // error is its own and the retry below reports it.
                if (previous.empty() || m_cancel) throw;
                too_large = true;
            }
            if (!too_large && !previous.empty() && result.previous == ShaderProfilePrevious::Unreadable) {
                // Only the installed profile can be unreadable; staged parts were just written.
                notes.push_back(Tr("The installed profile cannot be read and was not included."));
                group.installed = false;
            } else if (too_large) {
                std::filesystem::remove(staged, error_code);
                if (group.versions.empty())
                    notes.push_back(Tr("The installed profile is too large to be completed; your recordings were exported in a new file."));
                else {
                    notes.push_back(Tr("%1 starts a new file: combined with the previous versions, the profile would exceed its limits.",
                        {m_versions[index].short_label}));
                    outputs.push_back(std::move(group));
                }
                group = {};
                result = run(index, {}, {}, staged);
            }
            group.versions.push_back(index);
            group.staged = staged;
            group.result = result;
            previous = staged;
        }
        outputs.push_back(std::move(group));
        std::map<std::string, size_t> names;
        for (auto & output : outputs) {
            if (output.installed) output.name = Utf8(m_installed.filename());
            else {
                std::vector<const Profile::RecordingVersion *> versions;
                for (const auto index : output.versions) versions.push_back(&m_versions[index]);
                output.name = Profile::CombinedProfileFilename(m_gameLabel, versions, m_title);
            }
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
    const auto filter = FileFilter({{Tr("Shader profile"), "*.nxshader"}});
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
            Tr("Choose the folder for the %1 shader profiles", {std::to_string(m_outputs.size())}).c_str()));
        if (directory.empty()) return false;
        for (const auto & output : m_outputs) targets.push_back(std::filesystem::u8path(directory) / std::filesystem::u8path(output.name));
    }
    std::string existing;
    for (const auto & target : targets)
        if (std::filesystem::exists(target)) existing += (existing.empty() ? "" : "\n") + Utf8(target.filename());
    if (!existing.empty() && !confirmed && (!g_notify || g_notify->Query(
            (Tr("Replace these existing shader profiles?") + "\n\n" + existing).c_str(),
            Tr("Replace shader profiles").c_str()) != NotificationResponse::Yes))
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
    if (m_step == Step::Options) {
        UpdateGrouping();
        return;
    }
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
                Tr(m_importing ? "The profile could not be imported" : "The profile could not be exported") +
                "</div><div class=\"profile-error-text\">" + Escaped(error) + "</div></div>");
            SetText("profile-start", Tr("Close"));
            return;
        }
        lock.unlock();
        SetText("profile-status", "");
        if (m_importing) {
            ShowStep(Step::Done);
            return;
        }
        // Ask where to save once the files and their names are known.
        ShowStep(Step::Save);
        if (SaveOutputs()) ShowStep(Step::Done);
        return;
    }
    const char * status = m_phase == "identify-content" ? "Identifying the game version" : m_phase == "read-cache" ? "Reading your shader cache"
        : m_phase == "assemble" ? "Preparing the recipes" : m_phase == "compress" ? "Compressing the profile"
        : m_phase == "write-profile" ? "Saving the profile" : m_phase == "verify-index" ? "Checking earlier results"
        : m_phase == "raw" ? "Searching the game files" : m_phase == "verify" ? "Checking the shaders" : "Searching the game resources";
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
    if (Busy()) return;
    if (command == "toggle-full-game") {
        m_fullGame = !m_fullGame;
        if (auto * full = Document()->GetElementById("profile-full-game")) full->SetClass("on", m_fullGame);
        return;
    }
    if (command == "mode-share" || command == "mode-complete") {
        m_complete = command == "mode-complete";
        ShowStep(Step::Options);
        return;
    }
    if (command != "start") return;
    switch (m_step) {
    case Step::Options:
        Start();
        break;
    case Step::Save:
        if (SaveOutputs()) ShowStep(Step::Done);
        break;
    case Step::Done: {
        const bool play = m_success && m_importing;
        RemoveStaging();
        Close();
        if (play) m_startGame(m_game);
        break;
    }
    default:
        break;
    }
}
