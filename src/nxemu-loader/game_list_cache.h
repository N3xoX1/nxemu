#pragma once
#include <nxemu-module-spec/system_loader.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Systemloader;

struct GameDirectory
{
    std::string path;
    bool scanSubfolders = false;

    bool operator==(const GameDirectory &) const = default;
};

struct CachedGameMetadata
{
    LoaderFileType fileType = LoaderFileType::Unknown;
    LoaderResultStatus programIdStatus = LoaderResultStatus::ErrorNotInitialized;
    uint64_t programId = 0;
    LoaderResultStatus programIdsStatus = LoaderResultStatus::ErrorNotInitialized;
    std::vector<uint64_t> programIds;
    LoaderResultStatus titleStatus = LoaderResultStatus::ErrorNotInitialized;
    std::string title;
    LoaderResultStatus iconStatus = LoaderResultStatus::ErrorNotInitialized;
    std::vector<uint8_t> icon;
    LoaderResultStatus versionStatus = LoaderResultStatus::ErrorNotInitialized;
    std::string displayVersion;
    std::string developer;
};

class GameListCache
{
public:
    explicit GameListCache(Systemloader & systemloader);
    ~GameListCache();

    struct ScanFile
    {
        std::string path;
        uint64_t size = 0;
        int64_t modifiedTime = 0;
    };

    bool Scan(const std::vector<GameDirectory> & gameDirs, IGameListScanCallback & callback);
    bool ScanFiles(const std::vector<ScanFile> & files, IGameListScanCallback & callback);
    std::vector<GameDirectory> Directories();
    std::vector<std::string> GameDirectoryAddOnFiles(uint64_t baseProgramId);
    // The folders in the game directories named for the title id, which hold its mods
    std::vector<std::string> ModFolders(uint64_t titleId);
    // Looks through the game directories for them again, so a mod added since the scan is found; false when stopped,
    // keeping those found before
    bool RefreshModFolders(const std::function<bool()> & stopRequested);

    using TitleIdFolders = std::map<uint64_t, std::vector<std::string>>;

private:
    GameListCache() = delete;
    GameListCache(const GameListCache &) = delete;
    GameListCache & operator=(const GameListCache &) = delete;

    struct FileStamp
    {
        uint64_t size = 0;
        int64_t modifiedTime = 0;

        bool operator==(const FileStamp & other) const
        {
            return size == other.size && modifiedTime == other.modifiedTime;
        }
    };

    struct AddOnEntry
    {
        uint64_t titleId = 0;
        LoaderTitleType titleType = LoaderTitleType::AOC;
        LoaderContentRecordType contentType = LoaderContentRecordType::Program;
        uint32_t version = 0;
    };
    using AddOnEntries = std::vector<AddOnEntry>;

    struct ProgramRecord
    {
        CachedGameMetadata base;

        bool updateValid = false;
        uint64_t updateFingerprint = 0;
        std::string updateTitle;
        std::vector<uint8_t> updateIcon;
        std::string updateVersion;

        std::shared_ptr<const CachedGameMetadata> effective;
    };
    using ProgramRecords = std::vector<ProgramRecord>;

    struct GameFileRecord
    {
        FileStamp stamp;
        bool isGame = false;
        ProgramRecords programs;
        AddOnEntries addOns;
        bool seen = false;
    };

    struct UpdateCandidate
    {
        const std::string * path;
        FileStamp stamp;
        uint32_t version;
    };
    using UpdateIndex = std::multimap<uint64_t, UpdateCandidate>;

    using GameFiles = std::map<std::string, GameFileRecord>;
    using DisabledAddOns = std::map<uint64_t, std::vector<std::string>>;

    struct DirFile
    {
        std::string path;
        FileStamp stamp;
    };

    void BeginScan();
    bool FinishScan(bool complete, IGameListScanCallback & callback);
    bool ListGameDirectory(const GameDirectory & gameDir, IGameListScanCallback & callback, std::vector<DirFile> & files, TitleIdFolders & modFolders);
    void SetModFolderDirectories(const std::vector<GameDirectory> & gameDirs);
    void SetModFolders(TitleIdFolders modFolders);
    bool ScanGameFiles(const std::vector<DirFile> & files, IGameListScanCallback & callback);
    void ParseGameFile(const std::string & path, GameFileRecord & record) const;
    AddOnEntries ReadAddOnEntries(const std::string & path) const;
    bool RefreshUpdate(ProgramRecord & program, const UpdateIndex & updateIndex) const;
    void ReadUpdateMetadata(const UpdateCandidate & candidate, uint64_t updateTitleId, ProgramRecord & program) const;
    void ReportGame(const std::string & path, GameFileRecord & record, bool changed, IGameListScanCallback & callback);
    void ReportProgram(const std::string & path, ProgramRecord & program, bool changed, IGameListScanCallback & callback);

    void Load();
    bool Save() const;

    Systemloader & m_systemloader;
    std::mutex m_scanMutex;
    bool m_loaded;
    bool m_dirty;           // what is cached differs from what was last written
    bool m_listingComplete; // every directory of this scan could be read, so what it did not see is gone
    GameFiles m_gameFiles;
    std::vector<GameDirectory> m_gameDirectories;
    DisabledAddOns m_disabledAddOns;

    // Not under m_scanMutex: reading a game during a scan asks for its mod folders, as does a game starting meanwhile
    std::mutex m_modFoldersMutex;
    TitleIdFolders m_modFolders;
    bool m_modFoldersFound;
    std::vector<GameDirectory> m_modFolderDirectories;
    bool m_modFolderDirectoriesKnown;
};
