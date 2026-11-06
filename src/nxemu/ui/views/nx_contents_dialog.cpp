#include "nx_contents_dialog.h"
#include <RmlUi/Core/ElementDocument.h>
#include <algorithm>
#include <cstdlib>
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/ui/i18n/translation.h>
#include <nxemu/ui/platform/file_dialogs.h>
#include <common/path.h>
#include <common/std_string.h>
#include <yuzu_common/fs/fs_util.h>

namespace
{
constexpr const char * kRemovePrefix = "remove:";
constexpr const char * kRebuildPrefix = "rebuild:";

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

std::string FileName(const std::string & utf8)
{
    return Common::FS::PathToUTF8String(std::filesystem::path{Common::FS::ToU8String(utf8)}.filename());
}

std::string Folder(const std::string & utf8)
{
    return Common::FS::PathToUTF8String(std::filesystem::path{Common::FS::ToU8String(utf8)}.parent_path());
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

std::string Version(const std::string & display, uint32_t version)
{
    // Native versions count updates in units of 0x10000.
    return display.empty() ? Tr("Version %1", {std::to_string(version >> 16)}) : display;
}
} // namespace

NxContentsDialog::NxContentsDialog(UiWindow & window, const std::filesystem::path & assetDir, Host host) :
    Dialog(window.Context(), assetDir / "nx_contents_dialog.rml"),
    m_window(window),
    m_host(std::move(host))
{
}

void NxContentsDialog::Open(const std::string & utf8Archive)
{
    if (m_tool.Busy())
    {
        Show();
        return;
    }
    m_archive = utf8Archive;
    if (IsVisible())
    {
        OnOpen();
    }
    else
    {
        Show();
    }
}

bool NxContentsDialog::OfferDrop(const std::string & utf8Path)
{
    if (!IsVisible() || m_tool.Busy())
    {
        return false;
    }
    Add({utf8Path});
    return true;
}

void NxContentsDialog::OnOpen()
{
    m_items.clear();
    m_originals.clear();
    m_size = 0;
    m_message.clear();
    m_changed = false;
    if (!m_tool.Busy())
    {
        m_tool.Reset();
    }
    SetText("archive-name", Escaped(FileName(m_archive)));
    SetText("archive-folder", Escaped(Folder(m_archive)));
    Run(Task::List, {"contents", m_archive}, Tr("Reading the file..."));
}

void NxContentsDialog::Run(Task task, std::vector<std::string> arguments, std::string what)
{
    m_task = task;
    m_what = std::move(what);
    m_shownProgress.clear();
    if (!m_tool.Start(std::move(arguments)))
    {
        Finished(m_tool.Current());
        m_tool.Reset();
        return;
    }
    ShowPage("work");
    SetText("work-heading", Escaped(m_what));
    SetText("work-detail", "");
}

void NxContentsDialog::OnUpdate()
{
    const NxTool::State state = m_tool.Current();
    if (state.phase == NxTool::Phase::Running)
    {
        const std::string detail = state.progress != 0 ? Tr("%1 written", {Bytes(state.progress)}) : std::string();
        if (detail != m_shownProgress)
        {
            m_shownProgress = detail;
            SetText("work-detail", Escaped(detail));
        }
    }
    else if (state.phase == NxTool::Phase::Done && m_task != Task::None)
    {
        Finished(state);
        m_tool.Reset();
    }
}

void NxContentsDialog::Finished(const NxTool::State & state)
{
    const Task task = m_task;
    m_task = Task::None;
    if (task == Task::List)
    {
        m_items.clear();
        m_originals.clear();
        for (const std::vector<std::string> & record : state.records)
        {
            // SNAPSHOT identity kind application title version display name
            if (record[0] == "SNAPSHOT" && record.size() >= 8)
            {
                Item item;
                item.identity = record[1];
                item.kind = record[2];
                item.titleId = std::strtoull(record[4].c_str(), nullptr, 16);
                item.version = static_cast<uint32_t>(std::strtoul(record[5].c_str(), nullptr, 10));
                item.displayVersion = record[6];
                item.name = record[7];
                m_items.push_back(std::move(item));
            }
            // RECIPE checksum application original_size recipe_size name
            else if (record[0] == "RECIPE" && record.size() >= 6)
            {
                m_originals.push_back({record[1], record[5], std::strtoull(record[3].c_str(), nullptr, 10)});
            }
            else if (record[0] == "SIZE" && record.size() >= 2)
            {
                m_size = std::strtoull(record[1].c_str(), nullptr, 10);
            }
        }
        if (!state.succeeded)
        {
            m_message = state.error;
            m_messageFailed = true;
        }
        ShowList();
        return;
    }
    if (task == Task::Rebuild)
    {
        if (state.succeeded)
        {
            m_message = Tr("Rebuilt, identical to the original file.");
            m_messageFailed = false;
        }
        else
        {
            m_message = state.cancelled ? Tr("Cancelled: nothing was kept.") : Tr("Not rebuilt: %1", {state.error});
            m_messageFailed = true;
        }
        ShowList();
        return;
    }
    if (state.succeeded)
    {
        m_changed = true;
        size_t added = 0, skipped = 0;
        for (const std::vector<std::string> & record : state.records)
        {
            added += record[0] == "ADDED" ? 1 : 0;
            skipped += record[0] == "SKIPPED" ? 1 : 0;
        }
        if (task == Task::Add)
        {
            m_message = added != 0 && skipped == 0 ? Tr("Added.") : added != 0 ? Tr("Added; the rest was already in the file.") : Tr("Already in the file: nothing changed.");
        }
        else
        {
            m_message = Tr("Removed. The space it used is reclaimed by a later change or when it grows large.");
        }
        m_messageFailed = false;
    }
    else
    {
        m_message = state.cancelled ? Tr("Cancelled: the file was left as it was.") : Tr("Nothing changed: %1", {state.error});
        m_messageFailed = true;
    }
    Run(Task::List, {"contents", m_archive}, Tr("Reading the file..."));
}

void NxContentsDialog::Add(const std::vector<std::string> & files)
{
    if (files.empty() || !m_host.canWork())
    {
        return;
    }
    std::vector<std::string> arguments{"add", m_archive};
    arguments.insert(arguments.end(), files.begin(), files.end());
    // Keep what rebuilds each added file inside the NX, as the conversion does.
    arguments.push_back("--recipe");
    Run(Task::Add, std::move(arguments), Tr("Adding to %1...", {FileName(m_archive)}));
}

void NxContentsDialog::OnCommand(const std::string & command)
{
    if (command == "cancel")
    {
        m_tool.Cancel();
        return;
    }
    if (m_tool.Busy())
    {
        return;
    }
    if (command == "add-content")
    {
        std::vector<std::string> files;
        const std::string filter = FileFilter({{Tr("Updates and DLC"), "*.dnsp;*.dxci"}, {Tr("All files"), "*.*"}});
        if (FileSelectMany(m_window.NativeHandle(), Folder(m_archive).c_str(), filter.c_str(), files))
        {
            Add(files);
        }
    }
    else if (command.rfind(kRemovePrefix, 0) == 0)
    {
        m_asking = static_cast<size_t>(std::atoi(command.c_str() + std::string(kRemovePrefix).size()));
        if (m_asking < m_items.size() && m_items[m_asking].kind != "base")
        {
            const Item & item = m_items[m_asking];
            ShowPage("ask");
            SetText("ask-heading", Escaped(Tr("Remove %1 %2?", {KindName(item.kind), item.kind == "dlc" ? stdstr_f("%04llX", static_cast<unsigned long long>(item.titleId & 0xFFFF)) : Version(item.displayVersion, item.version)})));
            SetText("ask-text", Escaped(item.kind == "update" ? Tr("The game then starts with the next most recent update in the file, or without one. Its saves are not touched.")
                                                              : Tr("The game no longer sees this DLC. Its saves are not touched.")));
        }
    }
    else if (command.rfind(kRebuildPrefix, 0) == 0)
    {
        const size_t index = static_cast<size_t>(std::atoi(command.c_str() + std::string(kRebuildPrefix).size()));
        if (index < m_originals.size())
        {
            const Path selected = BrowseForDirectory(m_window.NativeHandle(), Tr("Choose where the original file goes").c_str());
            const std::string folder = static_cast<const char *>(selected);
            if (!folder.empty())
            {
                const Original & original = m_originals[index];
                m_message.clear();
                Run(Task::Rebuild, {"rebuild", m_archive, folder, original.id}, Tr("Rebuilding %1...", {original.name}));
            }
        }
    }
    else if (command == "confirm-remove" && m_asking < m_items.size())
    {
        if (m_host.canWork())
        {
            const Item & item = m_items[m_asking];
            Run(Task::Remove, {"remove", m_archive, item.identity}, Tr("Removing %1 %2...", {KindName(item.kind), Version(item.displayVersion, item.version)}));
        }
    }
    else if (command == "keep")
    {
        ShowList();
    }
}

void NxContentsDialog::OnCloseRequested()
{
    if (m_tool.Busy())
    {
        m_tool.Cancel();
        return;
    }
    Close();
    if (m_changed)
    {
        m_changed = false;
        m_host.libraryChanged();
    }
}

void NxContentsDialog::ShowPage(const char * page)
{
    if (Rml::Element * panel = Document()->GetElementById("nx-contents"))
    {
        for (const char * name : {"list", "work", "ask"})
        {
            panel->SetClass(std::string("at-") + name, std::string(name) == page);
        }
    }
}

void NxContentsDialog::ShowList()
{
    ShowPage("list");
    std::string name;
    uint32_t latest = 0;
    bool hasUpdate = false;
    for (const Item & item : m_items)
    {
        if (item.kind == "base" && !item.name.empty())
        {
            name = item.name;
        }
        if (item.kind == "update")
        {
            latest = hasUpdate ? std::max(latest, item.version) : item.version;
            hasUpdate = true;
            if (name.empty())
            {
                name = item.name;
            }
        }
    }
    SetText("archive-title", Escaped(name.empty() ? FileName(m_archive) : name));
    SetText("archive-size", Escaped(m_size != 0 ? Bytes(m_size) : std::string()));
    std::vector<size_t> order(m_items.size());
    for (size_t i = 0; i < order.size(); ++i)
    {
        order[i] = i;
    }
    const auto rank = [](const std::string & kind) { return kind == "base" ? 0 : kind == "update" ? 1 : 2; };
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const Item & x = m_items[a];
        const Item & y = m_items[b];
        return rank(x.kind) != rank(y.kind) ? rank(x.kind) < rank(y.kind) : x.kind == "dlc" ? x.titleId < y.titleId : x.version > y.version;
    });
    std::string rml;
    for (const size_t index : order)
    {
        const Item & item = m_items[index];
        const std::string version = item.kind == "dlc" ? (item.name.empty() ? Tr("Add-on content") : item.name) : Version(item.displayVersion, item.version);
        std::string tag;
        if (item.kind == "update" && item.version == latest)
        {
            tag = Tr("Newest");
        }
        rml += "<div class=\"nx-content\"><div class=\"nx-content-kind\">" + Escaped(KindName(item.kind)) + "</div><div class=\"nx-content-texts\"><div class=\"nx-content-version\">" +
               Escaped(version) + "</div><div class=\"nx-content-id\">" + stdstr_f("%016llX", static_cast<unsigned long long>(item.titleId)) + "</div></div>" +
               (tag.empty() ? std::string() : "<div class=\"nx-content-tag\">" + Escaped(tag) + "</div>") +
               (item.kind == "base" ? std::string() : "<button data-command=\"" + std::string(kRemovePrefix) + std::to_string(index) + "\">" + Escaped(Tr("Remove")) + "</button>") + "</div>";
    }
    if (m_items.empty())
    {
        rml = "<div class=\"nx-none\">" + Escaped(Tr("Nothing could be read from this file.")) + "</div>";
    }
    if (!m_originals.empty())
    {
        rml += "<div class=\"nx-content-group\">" + Escaped(Tr("Original files that can be rebuilt")) + "</div>";
    }
    for (size_t index = 0; index < m_originals.size(); ++index)
    {
        const Original & original = m_originals[index];
        rml += "<div class=\"nx-content\"><div class=\"nx-content-kind\">" + Escaped(Tr("Original")) + "</div><div class=\"nx-content-texts\"><div class=\"nx-content-version\">" +
               Escaped(original.name) + "</div><div class=\"nx-content-id\">" + Escaped(Bytes(original.size)) + "</div></div><button data-command=\"" + std::string(kRebuildPrefix) +
               std::to_string(index) + "\">" + Escaped(Tr("Rebuild")) + "</button></div>";
    }
    SetText("contents", rml);
    if (Rml::Element * message = Document()->GetElementById("contents-message"))
    {
        message->SetClass("failed", m_messageFailed);
    }
    SetText("contents-message", Escaped(m_message));
}
