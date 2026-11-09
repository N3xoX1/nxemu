#include "nx_convert_dialog.h"
#include <RmlUi/Core/ElementDocument.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <tuple>
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
constexpr std::string_view kFirmwarePrefix = "toggle-firmware:";

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

void NxConvertDialog::Offer(std::string game)
{
    if (m_converting)
    {
        Show();
        return;
    }
    if (!IsVisible())
    {
        m_sources.clear();
        m_omitFirmware.clear();
        m_pendingScan.clear();
        m_pendingGame.clear();
        Show();
    }
    if (!game.empty())
    {
        m_pendingGame = std::move(game);
        ShowSources();
    }
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
        m_discardScan = false;
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
    const NxTool::State state = m_converting ? m_tool.Current() : m_tool.TakeState();
    if (m_converting)
    {
        if (state.phase == NxTool::Phase::Done)
        {
            Job & job = m_jobs[m_job];
            job.succeeded = state.succeeded;
            job.retryable = !state.succeeded && !state.cancelled;
            job.canRetry = job.recipe && !job.omitFirmware && CanRetryWithoutRecipe(state);
            if (state.succeeded)
            {
                job.outputStamp = Stamp(job.output);
                m_changed = true;
            }
            else
            {
                job.message = state.cancelled ? Tr("Cancelled: nothing was written") : state.error;
            }
            job.done = true;
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
    if (!m_discardScan && !state.records.empty())
    {
        AppendScanResults(state);
    }
    if (state.phase == NxTool::Phase::Done)
    {
        m_tool.Reset();
        m_discardScan = false;
        ShowSources();
    }
    if (state.phase != NxTool::Phase::Running && !m_pendingGame.empty())
    {
        if (m_tool.Start({"scan-game", m_pendingGame}, KnownSources()))
        {
            m_pendingGame.clear();
        }
        ShowSources();
    }
    else if (state.phase != NxTool::Phase::Running && !m_pendingScan.empty())
    {
        std::vector<std::string> arguments{"scan"};
        arguments.insert(arguments.end(), m_pendingScan.begin(), m_pendingScan.end());
        if (m_tool.Start(std::move(arguments), KnownSources()))
        {
            m_pendingScan.clear();
        }
        ShowSources();
    }
}

std::vector<NxTool::ScanSource> NxConvertDialog::KnownSources() const
{
    std::vector<NxTool::ScanSource> known;
    for (const Source & source : m_sources)
    {
        if (!source.signature.empty() && source.duplicateOf.empty())
            known.push_back({source.path, std::to_string(source.size), source.signature});
    }
    return known;
}

void NxConvertDialog::AppendScanResults(const NxTool::State & state)
{
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
            if (record.size() >= 6) source.signature = record[5];
            if (record.size() >= 7) source.duplicateOf = record[6];
            source.selected = source.supported && source.duplicateOf.empty();
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
        else if (record[0] == "FIRMWARE" && record.size() >= 3)
        {
            const auto found = added.find(Key(record[1]));
            if (found != added.end())
                m_sources[found->second].firmwareBytes = std::strtoull(record[2].c_str(), nullptr, 10);
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
        std::string baseFolder = Utf8(FsPath(m_sources[game.sources.front()].path).parent_path());
        for (const size_t index : game.sources)
        {
            const Source & source = m_sources[index];
            if (!source.supported) continue;
            const bool hasBase = std::any_of(source.contents.begin(), source.contents.end(), [](const Content & content) {
                return content.kind == "base";
            });
            if (hasBase && source.selected) baseFolder = Utf8(FsPath(source.path).parent_path());
            for (const Content & content : source.contents)
            {
                if (content.kind == "update") game.updateVersions.insert(content.version);
            }
            if (!source.selected) continue;
            const bool primarySelected = !source.duplicateOf.empty() && std::any_of(game.sources.begin(), game.sources.end(), [&](size_t other) {
                return m_sources[other].selected && Key(m_sources[other].path) == Key(source.duplicateOf);
            });
            if (primarySelected) continue;
            game.selected.push_back(index);
            if (hasBase) ++bases;
        }
        const std::string id = stdstr_f("%016llX", static_cast<unsigned long long>(game.applicationId));
        if (game.name.empty()) game.name = id;
        const std::string folder = m_outputDir.empty() ? baseFolder : m_outputDir;
        game.output = Utf8(FsPath(folder) / FsPath(SafeName(game.name) + " [" + id + "].nx"));
        if (Exists(game.output))
        {
            game.problem = Tr("%1 already exists. Choose a different destination folder.", {FileName(game.output)});
            game.output.clear();
        }
        else if (bases != 1)
        {
            game.problem = bases == 0 ? Tr("Add the game itself (its base file): an NX holds the game with its updates and DLC.")
                                      : Tr("Two files hold the game itself: keep only one of them.");
            game.output.clear();
        }
        for (const size_t index : game.selected)
            game.firmwareBytes += m_sources[index].firmwareBytes;
        if (!game.output.empty())
        {
            std::set<std::tuple<std::string, uint64_t, uint32_t>> identities;
            for (const size_t index : game.selected)
            {
                for (const Content & content : m_sources[index].contents)
                {
                    if (content.kind != "base" && !identities.emplace(content.kind, content.titleId, content.version).second)
                    {
                        game.problem = Tr("Several files have the same content version. Select one copy of each version.");
                        game.output.clear();
                    }
                }
            }
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
        else if (command.rfind(kTogglePrefix, 0) == 0)
        {
            const size_t index = static_cast<size_t>(std::atoi(command.c_str() + std::string(kTogglePrefix).size()));
            if (index < m_sources.size() && m_sources[index].supported)
            {
                Source & source = m_sources[index];
                source.selected = !source.selected;
                if (source.selected)
                {
                    const std::string primary = Key(source.duplicateOf.empty() ? source.path : source.duplicateOf);
                    for (Source & other : m_sources)
                    {
                        if (&other != &source && Key(other.duplicateOf.empty() ? other.path : other.duplicateOf) == primary)
                            other.selected = false;
                    }
                }
                ShowSources();
            }
        }
        else if (command.starts_with(kFirmwarePrefix))
        {
            const auto id = std::strtoull(command.c_str() + kFirmwarePrefix.size(), nullptr, 10);
            if (m_omitFirmware.erase(id) == 0) m_omitFirmware.insert(id);
            ShowSources();
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
            ShowSources();
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
    else if (m_step == Step::Done && (command == "retry-failed" || command == "retry-without-recipe") && m_host.canWork())
    {
        RetryJobs(command == "retry-without-recipe");
    }
    else if (m_step == Step::Done && command == "finish")
    {
        const auto * checkbox = Document()->GetElementById("trash-originals");
        if (checkbox && checkbox->HasAttribute("checked") && !TrashOriginals())
            return;
        OnCloseRequested();
    }
}

void NxConvertDialog::OnCloseRequested()
{
    if (m_converting)
    {
        Cancel();
        return;
    }
    m_discardScan = m_tool.Busy();
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
    for (const int level : {1, 3, 13})
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
    const bool scanning = m_tool.Busy() || !m_pendingGame.empty() || !m_pendingScan.empty();
    std::string rml;
    size_t ready = 0;
    std::string outputFiles;
    for (const Game & game : Games())
    {
        rml += std::string("<div class=\"nx-game") + (game.output.empty() ? " blocked" : "") + "\"><div class=\"nx-game-head\"><div class=\"nx-game-name\">" + Escaped(game.name) +
               "</div><div class=\"nx-game-id\">" + stdstr_f("%016llX", static_cast<unsigned long long>(game.applicationId)) + "</div></div>";
        bool firmwareShown = false;
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
                if (!content.displayVersion.empty())
                    what += " " + content.displayVersion;
                else if (content.kind == "update")
                    what += " " + Tr("Version %1", {std::to_string(content.version)});
            }
            if (!source.duplicateOf.empty()) what += " · " + Tr("Identical to %1", {FileName(source.duplicateOf)});
            rml += std::string("<div class=\"nx-source") + (source.selected ? "" : " off") + "\"><div class=\"switch" + (source.selected ? " on" : "") + "\" data-command=\"" +
                   kTogglePrefix + std::to_string(index) + "\"><div class=\"switch-knob\"/></div><div class=\"nx-source-texts\"><div class=\"nx-source-what\">" + Escaped(what) +
                   "</div><div class=\"nx-source-file\">" + Escaped(FileName(source.path) + "  ·  " + Bytes(source.size)) + "</div></div></div>";
            if (!firmwareShown && source.firmwareBytes != 0 &&
                std::find(game.selected.begin(), game.selected.end(), index) != game.selected.end())
            {
                firmwareShown = true;
                const bool include = !m_omitFirmware.contains(game.applicationId);
                rml += std::string("<div class=\"nx-source") + (include ? "" : " off") + "\"><div class=\"switch" + (include ? " on" : "") +
                    "\" data-command=\"" + std::string(kFirmwarePrefix) + std::to_string(game.applicationId) +
                    "\"><div class=\"switch-knob\"/></div><div class=\"nx-source-texts\"><div class=\"nx-source-what\">" +
                    Escaped(Tr("Bundled firmware") + " · " + Bytes(game.firmwareBytes)) + "</div>";
                if (!include && m_recipe)
                    rml += "<div class=\"nx-source-file\">" + Escaped(Tr("Omitting firmware disables rebuild data for this game.")) + "</div>";
                rml += "</div></div>";
            }
        }
        if (game.updateVersions.size() > 1)
            rml += "<div class=\"nx-game-note\">" + Escaped(Tr("Several update versions are available. Keep the versions you want.")) + "</div>";
        if (game.output.empty())
        {
            rml += "<div class=\"nx-game-note\">" + Escaped(game.problem) + "</div>";
        }
        else
        {
            ++ready;
            const std::string label = Escaped(Tr("Save as %1", {FileName(game.output)}));
            outputFiles += "<div title=\"" + label + "\">" + label + "</div>";
        }
        rml += "</div>";
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
    SetText("output-files", outputFiles);
    if (auto * output = Document()->GetElementById("output-files"))
        output->SetClass("hidden", outputFiles.empty());
}

void NxConvertDialog::StartConversion()
{
    if (m_tool.Busy() || !m_pendingGame.empty() || !m_pendingScan.empty() || !m_host.canWork())
    {
        return;
    }
    m_jobs.clear();
    m_confirmTrash.clear();
    m_trashResult.clear();
    for (const Game & game : Games())
    {
        if (game.output.empty())
        {
            continue;
        }
        Job job;
        job.name = game.name;
        job.recipe = m_recipe;
        job.omitFirmware = game.firmwareBytes != 0 && m_omitFirmware.contains(game.applicationId);
        job.output = game.output;
        for (const size_t index : game.selected)
        {
            const auto & source = m_sources[index];
            job.sources.push_back(source.path);
            if (const auto stamp = Stamp(source.path)) job.sourceStamps.emplace(source.path, *stamp);
            job.sourceBytes += source.size;
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

void NxConvertDialog::RetryJobs(bool withoutRecipe)
{
    bool found = false;
    for (Job & job : m_jobs)
    {
        if (!(withoutRecipe ? job.canRetry : job.retryable)) continue;
        job.canRetry = job.retryable = false;
        job.done = false;
        if (withoutRecipe) job.recipe = false;
        found = true;
    }
    if (!found) return;
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
        if (job.done)
        {
            ++m_job;
            continue;
        }
        std::vector<std::string> arguments{"pack"};
        arguments.insert(arguments.end(), job.sources.begin(), job.sources.end());
        arguments.push_back(job.output);
        arguments.push_back("--level");
        arguments.push_back(std::to_string(m_level));
        if (job.omitFirmware) arguments.push_back("--omit-firmware");
        arguments.push_back(job.recipe && !job.omitFirmware ? "--recipe" : "--no-recipe");
        m_lastProgress.clear();
        if (m_tool.Start(arguments))
        {
            ShowWork(m_tool.Current());
            return;
        }
        job.done = job.retryable = true;
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
    const std::string heading = Tr("Converting %1...", {job.name});
    std::string detail = ProgressText(state);
    double totalWeight = 0, finishedWeight = 0, remainingWeight = 0;
    for (const Job & item : m_jobs)
    {
        const double weight = std::max<uint64_t>(1, item.sourceBytes);
        totalWeight += weight;
        if (item.done) finishedWeight += weight;
        else if (&item != &job) remainingWeight += weight;
    }
    const double fraction = state.total == 0 ? 0.0 : std::min(0.99, state.Fraction());
    const double weight = std::max<uint64_t>(1, job.sourceBytes);
    const int percent = std::min(99, static_cast<int>(100 * (finishedWeight + weight * fraction) / totalWeight));
    detail += " · " + Tr("Batch %1 of %2 (%3 %)", {std::to_string(m_job + 1), std::to_string(m_jobs.size()), std::to_string(percent)});
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.started).count();
    if (fraction >= 0.02 && elapsed >= 3)
    {
        const double seconds = elapsed * (remainingWeight / weight + 1 - fraction) / fraction;
        if (seconds < 60)
            detail += " · " + Tr("About %1 s remaining", {std::to_string(static_cast<int>(seconds + 0.5))});
        else
            detail += " · " + Tr("About %1 min remaining", {std::to_string(static_cast<uint64_t>(seconds / 60 + 0.5))});
    }
    else
        detail += " · " + Tr("Estimating time remaining...");
    if (heading + detail == m_lastProgress)
    {
        return;
    }
    m_lastProgress = heading + detail;
    if (Document())
    {
        if (auto * fill = Document()->GetElementById("batch-fill"))
            fill->SetProperty("width", std::to_string(percent) + "%");
    }
    SetText("work-heading", Escaped(heading));
    SetText("work-detail", Escaped(detail));
}

std::optional<NxConvertDialog::FileStamp> NxConvertDialog::Stamp(const std::string & path)
{
    const auto file = FsPath(path);
    std::error_code error;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(file, error))) return std::nullopt;
    const auto size = std::filesystem::file_size(file, error);
    if (error) return std::nullopt;
    const auto modified = std::filesystem::last_write_time(file, error);
    if (error) return std::nullopt;
    return FileStamp{size, modified};
}

std::vector<std::string> NxConvertDialog::TrashCandidates() const
{
    std::vector<std::string> candidates;
    std::set<std::string> seen;
    for (const Job & job : m_jobs)
    {
        if (!job.succeeded || !job.outputStamp || Stamp(job.output) != job.outputStamp) continue;
        for (const auto & [path, stamp] : job.sourceStamps)
        {
            if (Stamp(path) == stamp && seen.insert(Key(path)).second) candidates.push_back(path);
        }
    }
    return candidates;
}

bool NxConvertDialog::TrashOriginals()
{
    if (m_confirmTrash.empty()) return true;
    if (!m_host.canWork()) return false;
    const auto candidates = TrashCandidates();
    size_t moved = 0;
    std::string failures;
    for (const auto & path : m_confirmTrash)
    {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(FsPath(path), error);
        if (!error && std::find(candidates.begin(), candidates.end(), path) != candidates.end() &&
            MoveFileToTrash(m_window.NativeHandle(), Utf8(absolute)))
        {
            ++moved;
        }
        else
        {
            if (!failures.empty()) failures += ", ";
            failures += FileName(path);
        }
    }
    m_confirmTrash.clear();
    m_trashResult = TrCount("%n original file moved to the trash.", "%n original files moved to the trash.", moved);
    if (failures.empty()) return true;
    m_trashResult += " " + Tr("Could not move to the trash: %1", {failures});
    ShowResults();
    return false;
}

void NxConvertDialog::ShowResults()
{
    std::string rml;
    size_t succeeded = 0;
    for (const Job & job : m_jobs)
    {
        succeeded += job.succeeded;
        if (job.succeeded)
        {
            rml += "<div class=\"nx-result-item\"><div class=\"nx-result-text\">" +
                Escaped(Tr("Saved as %1", {FileName(job.output)})) + "</div>";
            if (job.outputStamp)
            {
                std::string size = Tr("Size: %1 · original: %2", {Bytes(job.outputStamp->size), Bytes(job.sourceBytes)});
                // The change in whole percent, left out when it rounds to zero.
                if (job.sourceBytes != 0)
                {
                    const double change = 100.0 * (static_cast<double>(job.outputStamp->size) - static_cast<double>(job.sourceBytes)) / static_cast<double>(job.sourceBytes);
                    const long long rounded = std::llround(change);
                    if (rounded != 0)
                        size += stdstr_f(" (%s%lld %%)", rounded > 0 ? "+" : "\xE2\x88\x92", rounded > 0 ? rounded : -rounded);
                }
                rml += "<div class=\"nx-result-size\">" + Escaped(size) + "</div>";
            }
            rml += "</div>";
        }
        else
        {
            rml += "<div class=\"nx-result-text\">" + Escaped(job.name + " · " + job.message) + "</div>";
        }
    }
    if (std::any_of(m_jobs.begin(), m_jobs.end(), [](const Job & job) { return job.retryable; }))
        rml += "<button data-command=\"retry-failed\">" + Escaped(Tr("Retry failed jobs")) + "</button> ";
    if (std::any_of(m_jobs.begin(), m_jobs.end(), [](const Job & job) { return job.canRetry; }))
    {
        rml += "<button data-command=\"retry-without-recipe\">" + Escaped(Tr("Retry without rebuild data")) + "</button>";
    }
    SetText("results", rml);
    const bool success = succeeded == m_jobs.size();
    if (auto * icon = Document()->GetElementById("result-icon"))
    {
        icon->SetAttribute("src", success ? "images/check.png" : "images/alert.png");
        icon->SetClass("failed", !success);
    }
    SetText("result-heading", Escaped(success ? Tr("Conversion completed successfully")
                                             : Tr("%1 of %2 games converted", {std::to_string(succeeded), std::to_string(m_jobs.size())})));
    m_confirmTrash = TrashCandidates();
    if (auto * row = Document()->GetElementById("trash-option"))
        row->SetClass("hidden", m_confirmTrash.empty());
    if (auto * checkbox = Document()->GetElementById("trash-originals"))
    {
        if (m_confirmTrash.empty()) checkbox->RemoveAttribute("checked");
        else checkbox->SetAttribute("checked", "");
    }
    SetText("result-text", Escaped(m_trashResult));
}
