#include <nxemu-core/modules/module_settings.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/settings/identifiers.h>
#include <yuzu_common/fs/path_util.h>
#include <yuzu_common/logging/backend.h>
#include <system_loader.h>
#include <loader_settings.h>
#include <core/file_sys/filesystem.h>
#include <core/file_sys/romfs_factory.h>
#include <core/file_sys/vfs/vfs_vector.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

extern IModuleSettings * g_settings;

// The registry belongs to the loader, so this test needs no guest OS or GPU.
class NoGuestModules final : public ISystemModules
{
public:
    void StartEmulation() override { std::abort(); }
    void StopEmulation(bool) override { std::abort(); }
    bool IsValid() const override { return false; }
    ISystemloader & Systemloader() override { throw std::logic_error("Unexpected guest access"); }
    IOperatingSystem & OperatingSystem() override { throw std::logic_error("Unexpected guest access"); }
    IVideo & Video() override { throw std::logic_error("Unexpected guest access"); }
    ICpu & Cpu() override { throw std::logic_error("Unexpected guest access"); }
};

int main()
{
    const auto directory = std::filesystem::temp_directory_path() /
        ("nxemu-loader-isolation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    auto & store = SettingsStore::GetInstance();
    assert(store.Initialize((directory / "config.json").string().c_str(), directory.string().c_str()));
    store.SetString(NXCoreSetting::AppDirectory, directory.string().c_str());
    Common::FS::SetAppDirectory(directory.string());
    Common::Log::Initialize(nullptr, "*:Critical");
    Common::Log::Start();
    ModuleSettings settings;
    g_settings = &settings;
    NoGuestModules modules;
    {
        Systemloader loader(modules);
        assert(loader.Initialize());
        auto & filesystem = loader.GetFileSystemController();
        // A fresh kernel reuses 0x51 for its first application process.
        constexpr uint64_t process = 0x51;
        for (unsigned session = 0; session < 4; ++session)
        {
            const uint64_t title = 0x0100000000001000ULL + session;
            const uint8_t marker = 0x40 + session;
            auto bytes = std::make_shared<FileSys::VectorVfsFile>(std::vector<uint8_t>{marker});
            auto factory = std::make_shared<FileSys::RomFSFactory>(
                bytes, false, loader.GetContentProvider(), filesystem);
            const std::weak_ptr<FileSys::RomFSFactory> borrowed(factory);
            assert(filesystem.RegisterProcess(process, title, std::move(factory)));
            uint64_t actualTitle = 0;
            ISaveDataFactory * save = nullptr;
            IRomFsController * romfs = nullptr;
            assert(filesystem.OpenProcess(&actualTitle, &save, &romfs, process));
            assert(actualTitle == title);
            auto * file = romfs->OpenRomFSCurrentProcess();
            uint8_t actual = 0;
            assert(file && file->ReadBytes(&actual, 1, 0) == 1 && actual == marker);
            file->Release();
            romfs->Release();
            save->Release();
            assert(!borrowed.expired());
            loader.ResetSession();
            // Reset must release the actual factory, including retained files.
            assert(borrowed.expired());
            loader.ResetSession();
        }
        CleanupLoaderSetting();
    }
    g_settings = nullptr;
    Common::Log::Stop();
    SettingsStore::CleanUp();
    std::filesystem::remove_all(directory);
    std::puts("PASS production loader: four sessions reuse PID 0x51 with distinct titles and RomFS bytes");
}
