#pragma once
#include <nxemu-module-spec/system_loader.h>
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
};

class GameListCache
{
public:
    explicit GameListCache(Systemloader & systemloader);
    ~GameListCache();

    bool Scan(const std::vector<GameDirectory> & gameDirs, IGameListScanCallback & callback);
    std::vector<GameDirectory> Directories();
    std::vector<std::string> GameDirectoryAddOnFiles(uint64_t baseProgramId);

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

    struct AddOnFileRecord
    {
        FileStamp stamp;
        AddOnEntries entries;
        bool seen = false;
    };

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

    using AddOnFiles = std::map<std::string, AddOnFileRecord>;
    using GameFiles = std::map<std::string, GameFileRecord>;
    using DisabledAddOns = std::map<uint64_t, std::vector<std::string>>;

    bool ScanAddOnDirectories(IGameListScanCallback & callback);
    bool ScanGameDirectory(const GameDirectory & gameDir, IGameListScanCallback & callback);
    void ParseGameFile(const std::string & path, GameFileRecord & record) const;
    AddOnEntries ReadAddOnEntries(const std::string & path) const;
    bool RefreshUpdate(ProgramRecord & program, const UpdateIndex & addOnIndex, const UpdateIndex & gameDirIndex) const;
    void ReadUpdateMetadata(const UpdateCandidate & candidate, uint64_t updateTitleId, ProgramRecord & program) const;
    void ReportGame(const std::string & path, GameFileRecord & record, bool changed, IGameListScanCallback & callback);
    void ReportProgram(const std::string & path, ProgramRecord & program, bool changed, IGameListScanCallback & callback);

    void Load();
    void Save() const;

    Systemloader & m_systemloader;
    std::mutex m_scanMutex;
    bool m_loaded;
    AddOnFiles m_addOnFiles;
    GameFiles m_gameFiles;
    std::vector<GameDirectory> m_gameDirectories;
    DisabledAddOns m_disabledAddOns;
};
