#pragma once

#include "core/file_sys/vfs/vfs_types.h"
#include "core/hle/service/set/settings_types.h"
#include <filesystem>
#include <nxemu-module-spec/system_loader.h>
#include <string>

class FileSystemController;

namespace FirmwareInstall
{
using Version = Service::Set::FirmwareVersionFormat;

struct Result
{
    FirmwareInstallStatus status = FirmwareInstallStatus::NotFirmware;
    std::string version;
};

void RecoverInterrupted();

Result Install(const std::filesystem::path & source, const FileSys::VirtualFilesystem & vfs, ::FileSystemController & fs_controller,
               IFirmwareInstallObserver * observer);

bool ReadInstalledVersion(::FileSystemController & fs_controller, Version & version);
bool ReadGameCardVersion(const FileSys::VirtualFilesystem & vfs, const std::filesystem::path & path, Version & version);
bool IsNewer(const Version & package, const Version & installed);
std::string DisplayVersion(const Version & version);
}
