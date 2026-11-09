#include "firmware_install.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/partition_filesystem.h"
#include "core/file_sys/vfs/vfs_nx.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/filesystem.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs.h"
#include "firmware_zip.h"
#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <set>
#include <vector>
#include <yuzu_common/fs/fs.h>
#include <yuzu_common/fs/fs_util.h>
#include <yuzu_common/fs/path_util.h>
#include <yuzu_common/logging/log.h>

namespace
{
using FirmwareInstall::Version;

constexpr uint64_t SystemUpdateTitleId = 0x0100000000000816ULL;
constexpr uint64_t FirmwareVersionSystemDataId = 0x0100000000000809ULL;
constexpr size_t kCopyBufferSize = 1024 * 1024;

constexpr const char * kInstalled = "registered";
constexpr const char * kStaging = "registered.installing";
constexpr const char * kPrevious = "registered.previous";

std::filesystem::path ContentsDirectory()
{
    return Common::FS::GetYuzuPath(Common::FS::YuzuPath::NANDDir) / "system" / "Contents";
}

std::string RegisteredName(std::string_view packagePath)
{
    const auto lastComponent = [](std::string_view & path) {
        const size_t separator = path.find_last_of("/\\");
        const std::string_view component = separator == std::string_view::npos ? path : path.substr(separator + 1);
        path = separator == std::string_view::npos ? std::string_view() : path.substr(0, separator);
        return component;
    };
    std::string_view name = lastComponent(packagePath);
    if (name == "00")
    {
        name = lastComponent(packagePath);
    }
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
    {
        return {};
    }
    std::string extension(name.substr(dot));
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".nca" && extension != ".dnca")
    {
        return {};
    }
    return std::string(name.substr(0, dot)) + ".nca";
}

bool ReadVersionFile(const FileSys::NCA & nca, Version & version)
{
    const FileSys::VirtualFile romfs_file = nca.RomFS();
    const FileSys::VirtualDir romfs = romfs_file != nullptr ? FileSys::ExtractRomFS(romfs_file) : nullptr;
    const FileSys::VirtualFile version_file = romfs != nullptr ? romfs->GetFile("file") : nullptr;
    return version_file != nullptr && version_file->ReadBytes(reinterpret_cast<uint8_t *>(&version), sizeof(version), 0) == sizeof(version);
}

struct Inspection
{
    bool describesSystemUpdate = false;
    bool hasVersion = false;
    Version version{};

    void Add(const FileSys::VirtualFile & file)
    {
        const FileSys::NCA nca(file);
        if (nca.GetStatus() != LoaderResultStatus::Success)
        {
            return;
        }
        if (nca.GetType() == FileSys::NCAContentType::Meta && nca.GetTitleId() == SystemUpdateTitleId)
        {
            describesSystemUpdate = true;
        }
        if (!hasVersion && nca.GetType() == FileSys::NCAContentType::Data && nca.GetTitleId() == FirmwareVersionSystemDataId)
        {
            hasVersion = ReadVersionFile(nca, version);
        }
    }
    bool IsFirmware() const { return describesSystemUpdate && hasVersion; }
};

// Bound and validate source-resource tables before the legacy HFS parser
// allocates or follows names/offsets. Payload bytes remain streamed on demand.
FileSys::VirtualDir OpenSourceHfs(const FileSys::VirtualFile & source)
{
    if (source == nullptr || source->GetSize() < 16) return nullptr;
    const auto header = source->ReadBytes(16);
    if (header.size() != 16 || !std::equal(header.begin(), header.begin() + 4, "HFS0")) return nullptr;
    const auto u32 = [](const uint8_t * p) {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    };
    const auto u64 = [&u32](const uint8_t * p) { return uint64_t(u32(p)) | (uint64_t(u32(p + 4)) << 32); };
    const uint32_t count = u32(header.data() + 4), strings = u32(header.data() + 8);
    if (count > 4096 || strings > 1024 * 1024) return nullptr;
    const uint64_t names = 16 + uint64_t(count) * 64, data = names + strings;
    if (data > source->GetSize()) return nullptr;
    const auto metadata = source->ReadBytes(static_cast<size_t>(data));
    if (metadata.size() != data) return nullptr;
    std::set<std::string> seen;
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto * entry = metadata.data() + 16 + uint64_t(i) * 64;
        const uint64_t offset = u64(entry), size = u64(entry + 8);
        const uint32_t name_offset = u32(entry + 16);
        if (name_offset >= strings || offset > source->GetSize() - data || size > source->GetSize() - data - offset || u32(entry + 20) > size) return nullptr;
        const auto first = metadata.begin() + static_cast<size_t>(names + name_offset);
        const auto last = std::find(first, metadata.end(), uint8_t(0));
        if (last == metadata.end()) return nullptr;
        const std::string name(first, last);
        if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\") != std::string::npos || !seen.insert(name).second) return nullptr;
        ranges.emplace_back(offset, offset + size);
    }
    std::sort(ranges.begin(), ranges.end());
    for (size_t i = 1; i < ranges.size(); ++i) if (ranges[i].first < ranges[i - 1].second) return nullptr;
    auto partition = std::make_shared<FileSys::PartitionFilesystem>(source);
    return partition->GetStatus() == LoaderResultStatus::Success ? partition : nullptr;
}

// Both container kinds expose the same immutable native HFS0 view. The NX
// resource contains the original partition, not a synthesized firmware set.
FileSys::VirtualDir SourceFirmwarePartition(const FileSys::VirtualFile & file, std::string_view host_path,
                                          Version * version = nullptr)
{
    if (file == nullptr) return nullptr;
    std::array<uint8_t, 4> magic{};
    if (file->Read(magic.data(), magic.size(), 0) != magic.size()) return nullptr;
    std::vector<FileSys::VirtualDir> candidates;
    if (magic == std::array<uint8_t, 4>{'N','X','A','R'})
    {
        const auto opened = FileSys::OpenNxSession(file, host_path);
        if (!opened) return nullptr;
        const auto session = *opened.value;
        const auto & resources = session->archive().source_resources();
        for (uint32_t i = 0; i < resources.size(); ++i)
        {
            if (resources[i].kind != 1 || session->archive().applications()[resources[i].application_index].platform != 1) continue;
            const auto source = FileSys::NxSourceResourceFile(session, i, "partition.hfs0");
            if (source == nullptr) continue;
            auto partition = OpenSourceHfs(source);
            if (partition != nullptr) candidates.push_back(std::move(partition));
        }
    }
    else
    {
        FileSys::XCI card(file);
        if (card.GetStatus() == LoaderResultStatus::Success && card.GetSystemUpdateVersion() != 0)
        {
            const auto update = card.GetUpdatePartition();
            if (update != nullptr) candidates.push_back(update);
        }
    }
    FileSys::VirtualDir selected;
    Version selected_version{};
    for (const auto & partition : candidates)
    {
        Inspection inspection;
        for (const auto & entry : partition->GetFiles()) inspection.Add(entry);
        if (inspection.IsFirmware() && (selected == nullptr || FirmwareInstall::IsNewer(inspection.version, selected_version)))
        {
            selected = partition;
            selected_version = inspection.version;
        }
    }
    if (selected != nullptr && version != nullptr) *version = selected_version;
    return selected;
}

class Staging
{
public:
    Staging(const FileSys::VirtualFilesystem & vfs, IFirmwareInstallObserver * observer) :
        m_vfs(vfs),
        m_observer(observer),
        m_contents(ContentsDirectory()),
        m_directory(m_contents / kStaging),
        m_buffer(kCopyBufferSize)
    {
    }

    ~Staging()
    {
        std::error_code error;
        std::filesystem::remove_all(m_directory, error);
    }

    bool Prepare()
    {
        std::error_code error;
        std::filesystem::remove_all(m_directory, error);
        std::filesystem::create_directories(m_directory, error);
        return !error;
    }

    FirmwareInstallStatus Fill(const std::filesystem::path & source)
    {
        if (Common::FS::IsDir(source))
        {
            return FillFromDirectory(source);
        }
        if (!Common::FS::IsFile(source))
        {
            return FirmwareInstallStatus::SourceUnreadable;
        }
        const FileSys::VirtualFile file = m_vfs->OpenFile(Common::FS::PathToUTF8String(source), VirtualFileOpenMode::Read);
        if (file == nullptr)
        {
            return FirmwareInstallStatus::SourceUnreadable;
        }
        const auto update = SourceFirmwarePartition(file, Common::FS::PathToUTF8String(source));
        if (update != nullptr) return FillFromPartition(update);
        switch (ExtractFirmwareZip(file, m_directory, RegisteredName, [this](uint64_t done, uint64_t total) { return Report(FirmwareInstallStage::Copying, done, total); }))
        {
        case FirmwareZipResult::Extracted: return FirmwareInstallStatus::Installed;
        case FirmwareZipResult::Cancelled: return FirmwareInstallStatus::Cancelled;
        case FirmwareZipResult::NotAnArchive: return FirmwareInstallStatus::NotFirmware;
        default: return FirmwareInstallStatus::CopyFailed;
        }
    }

    FirmwareInstallStatus Inspect(Inspection & inspection)
    {
        std::vector<std::filesystem::path> files;
        std::error_code error;
        for (std::filesystem::directory_iterator entry(m_directory, error), end; !error && entry != end; entry.increment(error))
        {
            files.push_back(entry->path());
        }
        if (error)
        {
            return FirmwareInstallStatus::CopyFailed;
        }
        for (size_t i = 0; i < files.size(); i++)
        {
            if (!Report(FirmwareInstallStage::Checking, i, files.size()))
            {
                return FirmwareInstallStatus::Cancelled;
            }
            const FileSys::VirtualFile file = m_vfs->OpenFile(Common::FS::PathToUTF8String(files[i]), VirtualFileOpenMode::Read);
            if (file != nullptr)
            {
                inspection.Add(file);
            }
        }
        return inspection.IsFirmware() ? FirmwareInstallStatus::Installed : FirmwareInstallStatus::NotFirmware;
    }

    FirmwareInstallStatus Commit(::FileSystemController & fs_controller)
    {
        Report(FirmwareInstallStage::Replacing, 0, 0);
        const std::filesystem::path installed = m_contents / kInstalled;
        const std::filesystem::path previous = m_contents / kPrevious;
        std::error_code error;
        std::filesystem::remove_all(previous, error);
        const bool hadInstalled = std::filesystem::exists(installed, error);
        if (hadInstalled)
        {
            std::filesystem::rename(installed, previous, error);
            if (error)
            {
                LOG_ERROR(Core, "Firmware install: could not set the installed firmware aside: {}", error.message());
                return FirmwareInstallStatus::ReplaceFailed;
            }
        }
        std::filesystem::rename(m_directory, installed, error);
        if (error)
        {
            LOG_ERROR(Core, "Firmware install: could not put the new firmware in place: {}", error.message());
            if (hadInstalled)
            {
                std::filesystem::rename(previous, installed, error);
            }
            return FirmwareInstallStatus::ReplaceFailed;
        }
        fs_controller.CreateFactories(*m_vfs, true);
        std::filesystem::remove_all(previous, error);
        return FirmwareInstallStatus::Installed;
    }

private:
    Staging(const Staging &) = delete;
    Staging & operator=(const Staging &) = delete;

    bool Report(FirmwareInstallStage stage, uint64_t done, uint64_t total)
    {
        return m_observer == nullptr || m_observer->FirmwareInstallProgress(stage, done, total);
    }

    FirmwareInstallStatus FillFromDirectory(const std::filesystem::path & source)
    {
        struct Entry
        {
            std::filesystem::path path;
            std::string name;
            uint64_t size;
        };
        std::vector<Entry> entries;
        uint64_t total = 0;
        std::error_code error;
        const auto options = std::filesystem::directory_options::skip_permission_denied;
        for (std::filesystem::recursive_directory_iterator entry(source, options, error), end; !error && entry != end; entry.increment(error))
        {
            std::error_code entryError;
            if (!entry->is_regular_file(entryError))
            {
                continue;
            }
            std::string name = RegisteredName(Common::FS::PathToUTF8String(entry->path().lexically_relative(source)));
            const uint64_t size = entry->file_size(entryError);
            if (!name.empty() && !entryError)
            {
                total += size;
                entries.push_back({entry->path(), std::move(name), size});
            }
        }
        if (error)
        {
            return FirmwareInstallStatus::SourceUnreadable;
        }
        if (entries.empty())
        {
            return FirmwareInstallStatus::NotFirmware;
        }
        uint64_t done = 0;
        for (const Entry & entry : entries)
        {
            std::ifstream input(entry.path, std::ios::binary);
            if (!input)
            {
                LOG_ERROR(Core, "Firmware install: could not open {}", Common::FS::PathToUTF8String(entry.path));
                return FirmwareInstallStatus::SourceUnreadable;
            }
            const FirmwareInstallStatus status = Write(entry.name, done, total, [&input](char * data, size_t size, uint64_t) {
                input.read(data, static_cast<std::streamsize>(size));
                return static_cast<size_t>(input.gcount());
            });
            if (status != FirmwareInstallStatus::Installed)
            {
                return status;
            }
        }
        return FirmwareInstallStatus::Installed;
    }

    FirmwareInstallStatus FillFromPartition(const FileSys::VirtualDir & update)
    {
        struct Entry
        {
            FileSys::VirtualFile file;
            std::string name;
        };
        std::vector<Entry> entries;
        uint64_t total = 0;
        for (const FileSys::VirtualFile & file : update->GetFiles())
        {
            std::string name = RegisteredName(file->GetName());
            if (!name.empty())
            {
                total += file->GetSize();
                entries.push_back({file, std::move(name)});
            }
        }
        if (entries.empty())
        {
            return FirmwareInstallStatus::NotFirmware;
        }
        uint64_t done = 0;
        for (const Entry & entry : entries)
        {
            const FirmwareInstallStatus status = Write(entry.name, done, total, [&entry](char * data, size_t size, uint64_t offset) {
                return entry.file->Read(reinterpret_cast<uint8_t *>(data), size, offset);
            });
            if (status != FirmwareInstallStatus::Installed)
            {
                return status;
            }
        }
        return FirmwareInstallStatus::Installed;
    }

    template <typename Read>
    FirmwareInstallStatus Write(const std::string & name, uint64_t & done, uint64_t total, Read && read)
    {
        std::ofstream output(m_directory / std::filesystem::path(std::u8string(name.begin(), name.end())), std::ios::binary | std::ios::trunc);
        uint64_t offset = 0;
        for (size_t size = 0; output && (size = read(m_buffer.data(), m_buffer.size(), offset)) != 0; offset += size)
        {
            output.write(m_buffer.data(), static_cast<std::streamsize>(size));
            if (!Report(FirmwareInstallStage::Copying, done += size, total))
            {
                return FirmwareInstallStatus::Cancelled;
            }
        }
        output.close();
        if (!output)
        {
            LOG_ERROR(Core, "Firmware install: could not write {}", name);
            return FirmwareInstallStatus::CopyFailed;
        }
        return FirmwareInstallStatus::Installed;
    }

    const FileSys::VirtualFilesystem & m_vfs;
    IFirmwareInstallObserver * const m_observer;
    const std::filesystem::path m_contents;
    const std::filesystem::path m_directory;
    std::vector<char> m_buffer;
};

FirmwareInstall::Result InstallStaged(const std::filesystem::path & source, const FileSys::VirtualFilesystem & vfs,
                                      ::FileSystemController & fs_controller, IFirmwareInstallObserver * observer)
{
    FirmwareInstall::Result result;
    Staging staging(vfs, observer);
    if (!staging.Prepare())
    {
        result.status = FirmwareInstallStatus::StorageUnavailable;
        return result;
    }
    result.status = staging.Fill(source);
    if (result.status != FirmwareInstallStatus::Installed)
    {
        return result;
    }
    Inspection inspection;
    result.status = staging.Inspect(inspection);
    if (result.status != FirmwareInstallStatus::Installed)
    {
        return result;
    }
    result.version = FirmwareInstall::DisplayVersion(inspection.version);

    Version installed{};
    if (observer != nullptr && FirmwareInstall::ReadInstalledVersion(fs_controller, installed) &&
        !FirmwareInstall::IsNewer(inspection.version, installed) &&
        !observer->ConfirmFirmwareReplacement(result.version.c_str(), FirmwareInstall::DisplayVersion(installed).c_str()))
    {
        result.status = FirmwareInstallStatus::Cancelled;
        return result;
    }
    result.status = staging.Commit(fs_controller);
    return result;
}
}

void FirmwareInstall::RecoverInterrupted()
{
    const std::filesystem::path contents = ContentsDirectory();
    const std::filesystem::path installed = contents / kInstalled;
    const std::filesystem::path previous = contents / kPrevious;
    std::error_code error;
    std::filesystem::remove_all(contents / kStaging, error);
    if (!std::filesystem::is_directory(previous, error))
    {
        return;
    }
    if (!std::filesystem::exists(installed, error) || std::filesystem::is_empty(installed, error))
    {
        std::filesystem::remove(installed, error);
        std::filesystem::rename(previous, installed, error);
        LOG_WARNING(Core, "Firmware install: put back the firmware an interrupted installation was replacing");
    }
    else
    {
        std::filesystem::remove_all(previous, error);
    }
}

FirmwareInstall::Result FirmwareInstall::Install(const std::filesystem::path & source, const FileSys::VirtualFilesystem & vfs,
                                                 ::FileSystemController & fs_controller, IFirmwareInstallObserver * observer)
{
    try
    {
        const Result result = InstallStaged(source, vfs, fs_controller, observer);
        LOG_INFO(Core, "Firmware install: ended with status {} for version '{}'", static_cast<uint32_t>(result.status), result.version);
        return result;
    }
    catch (const std::exception & exception)
    {
        LOG_ERROR(Core, "Firmware install: {}", exception.what());
        return {FirmwareInstallStatus::CopyFailed, {}};
    }
}

bool FirmwareInstall::ReadInstalledVersion(::FileSystemController & fs_controller, Version & version)
{
    FileSys::RegisteredCache * const nand = fs_controller.SystemNANDContents();
    const FileSys::VirtualFile file = nand != nullptr ? nand->GetEntryRaw(FirmwareVersionSystemDataId, LoaderContentRecordType::Data) : nullptr;
    if (file == nullptr)
    {
        return false;
    }
    const FileSys::NCA nca(file);
    return nca.GetStatus() == LoaderResultStatus::Success && ReadVersionFile(nca, version);
}

bool FirmwareInstall::ReadGameCardVersion(const FileSys::VirtualFilesystem & vfs, const std::filesystem::path & path, Version & version)
{
    if (!Common::FS::IsFile(path))
    {
        return false;
    }
    const FileSys::VirtualFile file = vfs->OpenFile(Common::FS::PathToUTF8String(path), VirtualFileOpenMode::Read);
    if (file == nullptr)
    {
        return false;
    }
    return SourceFirmwarePartition(file, Common::FS::PathToUTF8String(path), &version) != nullptr;
}

bool FirmwareInstall::IsNewer(const Version & package, const Version & installed)
{
    if (package.major != installed.major)
    {
        return package.major > installed.major;
    }
    if (package.minor != installed.minor)
    {
        return package.minor > installed.minor;
    }
    return package.micro > installed.micro;
}

std::string FirmwareInstall::DisplayVersion(const Version & version)
{
    const auto end = std::find(version.display_version.begin(), version.display_version.end(), '\0');
    const std::string text(version.display_version.begin(), end);
    return text.empty() ? "unknown" : text;
}
