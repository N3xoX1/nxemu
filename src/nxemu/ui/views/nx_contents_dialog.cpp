#include "nx_contents_dialog.h"
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <nxemu/ui/host/ui_renderer.h>
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/ui/i18n/translation.h>
#include <nxemu/ui/platform/file_dialogs.h>
#include <common/path.h>
#include <common/std_string.h>
#include <yuzu_common/fs/fs_util.h>
#include <yuzu_common/stb.h>

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

void ToMemory(void * context, void * data, int size)
{
    auto * out = static_cast<std::vector<uint8_t> *>(context);
    const auto * bytes = static_cast<const uint8_t *>(data);
    out->insert(out->end(), bytes, bytes + size);
}

// The picture as a 256x256 JPEG, cut to a square from its middle, like a console icon.
std::vector<uint8_t> IconFromPicture(const std::vector<uint8_t> & encoded)
{
    constexpr int kSide = 256;
    int width = 0, height = 0, channels = 0;
    stbi_uc * pixels = encoded.empty() ? nullptr : stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels, 3);
    if (pixels == nullptr)
    {
        return {};
    }
    const int side = std::min(width, height);
    const stbi_uc * square = pixels + (static_cast<size_t>((height - side) / 2) * width + (width - side) / 2) * 3;
    std::vector<uint8_t> resized(static_cast<size_t>(kSide) * kSide * 3);
    std::vector<uint8_t> jpeg;
    if (stbir_resize_uint8(square, side, side, width * 3, resized.data(), kSide, kSide, kSide * 3, 3) != 0)
    {
        stbi_write_jpg_to_func(ToMemory, &jpeg, kSide, kSide, 3, resized.data(), 92);
    }
    stbi_image_free(pixels);
    return jpeg;
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

void NxContentsDialog::Open(const std::string & utf8Archive, bool editTitle)
{
    if (m_tool.Busy())
    {
        Show();
        return;
    }
    m_archive = utf8Archive;
    m_editOnOpen = editTitle;
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
    m_applicationId.clear();
    ForgetPicture();
    m_size = 0;
    m_message.clear();
    m_retryAdd = false;
    m_addFiles.clear();
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
        const std::string detail = ProgressText(state);
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
            else if (record[0] == "APP" && record.size() >= 2 && m_applicationId.empty())
            {
                m_applicationId = record[1];
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
        if (m_editOnOpen && state.succeeded)
        {
            ShowEdit();
        }
        m_editOnOpen = false;
        return;
    }
    if (task == Task::Metadata)
    {
        ForgetPicture();
        if (state.succeeded)
        {
            m_changed = true;
            m_message = Tr("The title and picture were saved.");
            m_messageFailed = false;
        }
        else
        {
            m_message = state.cancelled ? Tr("Cancelled: the file was left as it was.") : Tr("Nothing changed: %1", {state.error});
            m_messageFailed = true;
        }
        Run(Task::List, {"contents", m_archive}, Tr("Reading the file..."));
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
    if (task == Task::Add)
    {
        const auto hasRecord = [&](const char * kind) {
            return std::any_of(state.records.begin(), state.records.end(), [kind](const auto & record) { return record[0] == kind; });
        };
        const bool previouslyAdded = m_addCommitted;
        m_addCommitted = m_addCommitted || hasRecord("ADDED");
        m_changed = m_changed || m_addCommitted;
        m_retryAdd = m_addRecipe && CanRetryWithoutRecipe(state);
        if (state.succeeded)
        {
            m_message = m_addCommitted && (previouslyAdded || !hasRecord("SKIPPED")) ? Tr("Added.") :
                        m_addCommitted ? Tr("Added; the rest was already in the file.") : Tr("Already in the file: nothing changed.");
        }
        else if (m_addCommitted)
        {
            m_message = state.cancelled ? Tr("Cancelled: some content was already added.") : Tr("Some content was added before the error: %1", {state.error});
        }
        else
        {
            m_message = state.cancelled ? Tr("Cancelled: the file was left as it was.") : Tr("Nothing changed: %1", {state.error});
        }
        m_messageFailed = !state.succeeded;
    }
    else if (state.succeeded)
    {
        m_changed = true;
        m_message = Tr("Removed. The space it used is reclaimed by a later change or when it grows large.");
        m_messageFailed = false;
    }
    else
    {
        m_message = state.cancelled ? Tr("Cancelled: the file was left as it was.") : Tr("Nothing changed: %1", {state.error});
        m_messageFailed = true;
    }
    Run(Task::List, {"contents", m_archive}, Tr("Reading the file..."));
}

void NxContentsDialog::Add(std::vector<std::string> files, bool recipe)
{
    if (files.empty() || !m_host.canWork())
    {
        return;
    }
    std::vector<std::string> arguments{"add", m_archive};
    arguments.insert(arguments.end(), files.begin(), files.end());
    if (recipe) m_addCommitted = false;
    m_addFiles = std::move(files);
    m_addRecipe = recipe;
    m_retryAdd = false;
    arguments.push_back(recipe ? "--recipe" : "--no-recipe");
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
    if (command == "retry-without-recipe" && m_retryAdd)
    {
        Add(m_addFiles, false);
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
    else if (command == "edit-metadata")
    {
        ShowEdit();
    }
    else if (command == "edit-choose-picture")
    {
        ChoosePicture();
    }
    else if (command == "save-metadata")
    {
        SaveMetadata();
    }
    else if (command == "keep")
    {
        ForgetPicture();
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
        for (const char * name : {"list", "work", "ask", "edit"})
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
    SetText("contents-message", Escaped(m_message) + (m_retryAdd ?
        "<div><button data-command=\"retry-without-recipe\">" + Escaped(Tr("Retry without rebuild data")) + "</button></div>" : ""));

}

void NxContentsDialog::ShowEdit()
{
    if (m_applicationId.empty())
    {
        return;
    }
    std::string title;
    for (const Item & item : m_items)
    {
        if (title.empty() && (item.kind == "base" || item.kind == "update"))
        {
            title = item.name;
        }
    }
    if (title.empty())
    {
        // The name the game list shows for a game without a title.
        title = Common::FS::PathToUTF8String(std::filesystem::path{Common::FS::ToU8String(m_archive)}.stem());
    }
    m_shownTitle = title;
    if (auto * field = rmlui_dynamic_cast<Rml::ElementFormControlInput *>(Document()->GetElementById("edit-title")))
    {
        field->SetValue(title);
    }
    if (auto * field = rmlui_dynamic_cast<Rml::ElementFormControlInput *>(Document()->GetElementById("edit-publisher")))
    {
        field->SetValue("");
    }
    SetText("edit-picture", "");
    SetText("edit-note", Escaped(Tr("Without a new picture, the file keeps its own. An empty publisher keeps the one it has.")));
    if (Rml::Element * note = Document()->GetElementById("edit-note"))
    {
        note->SetClass("failed", false);
    }
    ShowPage("edit");
}

void NxContentsDialog::ChoosePicture()
{
    const std::string filter = FileFilter({{Tr("Pictures"), "*.png;*.jpg;*.jpeg;*.bmp"}});
    Path selected;
    if (!FileSelect(m_window.NativeHandle(), Folder(m_archive).c_str(), filter.c_str(), true, selected))
    {
        return;
    }
    std::ifstream stream(std::filesystem::path{Common::FS::ToU8String(static_cast<const char *>(selected))}, std::ios::binary);
    const std::vector<uint8_t> encoded((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const std::vector<uint8_t> icon = IconFromPicture(encoded);
    Rml::Element * note = Document()->GetElementById("edit-note");
    if (icon.empty())
    {
        SetText("edit-note", Escaped(Tr("This file is not a picture NXEmu can read.")));
        if (note != nullptr)
        {
            note->SetClass("failed", true);
        }
        return;
    }
    ForgetPicture();
    // A new name each time: the interface keeps the textures of the files it showed.
    std::error_code error;
    const std::filesystem::path file = std::filesystem::temp_directory_path(error) / ("nxemu-nx-icon-" + std::to_string(++m_pictures) + ".jpg");
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(icon.data()), static_cast<std::streamsize>(icon.size()));
    out.close();
    if (error || !out)
    {
        SetText("edit-note", Escaped(Tr("The picture could not be saved.")));
        if (note != nullptr)
        {
            note->SetClass("failed", true);
        }
        return;
    }
    m_picture = Common::FS::PathToUTF8String(file);
    SetText("edit-picture", "<img src=\"" + Rml::StringUtilities::EncodeRml(UiRenderer::FileImageSource(m_picture)) + "\"/>");
    SetText("edit-note", Escaped(Tr("The new picture replaces the one the file has.")));
    if (note != nullptr)
    {
        note->SetClass("failed", false);
    }
}

void NxContentsDialog::SaveMetadata()
{
    if (!m_host.canWork() || m_applicationId.empty())
    {
        return;
    }
    std::string title, publisher;
    if (auto * field = rmlui_dynamic_cast<Rml::ElementFormControlInput *>(Document()->GetElementById("edit-title")))
    {
        title = field->GetValue();
    }
    if (auto * field = rmlui_dynamic_cast<Rml::ElementFormControlInput *>(Document()->GetElementById("edit-publisher")))
    {
        publisher = field->GetValue();
    }
    const auto trim = [](std::string & text) {
        text.erase(0, text.find_first_not_of(" \t"));
        text.erase(text.find_last_not_of(" \t") + 1);
    };
    trim(title);
    trim(publisher);
    std::vector<std::string> arguments{"set-metadata", m_archive, m_applicationId};
    // The title shown is written only once changed: a file name never becomes the title.
    if (!title.empty() && title != m_shownTitle)
    {
        arguments.push_back("title=" + title);
    }
    if (!publisher.empty())
    {
        arguments.push_back("publisher=" + publisher);
    }
    if (!m_picture.empty())
    {
        arguments.push_back("icon=" + m_picture);
    }
    if (arguments.size() == 3)
    {
        ShowList();
        return;
    }
    m_message.clear();
    Run(Task::Metadata, std::move(arguments), Tr("Saving the title and picture..."));
}

void NxContentsDialog::ForgetPicture()
{
    if (m_picture.empty())
    {
        return;
    }
    Rml::ReleaseTexture(UiRenderer::FileImageSource(m_picture));
    std::error_code error;
    std::filesystem::remove(std::filesystem::path{Common::FS::ToU8String(m_picture)}, error);
    m_picture.clear();
}
