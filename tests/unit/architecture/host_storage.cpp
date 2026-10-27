#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nxemu-core/settings/settings.h>
#include <nxemu-module-spec/profile_storage.h>
#include <nxemu/ui/host/input_service.h>

int main()
{
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("nxemu-host-storage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    auto &store = SettingsStore::GetInstance();
    const auto config = directory / "config.json";
    assert(store.Initialize(config.string().c_str(), directory.string().c_str()));
    {
        HostInputService input;
        JsonValue os;
        os["unrelated"] = "preserved";
        os["controller"]["player_1"]["Button"]["A"] = "engine:sdl,button:2";
        os["controller"]["player_0"]["metadata"] = "preserved";
        store.SetSettings("nxemu-os", os);
        std::array<JsonValue, 8> players{};
        std::array<bool, 8> changed{};
        players[0] = input.StoredPlayerOverrides(0);
        players[0]["Button"]["A"] = "engine:keyboard,code:65";
        changed[0] = true;
        assert(input.SaveOfflinePlayers(players, changed));
        assert(std::filesystem::is_regular_file(config));
        const auto saved = store.GetSettings("nxemu-os");
        assert(saved["unrelated"].asString() == "preserved");
        assert(saved["controller"]["player_0"]["metadata"].asString() == "preserved");
        assert(saved["controller"]["player_1"]["Button"]["A"].asString() == "engine:sdl,button:2");
        assert(input.StoredBinding(0, HostBindingGroup::Button, 0) == "engine:keyboard,code:65");
        players[0]["Button"]["A"] = std::string(4097, 'x');
        assert(!input.SaveOfflinePlayers(players, changed));
        assert(input.StoredBinding(0, HostBindingGroup::Button, 0) == "engine:keyboard,code:65");
        players[0]["Button"]["A"] = std::string("bad\0binding", 11);
        assert(!input.SaveOfflinePlayers(players, changed));
        assert(!input.SetStoredBindingOffline(10, HostBindingGroup::Button, 0, "x"));
        assert(!input.SetStoredBindingOffline(0, HostBindingGroup::Button, 999, "x"));
        os["controller"] = "malformed";
        store.SetSettings("nxemu-os", os);
        assert(!input.SetStoredBindingOffline(0, HostBindingGroup::Button, 0, "x"));
        assert(store.GetSettings("nxemu-os")["controller"].asString() == "malformed");
    }
    const auto profilePath = HostProfileStorage::FilePath(directory);
    assert(!HostProfileStorage::Read(profilePath));
    HostProfileStorage::FileRaw profiles{};
    profiles.users[0].uuid[0] = 42;
    profiles.users[0].username[0] = 'A';
    profiles.users[0].timestamp = 123456;
    assert(HostProfileStorage::Write(profilePath, profiles));
    const auto loaded = HostProfileStorage::Read(profilePath);
    assert(loaded && loaded->users[0].uuid[0] == 42 && loaded->users[0].timestamp == 123456);
    std::filesystem::resize_file(profilePath, 8);
    assert(!HostProfileStorage::Read(profilePath));
    assert(std::filesystem::file_size(profilePath) == 8);
    SettingsStore::CleanUp();
    std::filesystem::remove_all(directory);
    std::puts("PASS host storage: offline edits preserve unrelated data, malformed bindings rejected, "
              "profile format roundtrip");
}
