#include "game_list_worker.h"
#include "settings/ui_settings.h"
#include <yuzu_common/fs/fs.h>
#include <common/path.h>
#include <common/std_string.h>  
#include <nxemu-module-spec/system_loader.h>
#include <nxemu-core/modules/system_modules.h>
#include <yuzu_common/interface_pointer.h>
#include <yuzu_common/interface_pointer_def.h>
#include <unordered_set>

namespace
{
bool HasSupportedFileExtension(const std::string & file_name)
{
    const std::unordered_set<std::string> supported_extensions = {
        "nso", "nro", "nca",
        "dxci", "dnsp", "kip"
    };
    return supported_extensions.contains(stdstr(Path(file_name).GetExtension()).ToLower());
}

bool IsExtractedNCAMain(const std::string & file_name)
{
    return (stdstr(Path(file_name).GetNameExtension()).ToLower() == "main");
}

}

GameListWorker::GameListWorker(IManualContentProvider & provider_, SystemModules & modules) :
    m_provider(provider_),
    m_modules(modules),
    m_stop(false)
{
}

GameListWorker::~GameListWorker()
{
    Stop();
}

void GameListWorker::Start()
{
    m_thread = std::thread(&GameListWorker::Run, this);
}

void GameListWorker::Stop()
{
    m_stop = true;
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

void GameListWorker::Run()
{
#ifdef tofix
    watch_list.clear();
    provider.ClearAllEntries();

    const auto DirEntryReady = [&](GameListDir * game_list_dir) {
        RecordEvent([=](GameList * game_list) { game_list->AddDirEntry(game_list_dir); });
    };
#endif
    for (const std::string & game_dir : uiSettings.gameDirectories)
    {
        if (m_stop)
        {
            break;
        }

        ScanFileSystem(ScanTarget::PopulateGameList, game_dir, false);
    }
}

void GameListWorker::ScanFileSystem(ScanTarget target, const std::string & dir_path, bool deep_scan/*, GameListDir * parent_dir*/)
{
    using IRomInfoPtr = InterfacePtr<IRomInfo>;

    const auto callback = [this, target/*, parent_dir*/](const std::filesystem::path & path) -> bool 
    {
        if (m_stop)
        {
            // Breaks the callback loop.
            return false;
        }
        const std::string physical_name = Common::FS::PathToUTF8String(path);
        const bool is_dir = Common::FS::IsDir(path);
        if (!is_dir && (HasSupportedFileExtension(physical_name) || IsExtractedNCAMain(physical_name)))
        {
            ISystemloader & systemloader = m_modules.Modules().Systemloader();
            IRomInfoPtr info(systemloader.RomInfo(physical_name.c_str(), 0 ,0));
            if (!info)
            {
                return true;
            }
            const LoaderFileType file_type = info->GetFileType();
            if (file_type == LoaderFileType::Unknown || file_type == LoaderFileType::Error)
            {
                return true;
            }
            u64 program_id = 0;
            const LoaderResultStatus res2 = info->ReadProgramId(program_id);

            if (target == ScanTarget::FillManualContentProvider)
            {
                if (res2 == LoaderResultStatus::Success)
                {
                    info->AddToManualContentProvider(m_provider);
                }
            }
            else
            {
                uint32_t count = 0;
                std::vector<uint64_t> program_ids;

                if (info->ReadProgramIds(nullptr, &count) == LoaderResultStatus::Success)
                {
                    program_ids.resize(count);
                    info->ReadProgramIds(program_ids.data(), &count);
                }

                if (res2 == LoaderResultStatus::Success && program_ids.size() > 1 && (file_type == LoaderFileType::XCI || file_type == LoaderFileType::NSP))
                {
                    __debugbreak();
#ifdef tofix
                    for (const auto id : program_ids)
                    {
                        loader = Loader::GetLoader(systemModules.GetSystemloader(), file, id);
                        if (!loader)
                        {
                            continue;
                        }

                        std::vector<u8> icon;
                        [[maybe_unused]] const auto res1 = info->ReadIcon(icon);

                        std::string name = " ";
                        [[maybe_unused]] const auto res3 = info->ReadTitle(name);

                        const FileSys::PatchManager patch{id, system.GetFileSystemController(), system.GetContentProvider()};

                        auto entry = MakeGameListEntry(physical_name, name, Common::FS::GetSize(physical_name), icon, *loader, id, compatibility_list, play_time_manager, patch);

                        RecordEvent(
                            [=](GameList * game_list) { game_list->AddEntry(entry, parent_dir); });
                    }
#endif
                }
                else
                {
                    uint32_t size = 0;
                    std::vector<u8> icon;
                    LoaderResultStatus res1 = info->ReadIcon(nullptr, &size);
                    if (res1 == LoaderResultStatus::Success)
                    {
                        icon.resize(size);
                        res1 = info->ReadIcon(icon.data(), &size);
                    }

                    std::string name = " ";
                    LoaderResultStatus res3 = info->ReadTitle(nullptr, &size);
                    if (res3 == LoaderResultStatus::Success)
                    {
                        name.resize(size);
                        res3 = info->ReadTitle(name.data(), &size);
                    }

                    //__debugbreak();
#ifdef tofix
                    const FileSys::PatchManager patch{program_id, system.GetFileSystemController(), system.GetContentProvider()};
                    auto entry = MakeGameListEntry(physical_name, name, Common::FS::GetSize(physical_name), icon, *info, program_id, compatibility_list, play_time_manager, patch);
                    RecordEvent([=](GameList * game_list) { game_list->AddEntry(entry, parent_dir); });
#endif
                }
            }
        }
        else if (is_dir)
        {
            __debugbreak();
#ifdef tofix
            watch_list.append(QString::fromStdString(physical_name));
#endif
        }
        return true;
    };

    if (deep_scan)
    {
        Common::FS::IterateDirEntriesRecursively(dir_path, callback, Common::FS::DirEntryFilter::All);
    }
    else
    {
        Common::FS::IterateDirEntries(dir_path, callback, Common::FS::DirEntryFilter::File);
    }
}

template class InterfacePtr<IRomInfo>;