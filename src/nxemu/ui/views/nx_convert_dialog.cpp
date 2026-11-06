#include "nx_convert_dialog.h"
#include <RmlUi/Core/ElementDocument.h>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/ui/i18n/translation.h>
#include <nxemu/ui/platform/file_dialogs.h>
#include <common/path.h>
#include <common/std_string.h>
#include <system_error>
#include <yuzu_common/fs/fs_util.h>

namespace
{
constexpr const char * kTogglePrefix = "toggle-source:";
constexpr const char * kLevelPrefix = "level:";

std::string Escaped(const std::string & text)
{
    std::string result;
    for (const char c : text)
    {
        switch (c)
        {
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '&': result += "&amp;"; break;
        case '"': result += "&quot;"; break;
        default: result += c; break;
        }
    }
    return result;
}

std::string Bytes(uint64_t bytes)
{
    if (bytes >= 1024ull * 1024 * 1024)
    {
        return Tr("%1 GB", {stdstr_f("%.2f", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0))});
    }
    return Tr("%1 MB", {stdstr_f("%.0f", static_cast<double>(bytes) / (1024.0 * 1024.0))});
}

std::filesystem::path FsPath(const std::string & utf8)
{
    return std::filesystem::path{Common::FS::ToU8String(utf8)};
}

std::string Utf8(const std::filesystem::path & path)
{
    return Common::FS::PathToUTF8String(path);
}

std::string FileName(const std::string & utf8)
{
    return Utf8(FsPath(utf8).filename());
}

std::string Key(const std::string & utf8)
{
    std::string key = Utf8(FsPath(utf8).lexically_normal());
    std::replace(key.begin(), key.end(), '\\', '/');
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return key;
}

// A file name every system accepts.
std::string SafeName(const std::string & name)
{
    std::string result;
    for (const char c : name)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        result += (u < 0x20 || std::string("<>:\"/\\|?*").find(c) != std::string::npos) ? ' ' : c;
    }
    while (!result.empty() && (result.back() == ' ' || result.back() == '.'))
    {
        result.pop_back();
    }
    while (!result.empty() && result.front() == ' ')
    {
        result.erase(result.begin());
    }
    return result.empty() ? "game" : result;
}

std::string KindName(const std::string & kind)
{
    if (kind == "base")
    {
        return Tr("Game");
    }
    if (kind == "update")
    {
        return Tr("Update");
    }
    return Tr("DLC");
}

bool Exists(const std::string & utf8)
{
    std::error_code error;
    return std::filesystem::exists(FsPath(utf8), error);
}
} // namespace

NxConvertDialog::NxConvertDialog(UiWindow & window, const std::filesystem::path & assetDir, Host host) :
    Dialog(window.Context(), assetDir / "nx_convert_dialog.rml"),
    m_window(window),
    m_host(std::move(host))
{
}

void NxConvertDialog::Cancel()
{
    m_cancelled = true;
    m_tool.Cancel();
}

void NxConvertDialog::Offer(std::vector<std::string> paths, std::string focus)
{
    if (m_converting)
    {
        Show();
        return;
    }
    if (!IsVisible())
    {
        m_sources.clear();
        m_pendingScan.clear();
        m_focus.clear();
        Show();
    }
    if (!focus.empty())
    {
        m_sources.clear();
        m_focus = Key(focus);
    }
    Queue(paths);
}

bool NxConvertDialog::OfferDrop(const std::string & utf8Path)
{
    if (!IsVisible() || m_step != Step::Pick)
    {
        return false;
    }
    Queue({utf8Path});
    return true;
}

void NxConvertDialog::OnOpen()
{
    m_step = Step::Pick;
    m_converting = false;
    m_cancelled = false;
    m_changed = false;
    m_jobs.clear();
    if (!m_tool.Busy())
    {
        m_tool.Reset();
    }
    ShowStep(Step::Pick);
    ShowOptions();
    ShowSources();
}

void NxConvertDialog::Queue(const std::vector<std::string> & paths)
{
    for (const std::string & path : paths)
    {
        if (!path.empty())
        {
            m_pendingScan.push_back(path);
        }
    }
    ShowSources();
}

void NxConvertDialog::OnUpdate()
{
    const NxTool::State state = m_tool.Current();
    if (m_converting)
    {
        if (state.phase == NxTool::Phase::Done)
        {
            Job & job = m_jobs[m_job];
            job.done = true;
            job.succeeded = state.succeeded;
            if (state.succeeded)
            {
                std::error_code error;
                const uint64_t size = std::filesystem::file_size(FsPath(job.output), error);
                job.message = Tr("Saved as %1 (%2, the files were %3)", {FileName(job.output), Bytes(size), Bytes(job.sourceBytes)});
                m_changed = true;
            }
            else
            {
                job.message = state.cancelled ? Tr("Cancelled: nothing was written") : state.error;
            }
            m_tool.Reset();
            ++m_job;
            NextJob();
        }
        else
        {
            ShowWork(state);
        }
        return;
    }
    if (state.phase == NxTool::Phase::Done)
    {
        ScanFinished(state);
        m_tool.Reset();
    }
    if (!m_tool.Busy() && !m_pendingScan.empty())
    {
        m_scanning.swap(m_pendingScan);
        m_pendingScan.clear();
        std::vector<std::string> arguments{"scan"};
        arguments.insert(arguments.end(), m_scanning.begin(), m_scanning.end());
        if (!m_tool.Start(arguments))
        {
            ScanFinished(m_tool.Current());
            m_tool.Reset();
        }
        ShowSources();
    }
}

void NxConvertDialog::ScanFinished(const NxTool::State & state)
{
    const std::vector<std::string> scanned = std::move(m_scanning);
    m_scanning.clear();
    std::map<std::string, size_t> known;
    for (size_t i = 0; i < m_sources.size(); ++i)
    {
        known[Key(m_sources[i].path)] = i;
    }
    std::map<std::string, size_t> added;
    for (const std::vector<std::string> & record : state.records)
    {
        if (record[0] == "SOURCE" && record.size() >= 5)
        {
            const std::string key = Key(record[1]);
            if (known.count(key) != 0)
            {
                continue;
            }
            Source source;
            source.path = record[1];
            source.size = std::strtoull(record[2].c_str(), nullptr, 10);
            source.supported = record[4] == "Supported";
            source.selected = source.supported;
            known[key] = m_sources.size();
            added[key] = m_sources.size();
            m_sources.push_back(std::move(source));
        }
        else if (record[0] == "CONTENT" && record.size() >= 8)
        {
            // CONTENT path kind title application version display name
            const auto found = added.find(Key(record[1]));
            if (found == added.end())
            {
                continue;
            }
            Content content;
            content.kind = record[2];
            content.titleId = std::strtoull(record[3].c_str(), nullptr, 16);
            content.applicationId = std::strtoull(record[4].c_str(), nullptr, 16);
            content.version = static_cast<uint32_t>(std::strtoul(record[5].c_str(), nullptr, 10));
            content.displayVersion = record[6];
            content.name = record[7];
            m_sources[found->second].contents.push_back(std::move(content));
        }
        else if (record[0] == "ISSUE" && record.size() >= 3)
        {
            const auto found = added.find(Key(record[1]));
            if (found != added.end() && m_sources[found->second].issue.empty())
            {
                m_sources[found->second].issue = record[2];
            }
        }
    }
    if (!state.succeeded && !state.cancelled && added.empty())
    {
        Source failed;
        failed.path = scanned.empty() ? std::string() : scanned.front();
        failed.issue = state.error;
        failed.selected = false;
        m_sources.push_back(std::move(failed));
    }
    if (!m_focus.empty())
    {
        uint64_t application = 0;
        for (const Source & source : m_sources)
        {
            if (Key(source.path) == m_focus && !source.contents.empty())
            {
                application = source.contents.front().applicationId;
            }
        }
        if (application != 0)
        {
            std::erase_if(m_sources, [&](const Source & source) {
                return source.contents.empty() || source.contents.front().applicationId != application;
            });
            m_focus.clear();
        }
    }
    ShowSources();
}

std::vector<NxConvertDialog::Game> NxConvertDialog::Games() const
{
    std::vector<Game> games;
    for (size_t i = 0; i < m_sources.size(); ++i)
    {
        const Source & source = m_sources[i];
        if (source.contents.empty())
        {
            continue;
        }
        const uint64_t application = source.contents.front().applicationId;
        auto game = std::find_if(games.begin(), games.end(), [&](const Game & g) { return g.applicationId == application; });
        if (game == games.end())
        {
            games.push_back(Game{application});
            game = games.end() - 1;
        }
        game->sources.push_back(i);
        for (const Content & content : source.contents)
        {
            if (game->name.empty() || (content.kind == "base" && !content.name.empty()))
            {
                if (!content.name.empty())
                {
                    game->name = content.name;
                }
            }
        }
    }
    for (Game & game : games)
    {
        size_t bases = 0;
        std::string baseFolder;
        for (const size_t index : game.sources)
        {
            const Source & source = m_sources[index];
            for (const Content & content : source.contents)
            {
                if (source.selected && content.kind == "base")
                {
                    ++bases;
                    baseFolder = Utf8(FsPath(source.path).parent_path());
                }
            }
        }
        if (game.name.empty())
        {
            game.name = stdstr_f("%016llX", static_cast<unsigned long long>(game.applicationId));
        }
        if (bases == 0)
        {
            game.problem = Tr("Add the game itself (its base file): an NX holds the game with its updates and DLC.");
            continue;
        }
        if (bases > 1)
        {
            game.problem = Tr("Two files hold the game itself: keep only one of them.");
            continue;
        }
        const std::string folder = m_outputDir.empty() ? baseFolder : m_outputDir;
        game.output = Utf8(FsPath(folder) / FsPath(SafeName(game.name) + stdstr_f(" [%016llX].nx", static_cast<unsigned long long>(game.applicationId))));
        if (Exists(game.output))
        {
            game.problem = Tr("%1 already exists. Open it from the game list to add updates or DLC.", {FileName(game.output)});
            game.output.clear();
        }
    }
    return games;
}

void NxConvertDialog::OnCommand(const std::string & command)
{
    if (m_step == Step::Pick)
    {
        if (command == "add-files")
        {
            std::vector<std::string> files;
            const std::string filter = FileFilter({{Tr("Switch games"), "*.dxci;*.dnsp"}, {Tr("All files"), "*.*"}});
            if (FileSelectMany(m_window.NativeHandle(), static_cast<const char *>(Path(Path::CURRENT_DIRECTORY)), filter.c_str(), files))
            {
                Queue(files);
            }
        }
        else if (command == "add-folder")
        {
            const Path selected = BrowseForDirectory(m_window.NativeHandle(), Tr("Choose a folder of games").c_str());
            const std::string folder = static_cast<const char *>(selected);
            if (!folder.empty())
            {
                Queue({folder});
            }
        }
        else if (command == "clear" && !m_tool.Busy())
        {
            m_sources.clear();
            ShowSources();
        }
        else if (command.rfind(kTogglePrefix, 0) == 0)
        {
            const size_t index = static_cast<size_t>(std::atoi(command.c_str() + std::string(kTogglePrefix).size()));
            if (index < m_sources.size() && m_sources[index].supported)
            {
                m_sources[index].selected = !m_sources[index].selected;
                ShowSources();
            }
        }
        else if (command.rfind(kLevelPrefix, 0) == 0)
        {
            m_level = std::atoi(command.c_str() + std::string(kLevelPrefix).size());
            ShowOptions();
        }
        else if (command == "toggle-recipe")
        {
            m_recipe = !m_recipe;
            ShowOptions();
        }
        else if (command == "pick-output")
        {
            const Path selected = BrowseForDirectory(m_window.NativeHandle(), Tr("Choose where the NX files go").c_str());
            const std::string folder = static_cast<const char *>(selected);
            if (!folder.empty())
            {
                m_outputDir = folder;
                ShowOptions();
                ShowSources();
            }
        }
        else if (command == "output-beside")
        {
            m_outputDir.clear();
            ShowOptions();
            ShowSources();
        }
        else if (command == "convert")
        {
            StartConversion();
        }
    }
    else if (m_step == Step::Work && command == "cancel")
    {
        Cancel();
    }
    else if (m_step == Step::Done && command == "again")
    {
        m_sources.clear();
        m_jobs.clear();
        ShowStep(Step::Pick);
        ShowSources();
    }
}

void NxConvertDialog::OnCloseRequested()
{
    if (m_converting)
    {
        Cancel();
        return;
    }
    m_tool.Cancel();
    Close();
    if (m_changed)
    {
        m_changed = false;
        m_host.libraryChanged();
    }
}

void NxConvertDialog::ShowStep(Step step)
{
    m_step = step;
    if (Rml::Element * panel = Document()->GetElementById("nx-convert"))
    {
        panel->SetClass("at-pick", step == Step::Pick);
        panel->SetClass("at-work", step == Step::Work);
        panel->SetClass("at-done", step == Step::Done);
    }
}

void NxConvertDialog::ShowOptions()
{
    if (Document() == nullptr)
    {
        return;
    }
    for (const int level : {3, 9, 13})
    {
        if (Rml::Element * chip = Document()->GetElementById("level-" + std::to_string(level)))
        {
            chip->SetClass("selected", level == m_level);
        }
    }
    if (Rml::Element * recipe = Document()->GetElementById("recipe-switch"))
    {
        recipe->SetClass("on", m_recipe);
    }
    SetText("output-folder", Escaped(m_outputDir.empty() ? Tr("Beside each game's files") : m_outputDir));
    if (Rml::Element * beside = Document()->GetElementById("output-beside"))
    {
        beside->SetClass("hidden", m_outputDir.empty());
    }
}

void NxConvertDialog::ShowSources()
{
    if (Document() == nullptr)
    {
        return;
    }
    const bool scanning = m_tool.Busy() || !m_pendingScan.empty();
    std::string rml;
    size_t ready = 0;
    for (const Game & game : Games())
    {
        rml += std::string("<div class=\"nx-game") + (game.output.empty() ? " blocked" : "") + "\"><div class=\"nx-game-head\"><div class=\"nx-game-name\">" + Escaped(game.name) +
               "</div><div class=\"nx-game-id\">" + stdstr_f("%016llX", static_cast<unsigned long long>(game.applicationId)) + "</div></div>";
        for (const size_t index : game.sources)
        {
            const Source & source = m_sources[index];
            std::string what;
            for (const Content & content : source.contents)
            {
                if (!what.empty())
                {
                    what += " + ";
                }
                what += KindName(content.kind);
                if (content.kind == "dlc")
                {
                    what += stdstr_f(" %04llX", static_cast<unsigned long long>(content.titleId & 0xFFFF));
                }
                else if (!content.displayVersion.empty())
                {
                    what += " " + content.displayVersion;
                }
            }
            rml += std::string("<div class=\"nx-source") + (source.selected ? "" : " off") + "\"><div class=\"switch" + (source.selected ? " on" : "") + "\" data-command=\"" +
                   kTogglePrefix + std::to_string(index) + "\"><div class=\"switch-knob\"/></div><div class=\"nx-source-texts\"><div class=\"nx-source-what\">" + Escaped(what) +
                   "</div><div class=\"nx-source-file\">" + Escaped(FileName(source.path) + "  ·  " + Bytes(source.size)) + "</div></div></div>";
        }
        rml += "<div class=\"nx-game-note\">" + Escaped(game.output.empty() ? game.problem : Tr("Saved as %1", {FileName(game.output)})) + "</div></div>";
        ready += game.output.empty() ? 0 : 1;
    }
    for (const Source & source : m_sources)
    {
        if (!source.contents.empty())
        {
            continue;
        }
        rml += "<div class=\"nx-game blocked\"><div class=\"nx-game-head\"><div class=\"nx-game-name\">" + Escaped(source.path.empty() ? Tr("Files") : FileName(source.path)) +
               "</div></div><div class=\"nx-game-note\">" +
               Escaped(source.issue.empty() ? Tr("Not a decrypted DXCI or DNSP file: it cannot be converted.") : source.issue) + "</div></div>";
    }
    if (scanning)
    {
        rml += "<div class=\"nx-scanning\">" + Escaped(Tr("Reading the files...")) + "</div>";
    }
    else if (m_sources.empty())
    {
        rml = "<div class=\"nx-none\">" + Escaped(Tr("Add the game's files: the game itself, its updates and its DLC. They can be in several files; each game becomes one .nx file.")) + "</div>";
    }
    SetText("nx-sources", rml);
    if (Rml::Element * convert = Document()->GetElementById("convert-button"))
    {
        const bool enabled = ready != 0 && !scanning;
        convert->SetClass("disabled", !enabled);
        if (enabled)
        {
            convert->SetAttribute("data-command", "convert");
        }
        else
        {
            convert->RemoveAttribute("data-command");
        }
    }
    SetText("convert-button", Escaped(ready > 1 ? Tr("Convert %1 games", {std::to_string(ready)}) : Tr("Convert")));
}

void NxConvertDialog::StartConversion()
{
    if (m_tool.Busy() || !m_pendingScan.empty() || !m_host.canWork())
    {
        return;
    }
    m_jobs.clear();
    for (const Game & game : Games())
    {
        if (game.output.empty())
        {
            continue;
        }
        Job job;
        job.name = game.name;
        job.output = game.output;
        for (const size_t index : game.sources)
        {
            if (m_sources[index].selected)
            {
                job.sources.push_back(m_sources[index].path);
                job.sourceBytes += m_sources[index].size;
            }
        }
        m_jobs.push_back(std::move(job));
    }
    if (m_jobs.empty())
    {
        return;
    }
    m_job = 0;
    m_cancelled = false;
    m_converting = true;
    ShowStep(Step::Work);
    NextJob();
}

void NxConvertDialog::NextJob()
{
    while (m_job < m_jobs.size() && !m_cancelled)
    {
        Job & job = m_jobs[m_job];
        std::vector<std::string> arguments{"pack"};
        arguments.insert(arguments.end(), job.sources.begin(), job.sources.end());
        arguments.push_back(job.output);
        arguments.push_back("--level");
        arguments.push_back(std::to_string(m_level));
        if (m_recipe)
        {
            arguments.push_back("--recipe");
        }
        m_lastProgress.clear();
        if (m_tool.Start(arguments))
        {
            ShowWork(m_tool.Current());
            return;
        }
        job.done = true;
        job.message = m_tool.Current().error;
        m_tool.Reset();
        ++m_job;
    }
    for (Job & job : m_jobs)
    {
        if (!job.done)
        {
            job.done = true;
            job.message = Tr("Cancelled: nothing was written");
        }
    }
    m_converting = false;
    ShowStep(Step::Done);
    ShowResults();
}

void NxConvertDialog::ShowWork(const NxTool::State & state)
{
    if (m_job >= m_jobs.size())
    {
        return;
    }
    const Job & job = m_jobs[m_job];
    const std::string heading = m_jobs.size() > 1 ? Tr("Converting %1 (%2 of %3)...", {job.name, std::to_string(m_job + 1), std::to_string(m_jobs.size())}) : Tr("Converting %1...", {job.name});
    const std::string detail = state.progress != 0 ? Tr("%1 written", {Bytes(state.progress)}) : Tr("Reading the files...");
    if (heading + detail == m_lastProgress)
    {
        return;
    }
    m_lastProgress = heading + detail;
    SetText("work-heading", Escaped(heading));
    SetText("work-detail", Escaped(detail));
}

void NxConvertDialog::ShowResults()
{
    std::string rml;
    size_t succeeded = 0;
    for (const Job & job : m_jobs)
    {
        succeeded += job.succeeded ? 1 : 0;
        rml += std::string("<div class=\"nx-result") + (job.succeeded ? "" : " failed") + "\"><img src=\"images/" + (job.succeeded ? "check" : "alert") +
               ".png\" /><div class=\"nx-result-texts\"><div class=\"nx-result-name\">" + Escaped(job.name) + "</div><div class=\"nx-result-text\">" + Escaped(job.message) +
               "</div></div></div>";
    }
    SetText("results", rml);
    SetText("result-heading", Escaped(succeeded == m_jobs.size() ? (succeeded > 1 ? Tr("%1 games converted", {std::to_string(succeeded)}) : Tr("Game converted"))
                                                                : Tr("%1 of %2 games converted", {std::to_string(succeeded), std::to_string(m_jobs.size())})));
    SetText("result-text", Escaped(succeeded != 0 ? Tr("The NX files appear in the game list once their folder is one of the game directories. The original files were left as they were.")
                                                  : Tr("The original files were left as they were.")));
}
