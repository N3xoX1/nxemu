#include "file_dialogs.h"
#include <common/path.h>

#ifdef _WIN32
#include <Windows.h>

#include <CommDlg.h>
#include <common/std_string.h>
#include <shlobj_core.h>
#include <vector>
#elif defined(__linux__)
#include <dbus/dbus.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#endif

#ifdef __APPLE__
namespace MacOSFileDialogs
{
bool FileSave(const char * initialDir,const char * filter,const char * suggested,Path & selected);
bool FileSelect(const char * initialDir, const char * fileFilter, Path & selected);
Path BrowseForDirectory(const char * title);
} // namespace MacOSFileDialogs
#endif

#if defined(__linux__)
namespace {
bool LinuxFileDialog(bool save, const char * directory, const char * filter, const char * suggested, Path & selected);
}
#endif

std::string FileFilter(std::initializer_list<std::pair<std::string, const char *>> kinds)
{
    // The name with its patterns, then the patterns, each ended by a zero.
    std::string filter;
    for (const auto & kind : kinds)
    {
        filter.append(kind.first).append(" (").append(kind.second).append(")").append(1, '\0').append(kind.second).append(1, '\0');
    }
    return filter;
}

bool FileSelect(void * hwndOwner, const char * initialDir, const char * fileFilter, bool fileMustExist, Path & selected)
{
#ifdef _WIN32
    size_t filterLen = 0;
    while (fileFilter[filterLen] != '\0' || fileFilter[filterLen + 1] != '\0')
    {
        filterLen++;
    }
    filterLen += 2;

    std::vector<wchar_t> fileFilterW(filterLen);
    MultiByteToWideChar(CP_UTF8, 0, fileFilter, (int)filterLen, fileFilterW.data(), static_cast<int>(filterLen));

    Path currentDir(Path::CURRENT_DIRECTORY);
    std::wstring initialDirW = stdstr(initialDir).ToUTF16();

    OPENFILENAME openfilename = {};
    std::vector<wchar_t> fileName(32768);

    openfilename.lStructSize = sizeof(openfilename);
    openfilename.hwndOwner = (HWND)hwndOwner;
    openfilename.lpstrFilter = fileFilterW.data();
    openfilename.lpstrFile = fileName.data();
    openfilename.lpstrInitialDir = initialDirW.c_str();
    openfilename.nMaxFile = (DWORD)fileName.size();
    openfilename.Flags = OFN_HIDEREADONLY | (fileMustExist ? OFN_FILEMUSTEXIST : 0);

    bool res = GetOpenFileName(&openfilename) != 0;
    if (Path(Path::CURRENT_DIRECTORY) != currentDir)
    {
        currentDir.DirectoryChange();
    }
    if (!res)
    {
        return false;
    }
    selected = Path(stdstr().FromUTF16(fileName.data()).c_str());
    return true;
#elif defined(__APPLE__)
    (void)hwndOwner;
    (void)fileMustExist;
    return MacOSFileDialogs::FileSelect(initialDir, fileFilter, selected);
#else
    (void)hwndOwner;
    (void)fileMustExist;
    return LinuxFileDialog(false, initialDir, fileFilter, nullptr, selected);
#endif
}

bool FileSave(void * owner,const char * initialDir,const char * filter,const char * suggested,Path & selected)
{
#ifdef _WIN32
    std::vector<wchar_t> name(32768);const auto suggestedW=stdstr(suggested).ToUTF16();
    std::copy(suggestedW.begin(),suggestedW.end(),name.begin());
    size_t length=0;while(filter[length] || filter[length+1])++length;length+=2;
    std::vector<wchar_t> filters(length);MultiByteToWideChar(CP_UTF8,0,filter,static_cast<int>(length),filters.data(),static_cast<int>(length));
    const auto directory=stdstr(initialDir).ToUTF16();OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);
    dialog.hwndOwner=static_cast<HWND>(owner);dialog.lpstrFilter=filters.data();dialog.lpstrFile=name.data();dialog.nMaxFile=static_cast<DWORD>(name.size());
    dialog.lpstrInitialDir=directory.c_str();dialog.lpstrDefExt=L"nxshader";dialog.Flags=OFN_HIDEREADONLY|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST;
    if(!GetSaveFileNameW(&dialog))return false;selected=Path(stdstr().FromUTF16(name.data()).c_str());return true;
#elif defined(__APPLE__)
    return MacOSFileDialogs::FileSave(initialDir,filter,suggested,selected);
#else
    (void)owner;
    return LinuxFileDialog(true, initialDir, filter, suggested, selected);
#endif
}

#if defined(__linux__)
namespace
{

std::string FileUriToPath(const char * uri)
{
    const char prefix[] = "file://";
    if (uri == nullptr || strncmp(uri, prefix, sizeof(prefix) - 1) != 0)
    {
        return {};
    }

    std::string rest(uri + sizeof(prefix) - 1);
    if (!rest.empty() && rest[0] != '/')
    {
        const std::string::size_type slash = rest.find('/');
        if (slash == std::string::npos)
        {
            return {};
        }
        // A remote file://host/path must not silently become a local /path.
        if (rest.substr(0, slash) != "localhost")
        {
            return {};
        }
        rest.erase(0, slash);
    }

    std::string path;
    path.reserve(rest.size());
    for (std::string::size_type i = 0; i < rest.size(); i++)
    {
        if (rest[i] == '%' && i + 2 < rest.size())
        {
            char hex[3] = {rest[i + 1], rest[i + 2], '\0'};
            char * end = nullptr;
            const long value = strtol(hex, &end, 16);
            if (end == hex + 2)
            {
                if (value == 0)
                {
                    return {};
                }
                path.push_back(static_cast<char>(value));
                i += 2;
                continue;
            }
        }
        path.push_back(rest[i]);
    }
    return path;
}

void AppendDictEntry(DBusMessageIter * dict, const char * key, int type, const char * signature, const void * value)
{
    DBusMessageIter entry;
    DBusMessageIter variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, signature, &variant);
    dbus_message_iter_append_basic(&variant, type, value);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

void AppendFolderOption(DBusMessageIter * dict, const char * folder)
{
    if (folder == nullptr || folder[0] == '\0')
    {
        return;
    }
    const char * key = "current_folder";
    DBusMessageIter entry, variant, bytes;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "ay", &variant);
    dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "y", &bytes);
    const auto * data = reinterpret_cast<const unsigned char *>(folder);
    dbus_message_iter_append_fixed_array(&bytes, DBUS_TYPE_BYTE, &data, static_cast<int>(strlen(folder) + 1));
    dbus_message_iter_close_container(&variant, &bytes);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

void AppendFileFilters(DBusMessageIter * dict, const char * filter)
{
    if (filter == nullptr || filter[0] == '\0')
    {
        return;
    }
    const char * key = "filters";
    DBusMessageIter entry, variant, filters;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "a(sa(us))", &variant);
    dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "(sa(us))", &filters);
    for (const char * cursor = filter; *cursor != '\0';)
    {
        const char * name = cursor;
        cursor += strlen(cursor) + 1;
        if (*cursor == '\0')
        {
            break;
        }
        const std::string patterns(cursor);
        cursor += patterns.size() + 1;

        DBusMessageIter item, rules;
        dbus_message_iter_open_container(&filters, DBUS_TYPE_STRUCT, nullptr, &item);
        dbus_message_iter_append_basic(&item, DBUS_TYPE_STRING, &name);
        dbus_message_iter_open_container(&item, DBUS_TYPE_ARRAY, "(us)", &rules);
        std::string::size_type begin = 0;
        while (begin < patterns.size())
        {
            const auto end = patterns.find(';', begin);
            std::string glob = patterns.substr(begin, end == std::string::npos ? end : end - begin);
            if (glob == "*.*")
            {
                glob = "*";
            }
            if (!glob.empty())
            {
                const dbus_uint32_t globType = 0;
                const char * globValue = glob.c_str();
                DBusMessageIter rule;
                dbus_message_iter_open_container(&rules, DBUS_TYPE_STRUCT, nullptr, &rule);
                dbus_message_iter_append_basic(&rule, DBUS_TYPE_UINT32, &globType);
                dbus_message_iter_append_basic(&rule, DBUS_TYPE_STRING, &globValue);
                dbus_message_iter_close_container(&rules, &rule);
            }
            if (end == std::string::npos)
            {
                break;
            }
            begin = end + 1;
        }
        dbus_message_iter_close_container(&item, &rules);
        dbus_message_iter_close_container(&filters, &item);
    }
    dbus_message_iter_close_container(&variant, &filters);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

enum class PortalResponseState
{
    Pending,
    Cancelled,
    Selected,
};

PortalResponseState ReadPortalResponse(DBusMessage * message, const std::string & requestPath, std::string & selected)
{
    if (!dbus_message_is_signal(message, "org.freedesktop.portal.Request", "Response"))
    {
        return PortalResponseState::Pending;
    }
    const char * path = dbus_message_get_path(message);
    if (path == nullptr || requestPath != path)
    {
        return PortalResponseState::Pending;
    }

    DBusMessageIter args;
    if (!dbus_message_iter_init(message, &args) || dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_UINT32)
    {
        return PortalResponseState::Cancelled;
    }
    dbus_uint32_t response = 1;
    dbus_message_iter_get_basic(&args, &response);
    if (response != 0 || !dbus_message_iter_next(&args) || dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY)
    {
        return PortalResponseState::Cancelled;
    }

    DBusMessageIter results;
    dbus_message_iter_recurse(&args, &results);
    while (dbus_message_iter_get_arg_type(&results) == DBUS_TYPE_DICT_ENTRY)
    {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&results, &entry);
        const char * key = nullptr;
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING)
        {
            dbus_message_iter_get_basic(&entry, &key);
        }
        if (key != nullptr && strcmp(key, "uris") == 0 && dbus_message_iter_next(&entry) && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT)
        {
            DBusMessageIter variant;
            dbus_message_iter_recurse(&entry, &variant);
            if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_ARRAY)
            {
                DBusMessageIter uris;
                dbus_message_iter_recurse(&variant, &uris);
                if (dbus_message_iter_get_arg_type(&uris) == DBUS_TYPE_STRING)
                {
                    const char * uri = nullptr;
                    dbus_message_iter_get_basic(&uris, &uri);
                    selected = FileUriToPath(uri);
                    return selected.empty() ? PortalResponseState::Cancelled : PortalResponseState::Selected;
                }
            }
        }
        dbus_message_iter_next(&results);
    }
    return PortalResponseState::Cancelled;
}

enum class PortalStatus
{
    Unavailable,
    Cancelled,
    Selected,
};

struct PortalSelection
{
    PortalStatus status;
    std::string path;
};

enum class PortalMode
{
    Directory,
    OpenFile,
    SaveFile,
};

PortalSelection PortalPick(void * parentWindow, const char * title, PortalMode mode,
                          const char * directory = nullptr, const char * filter = nullptr,
                          const char * suggested = nullptr)
{
    if (!dbus_threads_init_default()) return {PortalStatus::Unavailable, {}};
    DBusError error;
    dbus_error_init(&error);
    DBusConnection * connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (dbus_error_is_set(&error) || connection == nullptr)
    {
        dbus_error_free(&error);
        return {PortalStatus::Unavailable, {}};
    }
    dbus_connection_set_exit_on_disconnect(connection, false);

    const char * match = "type='signal',interface='org.freedesktop.portal.Request',member='Response'";
    dbus_bus_add_match(connection, match, &error);
    if (dbus_error_is_set(&error))
    {
        dbus_error_free(&error);
        dbus_connection_close(connection);
        dbus_connection_unref(connection);
        return {PortalStatus::Unavailable, {}};
    }
    dbus_connection_flush(connection);

    DBusMessage * message = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.FileChooser",
        mode == PortalMode::SaveFile ? "SaveFile" : "OpenFile");
    if (message == nullptr)
    {
        dbus_connection_close(connection);
        dbus_connection_unref(connection);
        return {PortalStatus::Unavailable, {}};
    }

    static unsigned sequence = 0;
    char token[64];
    snprintf(token, sizeof(token), "nxemu%u_%u", static_cast<unsigned>(getpid()), ++sequence);
    const char * tokenValue = token;

    // A Wayland surface pointer is not a portal window handle. Leave the
    // parent empty until an xdg-foreign handle can be exported.
    (void)parentWindow;
    std::string parent;
    const char * parentValue = parent.c_str();
    const char * titleValue = (title != nullptr && title[0] != '\0') ? title :
        mode == PortalMode::Directory ? "Select Directory" : mode == PortalMode::SaveFile ? "Save File" : "Open File";
    dbus_bool_t yes = TRUE;

    DBusMessageIter args;
    DBusMessageIter options;
    dbus_message_iter_init_append(message, &args);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &parentValue);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &titleValue);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &options);
    AppendDictEntry(&options, "handle_token", DBUS_TYPE_STRING, "s", &tokenValue);
    if (mode == PortalMode::Directory)
    {
        AppendDictEntry(&options, "directory", DBUS_TYPE_BOOLEAN, "b", &yes);
    }
    AppendDictEntry(&options, "modal", DBUS_TYPE_BOOLEAN, "b", &yes);
    dbus_bool_t no = FALSE;
    if (mode != PortalMode::SaveFile)
    {
        AppendDictEntry(&options, "multiple", DBUS_TYPE_BOOLEAN, "b", &no);
    }
    AppendFolderOption(&options, directory);
    if (mode != PortalMode::Directory)
    {
        AppendFileFilters(&options, filter);
    }
    if (mode == PortalMode::SaveFile && suggested != nullptr && suggested[0] != '\0')
    {
        AppendDictEntry(&options, "current_name", DBUS_TYPE_STRING, "s", &suggested);
    }
    dbus_message_iter_close_container(&args, &options);

    DBusMessage * reply = dbus_connection_send_with_reply_and_block(connection, message, 5000, &error);
    dbus_message_unref(message);
    if (dbus_error_is_set(&error) || reply == nullptr)
    {
        dbus_error_free(&error);
        dbus_connection_close(connection);
        dbus_connection_unref(connection);
        return {PortalStatus::Unavailable, {}};
    }

    const char * requestPathValue = nullptr;
    if (!dbus_message_get_args(reply, &error, DBUS_TYPE_OBJECT_PATH, &requestPathValue, DBUS_TYPE_INVALID) || requestPathValue == nullptr)
    {
        dbus_error_free(&error);
        dbus_message_unref(reply);
        dbus_connection_close(connection);
        dbus_connection_unref(connection);
        return {PortalStatus::Unavailable, {}};
    }
    const std::string requestPath(requestPathValue);
    dbus_message_unref(reply);

    std::string selected;
    PortalResponseState pick = PortalResponseState::Pending;
    while (pick == PortalResponseState::Pending && dbus_connection_read_write(connection, 200))
    {
        while (pick == PortalResponseState::Pending)
        {
            DBusMessage * incoming = dbus_connection_pop_message(connection);
            if (incoming == nullptr)
            {
                break;
            }
            pick = ReadPortalResponse(incoming, requestPath, selected);
            dbus_message_unref(incoming);
        }
    }

    dbus_connection_close(connection);
    dbus_connection_unref(connection);
    if (pick == PortalResponseState::Selected)
    {
        return {PortalStatus::Selected, selected};
    }
    if (pick == PortalResponseState::Cancelled)
    {
        return {PortalStatus::Cancelled, {}};
    }
    return {PortalStatus::Unavailable, {}};
}

std::string CaptureCommand(const std::vector<std::string> & args)
{
    int pipeFd[2];
    if (pipe(pipeFd) != 0)
    {
        return {};
    }

    const pid_t pid = fork();
    if (pid < 0)
    {
        close(pipeFd[0]);
        close(pipeFd[1]);
        return {};
    }
    if (pid == 0)
    {
        dup2(pipeFd[1], STDOUT_FILENO);
        close(pipeFd[0]);
        close(pipeFd[1]);
        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (const std::string & arg : args)
        {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(pipeFd[1]);
    std::string output;
    char buffer[512];
    ssize_t count = 0;
    while ((count = read(pipeFd[0], buffer, sizeof(buffer))) > 0)
    {
        output.append(buffer, buffer + count);
    }
    close(pipeFd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        return {};
    }
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
    {
        output.pop_back();
    }
    return output;
}

std::string FindExecutable(const char * name)
{
    const char * environment = getenv("PATH");
    if (environment == nullptr)
    {
        return {};
    }
    const std::string searchPath(environment);
    for (size_t begin = 0; begin <= searchPath.size();)
    {
        const auto end = searchPath.find(':', begin);
        const std::string directory = searchPath.substr(begin, end == std::string::npos ? end : end - begin);
        const std::string candidate = (directory.empty() ? "." : directory) + std::string("/") + name;
        if (access(candidate.c_str(), X_OK) == 0)
        {
            return candidate;
        }
        if (end == std::string::npos)
        {
            break;
        }
        begin = end + 1;
    }
    return {};
}

std::string FallbackFileDialog(bool save, const char * directory, const char * suggested)
{
    const std::string initial = std::string(directory ? directory : "") + "/" + (suggested ? suggested : "");
    if (const std::string zenity = FindExecutable("zenity"); !zenity.empty())
    {
        std::vector<std::string> args{zenity, "--file-selection", "--filename=" + initial};
        if (save)
        {
            args.push_back("--save");
        }
        return CaptureCommand(args);
    }
    if (const std::string kdialog = FindExecutable("kdialog"); !kdialog.empty())
    {
        return CaptureCommand({kdialog, save ? "--getsavefilename" : "--getopenfilename", initial, "*"});
    }
    return {};
}

bool LinuxFileDialog(bool save, const char * directory, const char * filter, const char * suggested, Path & selected)
{
    const PortalSelection portal = PortalPick(nullptr, nullptr, save ? PortalMode::SaveFile : PortalMode::OpenFile,
                                              directory, filter, suggested);
    if (portal.status == PortalStatus::Cancelled)
    {
        return false;
    }
    const std::string file = portal.status == PortalStatus::Selected ?
                                 portal.path : FallbackFileDialog(save, directory, suggested);
    if (file.empty())
    {
        return false;
    }
    selected = Path(file.c_str());
    return true;
}

std::string FallbackPickDirectory(const char * title)
{
    const char * home = getenv("HOME");
    if (home == nullptr || home[0] == '\0')
    {
        home = "/";
    }
    if (const std::string zenity = FindExecutable("zenity"); !zenity.empty())
    {
        return CaptureCommand({zenity, "--file-selection", "--directory", std::string("--filename=") + home + "/"});
    }
    const std::string kdialog = FindExecutable("kdialog");
    if (kdialog.empty())
    {
        return {};
    }
    return CaptureCommand({
        kdialog,
        "--getexistingdirectory",
        home,
        "--title",
        (title != nullptr && title[0] != '\0') ? title : "Select Directory",
    });
}

std::string PickDirectory(void * parentWindow, const char * title)
{
    const PortalSelection portal = PortalPick(parentWindow, title, PortalMode::Directory);
    if (portal.status == PortalStatus::Selected)
    {
        return portal.path;
    }
    if (portal.status == PortalStatus::Cancelled)
    {
        return {};
    }
    return FallbackPickDirectory(title);
}

} // namespace
#endif

Path BrowseForDirectory(void * parentWindow, const char * title)
{
    Path selected;
#ifdef _WIN32
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    IFileDialog * dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
    if (SUCCEEDED(hr))
    {
        DWORD options = 0;
        if (SUCCEEDED(dlg->GetOptions(&options)))
        {
            options |= FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
            dlg->SetOptions(options);
        }

        if (title && *title)
        {
            dlg->SetTitle(stdstr_f(title).ToUTF16().c_str());
        }
        hr = dlg->Show((HWND)parentWindow);
        if (SUCCEEDED(hr))
        {
            IShellItem * result = nullptr;
            if (SUCCEEDED(dlg->GetResult(&result)) && result)
            {
                PWSTR psz = nullptr;
                if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz)
                {
                    selected = Path(stdstr().FromUTF16(psz).c_str(), "");
                    CoTaskMemFree(psz);
                }
                result->Release();
            }
        }
        dlg->Release();
    }

    if (SUCCEEDED(hrCo))
    {
        CoUninitialize();
    }
#elif defined(__APPLE__)
    (void)parentWindow;
    selected = MacOSFileDialogs::BrowseForDirectory(title);
#elif defined(__linux__)
    const std::string folder = PickDirectory(parentWindow, title);
    if (!folder.empty())
    {
        selected = Path(folder.c_str(), "");
    }
#endif
    return selected;
}
