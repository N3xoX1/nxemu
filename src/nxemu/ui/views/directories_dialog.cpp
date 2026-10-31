#include "directories_dialog.h"
#include <common/path.h>
#include <RmlUi/Core/ElementDocument.h>
#include <cstdlib>
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/ui/i18n/translation.h>
#include <nxemu/ui/library/game_library.h>
#include <nxemu/ui/platform/file_dialogs.h>
#include <system_error>
#include <yuzu_common/fs/fs_util.h>

namespace
{
constexpr const char * kRemovePrefix = "remove-directory:";
constexpr const char * kSubfoldersPrefix = "toggle-subfolders:";

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

bool Exists(const std::string & directory)
{
    std::error_code error;
    return std::filesystem::is_directory(std::filesystem::path{Common::FS::ToU8String(directory)}, error);
}
} // namespace

DirectoriesDialog::DirectoriesDialog(UiWindow & window, GameLibrary & library, const std::filesystem::path & assetDir, Host host) :
    Dialog(window, assetDir / "directories_dialog.rml", kSettingsNarrowBelow, kSettingsWidth, kSettingsHeight),
    m_window(window),
    m_library(library),
    m_host(std::move(host))
{
}

void DirectoriesDialog::OnOpen()
{
    m_libraryChanged = false;
    ShowDirectories();
}

void DirectoriesDialog::OnCommand(const std::string & command)
{
    SettingsPage page;
    if (SettingsPageOf(command, page))
    {
        if (m_host.openPage(page))
        {
            OnCloseRequested();
        }
    }
    else if (command == "add-directory")
    {
        AddDirectory();
    }
    else if (command.rfind(kRemovePrefix, 0) == 0)
    {
        RemoveDirectory(static_cast<size_t>(std::atoi(command.c_str() + std::string(kRemovePrefix).size())));
    }
    else if (command.rfind(kSubfoldersPrefix, 0) == 0)
    {
        ToggleSubfolders(static_cast<size_t>(std::atoi(command.c_str() + std::string(kSubfoldersPrefix).size())));
    }
}

void DirectoriesDialog::OnCloseRequested()
{
    const bool changed = m_libraryChanged;
    m_libraryChanged = false;
    Close();
    if (changed)
    {
        m_host.libraryChanged();
    }
}

void DirectoriesDialog::ShowDirectories()
{
    const bool locked = !m_host.canChangeLibrary();
    const std::vector<std::string> & directories = m_library.Directories();
    std::string rml;
    for (size_t index = 0; index < directories.size(); ++index)
    {
        const bool found = Exists(directories[index]);
        const bool subfolders = m_library.ScanSubfolders(directories[index]);
        rml += std::string("<div class=\"directory") + (found ? "" : " missing") + "\"><img src=\"images/folder.png\" /><div class=\"directory-texts\">"
               "<div class=\"directory-path\">" + Escaped(directories[index]) + "</div>" +
               (found ? std::string() : "<div class=\"directory-note\">" + Escaped(Tr("Not found: the games in it are not shown.")) + "</div>") + "</div>" +
               "<div class=\"directory-subfolders\">" + Escaped(Tr("Scan subfolders")) + "</div><div class=\"switch" + (subfolders ? " on" : "") +
               (locked ? std::string(" locked\"") : "\" data-command=\"" + std::string(kSubfoldersPrefix) + std::to_string(index) + "\"") +
               "><div class=\"switch-knob\"/></div>" +
               (locked ? std::string() : "<button data-command=\"" + std::string(kRemovePrefix) + std::to_string(index) + "\">" + Escaped(Tr("Remove")) + "</button>") + "</div>";
    }
    if (directories.empty())
    {
        rml = "<div class=\"directory-none\">" + Escaped(Tr("No game directory yet. Add the folders your games are in.")) + "</div>";
    }
    SetText("directories", rml);
    Document()->SetClass("library-locked", locked);
}

void DirectoriesDialog::AddDirectory()
{
    if (!m_host.canChangeLibrary())
    {
        return;
    }
    const Path selected = BrowseForDirectory(m_window.FrontNativeHandle(), Tr("Choose a game directory").c_str());
    if (m_library.AddDirectory(static_cast<const char *>(selected)))
    {
        m_libraryChanged = true;
    }
    ShowDirectories();
}

void DirectoriesDialog::RemoveDirectory(size_t index)
{
    if (!m_host.canChangeLibrary() || index >= m_library.Directories().size())
    {
        return;
    }
    const std::string directory = m_library.Directories()[index];
    if (m_library.RemoveDirectory(directory))
    {
        m_libraryChanged = true;
    }
    ShowDirectories();
}

void DirectoriesDialog::ToggleSubfolders(size_t index)
{
    if (!m_host.canChangeLibrary() || index >= m_library.Directories().size())
    {
        return;
    }
    const std::string directory = m_library.Directories()[index];
    m_library.SetScanSubfolders(directory, !m_library.ScanSubfolders(directory));
    m_libraryChanged = true;
    ShowDirectories();
}
