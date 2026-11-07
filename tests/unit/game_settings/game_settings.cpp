#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void Check(bool condition, const char * message)
{
    if (!condition) throw std::runtime_error(message);
}

void WriteText(const std::filesystem::path & file, const std::string & text)
{
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
}

std::string ReadText(const std::filesystem::path & file)
{
    std::ifstream input(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

JsonValue Parse(const char * json)
{
    JsonValue root;
    JsonReader().Parse(json, json + strlen(json), root);
    return root;
}
}

int main()
{
    const auto directory = std::filesystem::temp_directory_path() / ("nxemu-game-settings-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try
    {
        std::filesystem::create_directories(directory);
        auto & store = SettingsStore::GetInstance();
        Check(store.Initialize((directory / "config.json").string().c_str(), directory.string().c_str()), "Initialize isolated settings");

        const auto file = directory / "GameSettings" / "0100000000010000.json";
        JsonValue section = Parse("{\"system\": {\"docked_mode\": \"Docked\"}}");

        store.SetGameSettings("nxemu-os", section);
        Check(store.GetGameSettings("nxemu-os").isNull() && !std::filesystem::exists(file), "Ignore game settings without a game");

        WriteText(file, "{\"nxemu-os\": {\"system\": {\"memory_layout_mode\": \"Memory_6Gb\"}}}");
        store.SetString(NXCoreSetting::GameTitleId, "0100000000010000");
        Check(store.GetGameSettings("nxemu-os")["system"]["memory_layout_mode"].asString() == "Memory_6Gb", "Load the game's sections");
        Check(store.GetGameSettings("nxemu-video").isNull(), "Return nothing for a section the game does not set");

        store.SetGameSettings("nxemu-os", section);
        Check(ReadText(file).find("docked_mode") != std::string::npos && ReadText(file).find("memory_layout_mode") == std::string::npos, "Save the game's section");

        JsonValue video = Parse("{\"video\": {\"resolution_setup\": \"Res2X\"}}");
        store.SetGameSettings("nxemu-video", video);
        Check(ReadText(file).find("docked_mode") != std::string::npos && ReadText(file).find("resolution_setup") != std::string::npos, "Keep the other sections");

        store.SetString(NXCoreSetting::GameTitleId, "");
        Check(store.GetGameSettings("nxemu-os").isNull(), "Clear the game's sections");
        store.SetString(NXCoreSetting::GameTitleId, "0100000000010000");
        Check(store.GetGameSettings("nxemu-video")["video"]["resolution_setup"].asString() == "Res2X", "Reload the saved sections");

        JsonValue empty;
        store.SetGameSettings("nxemu-os", empty);
        store.SetGameSettings("nxemu-video", empty);
        Check(!std::filesystem::exists(file), "Delete the file when the game sets nothing");

        WriteText(file, "{ not json");
        store.SetString(NXCoreSetting::GameTitleId, "");
        store.SetString(NXCoreSetting::GameTitleId, "0100000000010000");
        Check(store.GetGameSettings("nxemu-os").isNull(), "Ignore a malformed file");
        store.SetGameSettings("nxemu-os", section);
        Check(ReadText(file) == "{ not json", "Leave a malformed file untouched");

        store.SetString(NXCoreSetting::GameTitleId, "");
        std::filesystem::remove_all(directory);
    }
    catch (const std::exception & error)
    {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
