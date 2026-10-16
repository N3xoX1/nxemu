#pragma once
#include "dialog.h"
#include "settings_page.h"
#include <functional>
#include <string>

class GameLibrary;
class UiWindow;

// The page of the settings that says where the games are. A change is kept as
// it is made.
class DirectoriesDialog final : public Dialog
{
public:
    struct Host
    {
        // Whether the game directories may change now: not while a game runs.
        std::function<bool()> canChangeLibrary;
        // The game directories are no longer the ones the games shown came from.
        std::function<void()> libraryChanged;
        // Shows another page of the settings in place of this one.
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

    UiWindow & m_window;
    GameLibrary & m_library;
    const Host m_host;
    bool m_libraryChanged = false; // since the dialog opened
};
