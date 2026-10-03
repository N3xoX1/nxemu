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
bool FileSelect(const char * initialDir, const char * fileFilter, Path & selected);
Path BrowseForDirectory(const char * title);
} // namespace MacOSFileDialogs
#endif

#ifdef __linux__
namespace { std::string PickLinuxFile(void * owner, const char * initialDir, bool mustExist); }
#endif

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
#elif defined(__linux__)
    (void)fileFilter; // The portal's native filter encoding is not the Win32 double-NUL format.
    const std::string result = PickLinuxFile(hwndOwner, initialDir, fileMustExist);
    if (result.empty()) return false;
    selected = Path(result.c_str());
    return true;
#else
    return false;
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
        if (rest.compare(0, slash, "localhost") != 0 || slash != 9)
            return {}; // Reject remote URIs. The API returns local Path objects.
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
                if (value == 0) return {};
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

enum class FolderPick
{
    Pending,
    Cancelled,
    Selected,
};

FolderPick ReadPortalResponse(DBusMessage * message, const std::string & requestPath, std::string & selected)
{
    if (!dbus_message_is_signal(message, "org.freedesktop.portal.Request", "Response"))
    {
        return FolderPick::Pending;
    }
    const char * path = dbus_message_get_path(message);
    if (path == nullptr || requestPath != path)
    {
        return FolderPick::Pending;
    }

    DBusMessageIter args;
    if (!dbus_message_iter_init(message, &args) || dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_UINT32)
    {
        return FolderPick::Cancelled;
    }
    dbus_uint32_t response = 1;
    dbus_message_iter_get_basic(&args, &response);
    if (response != 0 || !dbus_message_iter_next(&args) || dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY)
    {
        return FolderPick::Cancelled;
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
                    return selected.empty() ? FolderPick::Cancelled : FolderPick::Selected;
                }
            }
        }
        dbus_message_iter_next(&results);
    }
    return FolderPick::Cancelled;
}

enum class PortalStatus
{
    Unavailable,
    Cancelled,
    Selected,
};

struct PortalDirectory
{
    PortalStatus status;
    std::string path;
};

PortalDirectory PortalPickPath(void * parentWindow, const char * title, bool directory, bool save)
{
    DBusError error;
    dbus_error_init(&error);
    DBusConnection * connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (dbus_error_is_set(&error) || connection == nullptr)
    {
        dbus_error_free(&error);
        return {PortalStatus::Unavailable, {}};
    }

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
        save ? "SaveFile" : "OpenFile");
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

    // A wl_surface* is not an xdg-foreign exported parent identifier.
    // Until SciterUI provides xdg-foreign export, use an unparented portal dialog.
    (void)parentWindow;
    const std::string parent;
    const char * parentValue = parent.c_str();
    const char * titleValue = (title != nullptr && title[0] != '\0') ? title : "Select Directory";
    dbus_bool_t yes = TRUE;

    DBusMessageIter args;
    DBusMessageIter options;
    dbus_message_iter_init_append(message, &args);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &parentValue);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &titleValue);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &options);
    AppendDictEntry(&options, "handle_token", DBUS_TYPE_STRING, "s", &tokenValue);
    if (directory)
        AppendDictEntry(&options, "directory", DBUS_TYPE_BOOLEAN, "b", &yes);
    AppendDictEntry(&options, "modal", DBUS_TYPE_BOOLEAN, "b", &yes);
    dbus_bool_t no = FALSE;
    AppendDictEntry(&options, "multiple", DBUS_TYPE_BOOLEAN, "b", &no);
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
    FolderPick pick = FolderPick::Pending;
    while (pick == FolderPick::Pending && dbus_connection_read_write(connection, 200))
    {
        while (pick == FolderPick::Pending)
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
    if (pick == FolderPick::Selected)
    {
        return {PortalStatus::Selected, selected};
    }
    if (pick == FolderPick::Cancelled)
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

std::string KDialogPickDirectory(const char * title)
{
    if (access("/usr/bin/kdialog", X_OK) != 0)
    {
        return {};
    }
    const char * home = getenv("HOME");
    if (home == nullptr || home[0] == '\0')
    {
        home = "/";
    }
    return CaptureCommand({
        "/usr/bin/kdialog",
        "--getexistingdirectory",
        home,
        "--title",
        (title != nullptr && title[0] != '\0') ? title : "Select Directory",
    });
}

std::string PickLinuxFile(void * owner, const char * initialDir, bool mustExist)
{
    const auto portal = PortalPickPath(owner, mustExist ? "Select File" : "Save File",
                                      false, !mustExist);
    if (portal.status == PortalStatus::Selected)
        return portal.path;
    if (portal.status == PortalStatus::Cancelled)
        return {};
    if (access("/usr/bin/kdialog", X_OK) != 0)
        return {};
    const std::string directory = initialDir && *initialDir ? initialDir :
        (getenv("HOME") ? getenv("HOME") : "/");
    return CaptureCommand({"/usr/bin/kdialog", mustExist ? "--getopenfilename" : "--getsavefilename",
                           directory});
}

std::string PickDirectory(void * parentWindow, const char * title)
{
    const PortalDirectory portal = PortalPickPath(parentWindow, title, true, false);
    if (portal.status == PortalStatus::Selected)
    {
        return portal.path;
    }
    if (portal.status == PortalStatus::Cancelled)
    {
        return {};
    }
    return KDialogPickDirectory(title);
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
