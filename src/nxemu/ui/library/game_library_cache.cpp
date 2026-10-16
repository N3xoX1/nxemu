#include "game_library_cache.h"
#include "library_files.h"
#include <cinttypes>
#include <common/json.h>
#include <common/sha256.h>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <unordered_set>
#include <yuzu_common/fs/fs_util.h>

namespace
{
// Raised whenever an index written before could be misread.
constexpr int64_t kSchema = 3;
constexpr uintmax_t kMaxIndexBytes = 32 * 1024 * 1024;
constexpr size_t kMaxIconBytes = 8 * 1024 * 1024;

bool ReadString(const JsonValue & object, const char * key, std::string & value)
{
    const JsonValue & item = object[key];
    if (!item.isString())
    {
        return false;
    }
    value = item.asString();
    return true;
}

std::string ApplicationToText(uint64_t application)
{
    char text[17];
    std::snprintf(text, sizeof(text), "%016" PRIX64, application);
    return text;
}

bool ReadApplication(const std::string & text, uint64_t & application)
{
    int length = 0;
    return std::sscanf(text.c_str(), "%16" SCNx64 "%n", &application, &length) == 1 && static_cast<size_t>(length) == text.size();
}

bool ReadApplications(const JsonValue & list, std::vector<uint64_t> & applications)
{
    if (!list.isArray())
    {
        return false;
    }
    for (uint32_t i = 0; i < list.size(); i++)
    {
        const std::string text = list[i].isString() ? list[i].asString() : std::string();
        uint64_t application = 0;
        if (!ReadApplication(text, application))
        {
            return false;
        }
        applications.push_back(application);
    }
    return true;
}

// Replaces the file in one step: a reader sees the old contents or the new,
// never part of either.
bool WriteFile(const std::filesystem::path & path, const char * data, size_t size)
{
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    std::error_code error;
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(data, static_cast<std::streamsize>(size));
        output.flush();
        if (!output)
        {
            output.close();
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}
} // namespace

GameLibraryCache::GameLibraryCache(std::filesystem::path directory) :
    m_directory(std::move(directory))
{
}

bool GameLibraryCache::Load()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_games.clear();
    m_containers.clear();
    m_rejections.clear();
    m_dirty = false;

    const std::filesystem::path index = m_directory / "index.json";
    std::error_code error;
    const uintmax_t bytes = std::filesystem::file_size(index, error);
    if (error || bytes > kMaxIndexBytes)
    {
        return false;
    }
    std::string contents(static_cast<size_t>(bytes), '\0');
    std::ifstream input(index, std::ios::binary);
    if (!input.read(contents.data(), static_cast<std::streamsize>(contents.size())))
    {
        return false;
    }
    JsonValue root;
    if (!JsonReader().Parse(contents.data(), contents.data() + contents.size(), root) || !root.isObject() ||
        !root["schema"].isInt() || root["schema"].asInt64() != kSchema || !root["games"].isArray() || !root["containers"].isArray())
    {
        return false;
    }

    // An entry that does not read back is dropped alone: its file is read
    // again by the next scan.
    const JsonValue & games = root["games"];
    for (uint32_t i = 0; i < games.size(); i++)
    {
        const JsonValue & item = games[i];
        CachedGame cached;
        std::string icon;
        std::string application;
        if (!item.isObject() || !ReadString(item, "path", cached.game.path) || cached.game.path.empty() ||
            !ReadString(item, "title", cached.game.title) || !ReadString(item, "version", cached.game.version) ||
            !ReadString(item, "publisher", cached.game.publisher) || !ReadString(item, "baseVersion", cached.baseVersion) ||
            !ReadString(item, "file", cached.fileSignature) || !ReadString(item, "dependencies", cached.dependencySignature) ||
            !ReadString(item, "icon", icon) || !ReadString(item, "application", application) ||
            !ReadApplication(application, cached.game.programId))
        {
            m_dirty = true;
            continue;
        }
        cached.game.size = LibraryFiles::SizeOf(cached.fileSignature);
        if (!icon.empty())
        {
            // Only a name this cache gave an icon is accepted.
            const std::filesystem::path file = IconDirectory() / Common::FS::ToU8String(icon);
            if (icon.find_first_of("/\\") != std::string::npos || !std::filesystem::is_regular_file(file, error))
            {
                m_dirty = true;
                continue;
            }
            cached.game.icon = Common::FS::PathToUTF8String(file);
        }
        m_games.insert_or_assign(cached.game.path, std::move(cached));
    }
    const JsonValue & containers = root["containers"];
    for (uint32_t i = 0; i < containers.size(); i++)
    {
        const JsonValue & item = containers[i];
        std::string path;
        CachedContainer cached;
        if (!item.isObject() || !ReadString(item, "path", path) || path.empty() || !ReadString(item, "file", cached.fileSignature) ||
            !ReadApplications(item["applications"], cached.applications))
        {
            m_dirty = true;
            continue;
        }
        m_containers.insert_or_assign(std::move(path), std::move(cached));
    }
    // Absent from the indexes written before rejections were kept.
    const JsonValue & rejections = root["rejected"];
    for (uint32_t i = 0; rejections.isArray() && i < rejections.size(); i++)
    {
        const JsonValue & item = rejections[i];
        std::string path;
        CachedRejection cached;
        if (!item.isObject() || !ReadString(item, "path", path) || path.empty() || !ReadString(item, "file", cached.fileSignature) ||
            !ReadString(item, "dependencies", cached.dependencySignature))
        {
            m_dirty = true;
            continue;
        }
        m_rejections.insert_or_assign(std::move(path), std::move(cached));
    }
    return true;
}

bool GameLibraryCache::Save()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_dirty)
    {
        return true;
    }
    JsonValue games(JsonValueType::Array);
    for (const auto & [path, cached] : m_games)
    {
        JsonValue item(JsonValueType::Object);
        item["path"] = path;
        item["application"] = ApplicationToText(cached.game.programId);
        item["title"] = cached.game.title;
        item["version"] = cached.game.version;
        item["publisher"] = cached.game.publisher;
        item["baseVersion"] = cached.baseVersion;
        item["file"] = cached.fileSignature;
        item["dependencies"] = cached.dependencySignature;
        item["icon"] = Common::FS::PathToUTF8String(std::filesystem::path{Common::FS::ToU8String(cached.game.icon)}.filename());
        games.Append(std::move(item));
    }
    JsonValue containers(JsonValueType::Array);
    for (const auto & [path, cached] : m_containers)
    {
        JsonValue item(JsonValueType::Object);
        item["path"] = path;
        item["file"] = cached.fileSignature;
        JsonValue applications(JsonValueType::Array);
        for (const uint64_t application : cached.applications)
        {
            applications.Append(ApplicationToText(application));
        }
        item["applications"] = std::move(applications);
        containers.Append(std::move(item));
    }
    JsonValue rejections(JsonValueType::Array);
    for (const auto & [path, cached] : m_rejections)
    {
        JsonValue item(JsonValueType::Object);
        item["path"] = path;
        item["file"] = cached.fileSignature;
        item["dependencies"] = cached.dependencySignature;
        rejections.Append(std::move(item));
    }
    JsonValue root(JsonValueType::Object);
    root["schema"] = kSchema;
    root["games"] = std::move(games);
    root["containers"] = std::move(containers);
    root["rejected"] = std::move(rejections);
    const std::string contents = JsonStyledWriter().write(root);

    std::error_code error;
    std::filesystem::create_directories(m_directory, error);
    if (error || contents.size() > kMaxIndexBytes || !WriteFile(m_directory / "index.json", contents.data(), contents.size()))
    {
        return false;
    }
    m_dirty = false;
    RemoveUnusedIcons();
    return true;
}

std::optional<CachedGame> GameLibraryCache::FindGame(const std::string & path) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto itr = m_games.find(path);
    return itr == m_games.end() ? std::nullopt : std::optional<CachedGame>(itr->second);
}

std::optional<CachedContainer> GameLibraryCache::FindContainer(const std::string & path) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto itr = m_containers.find(path);
    return itr == m_containers.end() ? std::nullopt : std::optional<CachedContainer>(itr->second);
}

std::optional<CachedRejection> GameLibraryCache::FindRejection(const std::string & path) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto itr = m_rejections.find(path);
    return itr == m_rejections.end() ? std::nullopt : std::optional<CachedRejection>(itr->second);
}

std::vector<GameEntry> GameLibraryCache::Games() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<GameEntry> games;
    games.reserve(m_games.size());
    for (const auto & [path, cached] : m_games)
    {
        games.push_back(cached.game);
    }
    return games;
}

void GameLibraryCache::PutGame(CachedGame game)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_rejections.erase(game.game.path);
    m_games.insert_or_assign(game.game.path, std::move(game));
    m_dirty = true;
}

void GameLibraryCache::PutRejection(const std::string & path, CachedRejection rejection)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_games.erase(path);
    m_rejections.insert_or_assign(path, std::move(rejection));
    m_dirty = true;
}

void GameLibraryCache::PutContainer(const std::string & path, CachedContainer container)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_containers.insert_or_assign(path, std::move(container));
    m_dirty = true;
}

void GameLibraryCache::Prune(const std::function<bool(const std::string & path)> & keep)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto rejected = [&keep](const auto & entry) { return !keep(entry.first); };
    if (std::erase_if(m_games, rejected) + std::erase_if(m_containers, rejected) + std::erase_if(m_rejections, rejected) != 0)
    {
        m_dirty = true;
    }
}

std::string GameLibraryCache::StoreIcon(const std::vector<uint8_t> & image)
{
    if (image.empty() || image.size() > kMaxIconBytes)
    {
        return {};
    }
    const char * data = reinterpret_cast<const char *>(image.data());
    // Games sharing an icon share its file, and an icon that changes gets
    // another: whoever shows the old one is not left with a stale image.
    const std::filesystem::path file = IconDirectory() / sha256(std::string(data, image.size()));

    std::lock_guard<std::mutex> lock(m_mutex);
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error))
    {
        std::filesystem::create_directories(IconDirectory(), error);
        if (error || !WriteFile(file, data, image.size()))
        {
            return {};
        }
    }
    return Common::FS::PathToUTF8String(file);
}

void GameLibraryCache::RemoveUnusedIcons() const
{
    std::unordered_set<std::string> used;
    for (const auto & [path, cached] : m_games)
    {
        used.insert(cached.game.icon);
    }
    std::error_code error;
    for (std::filesystem::directory_iterator itr(IconDirectory(), error); !error && itr != std::filesystem::directory_iterator(); itr.increment(error))
    {
        if (!used.contains(Common::FS::PathToUTF8String(itr->path())))
        {
            std::error_code removeError;
            std::filesystem::remove(itr->path(), removeError);
        }
    }
}
