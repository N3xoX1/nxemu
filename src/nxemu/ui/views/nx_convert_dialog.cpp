#include "nx_convert_dialog.h"
#include <RmlUi/Core/ElementDocument.h>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
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
constexpr const char * kArchivePrefix = "pick-archive:";
constexpr const char * kClearArchivePrefix = "clear-archive:";
constexpr std::string_view kEditResultPrefix = "edit-result:";
constexpr std::string_view kManageResultPrefix = "manage-result:";

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
        m_existingArchives.clear();
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
            if (job.inspecting)
            {
                job.inspecting = false;
                job.inspected = true;
                job.missingSources.clear();
                bool matches = false;
                std::set<Job::Identity> present;
                for (const auto & row : state.records)
                {
                    if (row.size() < 6 || row[0] != "SNAPSHOT" ||
                        std::strtoull(row[3].c_str(), nullptr, 16) != job.applicationId) continue;
                    matches |= row[2] == "base";
                    present.emplace(row[2], std::strtoull(row[4].c_str(), nullptr, 16), std::strtoul(row[5].c_str(), nullptr, 10));
                }
                if (state.succeeded && matches)
                {
                    for (const auto & path : job.sources)
                    {
                        const auto & contents = job.sourceContents.at(path);
                        if (std::any_of(contents.begin(), contents.end(), [&](const auto & entry) { return !present.contains(entry); }))
                            job.missingSources.push_back(path);
                    }
                    if (!job.missingSources.empty())
                    {
                        m_tool.Reset();
                        NextJob();
                        return;
                    }
                    job.succeeded = true;
                    job.outputStamp = Stamp(job.output);
                    job.message = job.added.empty() ? Tr("The NX already contains the selected updates and DLC.")
                                                    : Tr("Updated %1.", {FileName(job.output)});
                }
                else
                {
                    job.retryable = !state.cancelled;
                    job.message = state.cancelled ? Tr("Cancelled: nothing was written") :
                        state.succeeded ? Tr("The existing NX does not contain this base game.") : state.error;
                }
            }
            else
            {
                for (const auto & row : state.records)
                {
                    if (row.size() >= 4 && row[0] == "ADDED")
                    {
                        job.added.emplace(row[1], std::strtoull(row[2].c_str(), nullptr, 16), std::strtoul(row[3].c_str(), nullptr, 10));
                        m_changed = true;
                    }
                }
                job.succeeded = state.succeeded;
                job.retryable = !state.succeeded && !state.cancelled;
                job.canRetry = job.recipe && CanRetryWithoutRecipe(state);
                if (state.succeeded)
                {
                    job.outputStamp = Stamp(job.output);
                    const uint64_t size = job.outputStamp ? job.outputStamp->size : 0;
                    if (job.add)
                        job.message = job.added.empty() ? Tr("The NX already contains the selected updates and DLC.")
                                                        : Tr("Updated %1.", {FileName(job.output)});
                    else
                        job.message = Tr("Saved as %1 (%2, the files were %3)", {FileName(job.output), Bytes(size), Bytes(job.sourceBytes)});
                    m_changed = true;
                }
                else if (job.add && !job.added.empty())
                {
                    job.message = state.cancelled ? Tr("Cancelled: some content was already added.")
                                                  : Tr("Some content was added before the error: %1", {state.error});
                }
                else
                {
                    job.message = state.cancelled ? Tr("Cancelled: nothing was written") : state.error;
                }
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
        std::vector<size_t> addOns;
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
            else addOns.push_back(index);
        }
        const std::string id = stdstr_f("%016llX", static_cast<unsigned long long>(game.applicationId));
        if (game.name.empty()) game.name = id;
        const std::string folder = m_outputDir.empty() ? baseFolder : m_outputDir;
        game.output = Utf8(FsPath(folder) / FsPath(SafeName(game.name) + " [" + id + "].nx"));
        game.canChooseArchive = !addOns.empty();
        const auto explicitArchive = m_existingArchives.find(game.applicationId);
        if (explicitArchive != m_existingArchives.end()) game.output = explicitArchive->second;
        if (explicitArchive != m_existingArchives.end() || Exists(game.output))
        {
            game.add = true;
            game.selected = std::move(addOns);
            if (game.selected.empty())
            {
                game.problem = Tr("Select updates or DLC to add to %1.", {FileName(game.output)});
                game.output.clear();
            }
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
            std::set<Job::Identity> identities;
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
        else if (command == "clear" && !m_tool.Busy())
        {
            m_sources.clear();
            m_existingArchives.clear();
            m_omitFirmware.clear();
            ShowSources();
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
        else if (command.rfind(kArchivePrefix, 0) == 0)
        {
            const uint64_t id = std::strtoull(command.c_str() + std::char_traits<char>::length(kArchivePrefix), nullptr, 10);
            Path selected;
            const std::string filter = FileFilter({{Tr("NX archive"), "*.nx"}});
            if (FileSelect(m_window.NativeHandle(), m_outputDir.c_str(), filter.c_str(), true, selected))
            {
                m_existingArchives[id] = static_cast<const char *>(selected);
                ShowSources();
            }
        }
        else if (command.rfind(kClearArchivePrefix, 0) == 0)
        {
            const uint64_t id = std::strtoull(command.c_str() + std::char_traits<char>::length(kClearArchivePrefix), nullptr, 10);
            m_existingArchives.erase(id);
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
    else if (m_step == Step::Done && m_host.canWork() &&
             (command.starts_with(kEditResultPrefix) || command.starts_with(kManageResultPrefix)))
    {
        const bool edit = command.starts_with(kEditResultPrefix);
        const auto prefix = edit ? kEditResultPrefix : kManageResultPrefix;
        const size_t index = std::strtoull(command.c_str() + prefix.size(), nullptr, 10);
        if (index < m_jobs.size() && m_jobs[index].succeeded && Exists(m_jobs[index].output))
            m_host.openArchive(m_jobs[index].output, edit);
    }
    else if (m_step == Step::Done && command == "ask-trash")
    {
        m_confirmTrash = TrashCandidates();
        ShowResults();
    }
    else if (m_step == Step::Done && command == "keep-originals")
    {
        m_confirmTrash.clear();
        ShowResults();
    }
    else if (m_step == Step::Done && command == "confirm-trash")
    {
        TrashOriginals();
    }
    else if (m_step == Step::Done && command == "again")
    {
        m_sources.clear();
        m_existingArchives.clear();
        m_omitFirmware.clear();
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
    size_t ready = 0, existing = 0;
    for (const Game & game : Games())
    {
        rml += std::string("<div class=\"nx-game") + (game.output.empty() ? " blocked" : "") + "\"><div class=\"nx-game-head\"><div class=\"nx-game-name\">" + Escaped(game.name) +
               "</div><div class=\"nx-game-id\">" + stdstr_f("%016llX", static_cast<unsigned long long>(game.applicationId)) + "</div></div>";
        if (game.firmwareBytes != 0)
        {
            const bool include = !m_omitFirmware.contains(game.applicationId);
            rml += std::string("<div class=\"nx-source") + (include ? "" : " off") + "\"><div class=\"switch" + (include ? " on" : "") +
                "\" data-command=\"" + std::string(kFirmwarePrefix) + std::to_string(game.applicationId) +
                "\"><div class=\"switch-knob\"/></div><div class=\"nx-source-texts\"><div class=\"nx-source-what\">" +
                Escaped(Tr("Bundled firmware") + " · " + Bytes(game.firmwareBytes)) + "</div>";
            if (!include && m_recipe)
                rml += "<div class=\"nx-source-file\">" + Escaped(Tr("Omitting firmware disables rebuild data for this game.")) + "</div>";
            rml += "</div></div>";
        }
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
            if (source.firmwareBytes != 0) what += " · " + Tr("Bundled firmware");
            if (!source.duplicateOf.empty()) what += " · " + Tr("Identical to %1", {FileName(source.duplicateOf)});
            rml += std::string("<div class=\"nx-source") + (source.selected ? "" : " off") + "\"><div class=\"switch" + (source.selected ? " on" : "") + "\" data-command=\"" +
                   kTogglePrefix + std::to_string(index) + "\"><div class=\"switch-knob\"/></div><div class=\"nx-source-texts\"><div class=\"nx-source-what\">" + Escaped(what) +
                   "</div><div class=\"nx-source-file\">" + Escaped(FileName(source.path) + "  ·  " + Bytes(source.size)) + "</div></div></div>";
        }
        if (game.updateVersions.size() > 1)
            rml += "<div class=\"nx-game-note\">" + Escaped(Tr("Several update versions are available. Keep the versions you want.")) + "</div>";
        const std::string action = game.output.empty() ? game.problem :
            game.add ? Tr("Add missing updates and DLC to %1.", {FileName(game.output)}) : Tr("Saved as %1", {FileName(game.output)});
        rml += "<div class=\"nx-game-note\">" + Escaped(action) + "</div>";
        if (game.canChooseArchive)
            rml += "<button data-command=\"" + std::string(kArchivePrefix) + std::to_string(game.applicationId) + "\">" +
                Escaped(Tr("Choose an existing NX")) + "</button> ";
        if (m_existingArchives.contains(game.applicationId))
            rml += "<button data-command=\"" + std::string(kClearArchivePrefix) + std::to_string(game.applicationId) + "\">" +
                Escaped(Tr("Use default destination")) + "</button>";
        rml += "</div>";
        ready += !game.output.empty();
        existing += game.add && !game.output.empty();
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
    SetText("convert-button", Escaped(existing != 0
        ? (existing == ready ? Tr("Add missing content") : Tr("Convert and add content"))
        : (ready > 1 ? Tr("Convert %1 games", {std::to_string(ready)}) : Tr("Convert"))));
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
        job.omitFirmware = m_omitFirmware.contains(game.applicationId);
        job.output = game.output;
        job.add = game.add;
        job.applicationId = game.applicationId;
        for (const size_t index : game.selected)
        {
            const auto & source = m_sources[index];
            job.sources.push_back(source.path);
            if (source.firmwareBytes != 0) job.firmwareSources.insert(source.path);
            if (const auto stamp = Stamp(source.path)) job.sourceStamps.emplace(source.path, *stamp);
            job.sourceBytes += source.size;
            for (const auto & content : source.contents)
                job.sourceContents[source.path].emplace_back(content.kind, content.titleId, content.version);
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
        job.done = job.inspected = false;
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
        std::vector<std::string> arguments;
        if (job.add && !job.inspected)
        {
            job.inspecting = true;
            arguments = {"contents", job.output};
        }
        else
        {
            if (job.add)
            {
                arguments = {"add", job.output};
                arguments.insert(arguments.end(), job.missingSources.begin(), job.missingSources.end());
            }
            else
            {
                arguments = {"pack"};
                arguments.insert(arguments.end(), job.sources.begin(), job.sources.end());
                arguments.push_back(job.output);
            }
            arguments.push_back("--level");
            arguments.push_back(std::to_string(m_level));
            const auto & inputs = job.add ? job.missingSources : job.sources;
            const bool omit = job.omitFirmware && std::any_of(inputs.begin(), inputs.end(), [&](const auto & path) {
                return job.firmwareSources.contains(path);
            });
            if (omit) arguments.push_back("--omit-firmware");
            arguments.push_back(job.recipe && !omit ? "--recipe" : "--no-recipe");
        }
        m_lastProgress.clear();
        if (m_tool.Start(arguments))
        {
            ShowWork(m_tool.Current());
            return;
        }
        job.inspecting = false;
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
    const std::string heading = job.add ? Tr("Adding to %1...", {job.name}) : Tr("Converting %1...", {job.name});
    std::string detail = job.inspecting ? Tr("Reading the file...") : ProgressText(state);
    double totalWeight = 0, finishedWeight = 0, remainingWeight = 0;
    for (const Job & item : m_jobs)
    {
        const double weight = std::max<uint64_t>(1, item.sourceBytes);
        totalWeight += weight;
        if (item.done) finishedWeight += weight;
        else if (&item != &job) remainingWeight += weight;
    }
    const double fraction = state.total == 0 ? 0.0 : std::min(0.99, static_cast<double>(state.progress) / state.total);
    const double weight = std::max<uint64_t>(1, job.sourceBytes);
    const int percent = std::min(99, static_cast<int>(100 * (finishedWeight + weight * fraction) / totalWeight));
    detail += " · " + Tr("Batch %1 of %2 (%3 %)", {std::to_string(m_job + 1), std::to_string(m_jobs.size()), std::to_string(percent)});
    if (!job.inspecting)
    {
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
    }
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
            if (job.add && !std::all_of(job.sourceContents.at(path).begin(), job.sourceContents.at(path).end(),
                    [&](const auto & entry) { return job.added.contains(entry); })) continue;
            if (Stamp(path) == stamp && seen.insert(Key(path)).second) candidates.push_back(path);
        }
    }
    return candidates;
}

void NxConvertDialog::TrashOriginals()
{
    if (m_confirmTrash.empty() || !m_host.canWork()) return;
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
    if (!failures.empty()) m_trashResult += " " + Tr("Could not move to the trash: %1", {failures});
    ShowResults();
}

void NxConvertDialog::ShowResults()
{
    std::string rml;
    size_t succeeded = 0;
    for (size_t i = 0; i < m_jobs.size(); ++i)
    {
        const Job & job = m_jobs[i];
        succeeded += job.succeeded ? 1 : 0;
        rml += std::string("<div class=\"nx-result") + (job.succeeded ? "" : " failed") + "\"><img src=\"images/" + (job.succeeded ? "check" : "alert") +
               ".png\" /><div class=\"nx-result-texts\"><div class=\"nx-result-name\">" + Escaped(job.name) + "</div><div class=\"nx-result-text\">" + Escaped(job.message) + "</div>";
        if (job.succeeded)
        {
            const std::string index = std::to_string(i);
            rml += "<button data-command=\"" + std::string(kEditResultPrefix) + index + "\">" + Escaped(Tr("Title and picture")) + "</button> ";
            rml += "<button data-command=\"" + std::string(kManageResultPrefix) + index + "\">" + Escaped(Tr("Updates and DLC")) + "</button>";
        }
        rml += "</div></div>";
    }
    if (std::any_of(m_jobs.begin(), m_jobs.end(), [](const Job & job) { return job.retryable; }))
        rml += "<button data-command=\"retry-failed\">" + Escaped(Tr("Retry failed jobs")) + "</button> ";
    if (std::any_of(m_jobs.begin(), m_jobs.end(), [](const Job & job) { return job.canRetry; }))
    {
        rml += "<button data-command=\"retry-without-recipe\">" + Escaped(Tr("Retry without rebuild data")) + "</button>";
    }
    if (!m_confirmTrash.empty())
    {
        rml = "<div class=\"nx-text\">" + Escaped(Tr("Move these original files to the trash?")) + "</div>";
        for (const auto & path : m_confirmTrash)
        {
            rml += "<div class=\"nx-note\">" + Escaped(path) + "</div>";
        }
        rml += "<button data-command=\"keep-originals\">" + Escaped(Tr("Keep originals")) + "</button> ";
        rml += "<button data-command=\"confirm-trash\">" + Escaped(Tr("Move to trash")) + "</button>";
    }
    else if (!TrashCandidates().empty())
    {
        rml += "<button data-command=\"ask-trash\">" + Escaped(Tr("Move converted originals to trash…")) + "</button>";
    }
    SetText("results", rml);
    const bool anyAdd = std::any_of(m_jobs.begin(), m_jobs.end(), [](const Job & job) { return job.add; });
    SetText("result-heading", Escaped(anyAdd ? Tr("%1 of %2 jobs completed", {std::to_string(succeeded), std::to_string(m_jobs.size())})
        : succeeded == m_jobs.size() ? (succeeded > 1 ? Tr("%1 games converted", {std::to_string(succeeded)}) : Tr("Game converted"))
                                                                : Tr("%1 of %2 games converted", {std::to_string(succeeded), std::to_string(m_jobs.size())})));
    SetText("result-text", Escaped(!m_trashResult.empty() ? m_trashResult : succeeded != 0
        ? Tr("The NX files appear in the game list once their folder is one of the game directories.")
        : Tr("The original files were left as they were.")));
}
