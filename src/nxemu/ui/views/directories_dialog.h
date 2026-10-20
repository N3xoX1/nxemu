#pragma once
#include "dialog.h"
#include "settings_page.h"
#include <functional>
#include <string>

class GameLibrary;
class UiWindow;

class DirectoriesDialog final : public Dialog
{
public:
    struct Host
    {
        std::function<bool()> canChangeLibrary;
        std::function<void()> libraryChanged;
        OpenSettingsPage openPage;
    };

    DirectoriesDialog(UiWindow & window, GameLibrary & library, const std::filesystem::path & assetDir, Host host);

private:
    void OnOpen() override;
    void OnCommand(const std::string & command) override;
    void OnCloseRequested() override;

    void ShowDirectories();
    void AddDirectory();
    void RemoveDirectory(size_t index);
    void ToggleSubfolders(size_t index);

    UiWindow & m_window;
    GameLibrary & m_library;
    const Host m_host;
    bool m_libraryChanged = false;
};
