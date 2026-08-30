#include "file_dialogs.h"

#ifdef _WIN32
#include <Windows.h>

#include <CommDlg.h>
#include <common/path.h>
#include <common/std_string.h>
#include <shlobj_core.h>
#include <vector>
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
#else
    return false;
#endif
}

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
#endif
    return selected;
}
