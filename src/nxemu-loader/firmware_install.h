#pragma once

#include "core/file_sys/vfs/vfs_types.h"
#include "core/hle/service/set/settings_types.h"
#include <filesystem>
#include <nxemu-module-spec/system_loader.h>
#include <string>

class FileSystemController;

// Installing the console's firmware into the emulated system storage.
namespace FirmwareInstall
{
using Version = Service::Set::FirmwareVersionFormat;

struct Result
{
    FirmwareInstallStatus status = FirmwareInstallStatus::NotFirmware;
    std::string version; // the package's, once it was read
};

// Puts back the firmware an installation was replacing when the emulator
// stopped. To be called before the system storage is first opened.
void RecoverInterrupted();

// Installs the firmware a .zip, a .dxci or a directory of .nca files holds.
// The new firmware is built beside the installed one and takes its place
// only once it is whole. observer may be null.
Result Install(const std::filesystem::path & source, const FileSys::VirtualFilesystem & vfs, ::FileSystemController & fs_controller,
               IFirmwareInstallObserver * observer);

bool ReadInstalledVersion(::FileSystemController & fs_controller, Version & version);
// The version of the firmware update a game card image carries.
bool ReadGameCardVersion(const FileSys::VirtualFilesystem & vfs, const std::filesystem::path & path, Version & version);
bool IsNewer(const Version & package, const Version & installed);
std::string DisplayVersion(const Version & version);
} // namespace FirmwareInstall
