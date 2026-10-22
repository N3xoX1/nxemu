#include "ui_identifiers.h"
#include "ui_settings.h"
#include <chrono>
#include <common/json.h>
#include <filesystem>
#include <iostream>
#include <nxemu-core/settings/settings.h>
#include <stdexcept>

namespace
{
void Check(bool condition, const char * message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    const auto directory = std::filesystem::temp_directory_path();
    const auto file = directory / ("nxemu-ui-settings-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    try
    {
        auto & store = SettingsStore::GetInstance();
        Check(store.Initialize(file.string().c_str(), directory.string().c_str()), "Initialize isolated settings");
        JsonValue preferences(JsonValueType::Object);
        preferences["UseRmlUI"] = false;
        preferences["SciterConsole"] = true;
        preferences["Language"]["Current"] = "old-interface";
        preferences["InterfaceLanguage"] = "fr";
        preferences["InterfaceTheme"] = "light";
        preferences["StartGamesInFullscreen"] = true;
        preferences["GameDirectories"].Append(JsonValue("games"));
        preferences["GameDirectoriesRecursive"].Append(JsonValue("games"));
        preferences["Shortcuts"]["LoadFile"] = "Ctrl+O";
        store.SetSettings("UI", preferences);
        SetupUISetting();
        Check(uiSettings.interfaceLanguage == "fr" && uiSettings.interfaceTheme == "light", "Keep RmlUi language and theme");
        Check(uiSettings.startGamesInFullscreen, "Keep startup options");
        Check(uiSettings.gameDirectories == Stringlist{"games"} &&
              uiSettings.gameDirectoriesRecursive == Stringlist{"games"}, "Keep scan preferences");
        Check(uiSettings.shortcuts.at("LoadFile") == "Ctrl+O", "Keep RmlUi shortcuts");
        SetUISettingStringList(NXUISetting::GameDirectories, {"new-games"});
        Check(uiSettings.gameDirectories == Stringlist{"new-games"}, "Keep settings callbacks");
        SaveUISetting();
        const auto saved = store.GetSettings("UI");
        Check(!saved.isMember("UseRmlUI") && !saved.isMember("SciterConsole") &&
              !saved.isMember("Language"), "Do not save removed frontend settings");
        Check(saved["InterfaceLanguage"].asString() == "fr", "Save RmlUi preferences");
        std::filesystem::remove(file);
    }
    catch (const std::exception & error)
    {
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
