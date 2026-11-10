// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/vfs/vfs.h"

class Systemloader;

namespace FileSys
{
class NACP;
} // namespace FileSys

namespace Kernel {
struct AddressMapping;
class KProcess;
} // namespace Kernel

namespace Loader {

/**
 * Identifies the type of a bootable file based on the magic value in its header.
 * @param file open file
 * @return FileType of file
 */
LoaderFileType IdentifyFile(FileSys::VirtualFile file);

/**
 * Guess the type of a bootable file from its name
 * @param name String name of bootable file
 * @return FileType of file. Note: this will return FileType::Unknown if it is unable to determine
 * a filetype, and will never return FileType::Error.
 */
LoaderFileType GuessFromFilename(const std::string & name);

/**
 * Convert a FileType into a string which can be displayed to the user.
 */
std::string GetFileTypeString(LoaderFileType type);

std::string GetResultStatusString(LoaderResultStatus status);
std::ostream& operator<<(std::ostream& os, LoaderResultStatus status);

/// Interface for loading an application
class AppLoader {
public:
    YUZU_NON_COPYABLE(AppLoader);
    YUZU_NON_MOVEABLE(AppLoader);

    struct LoadParameters {
        s32 main_thread_priority;
        uint64_t main_thread_stack_size;
        uint64_t base_address;
        uint64_t process_id;
    };
    using LoadResult = std::pair<LoaderResultStatus, std::optional<LoadParameters>>;

    explicit AppLoader(FileSys::VirtualFile file_);
    virtual ~AppLoader();

    /**
     * Returns the type of this file
     *
     * @return FileType corresponding to the loaded file
     */
    virtual LoaderFileType GetFileType() const = 0;

    /**
     * Load the application and return the created Process instance
     *
     * @param process The newly created process.
     * @param system  The system that this process is being loaded under.
     *
     * @return The status result of the operation.
     */
    virtual LoadResult Load(Systemloader & loader, ISystemModules & modules) = 0;

    /**
     * Try to verify the integrity of the file.
     */
    virtual LoaderResultStatus VerifyIntegrity(std::function<bool(size_t, size_t)> progress_callback) {
        return LoaderResultStatus::ErrorIntegrityVerificationNotImplemented;
    }

    /**
     * Get the code (typically .code section) of the application
     *
     * @param[out] buffer Reference to buffer to store data
     *
     * @return LoaderResultStatus result of function
     */
    virtual LoaderResultStatus ReadCode(std::vector<u8>& buffer) {
        return LoaderResultStatus::ErrorNotImplemented;
    }

    /**
     * Get the icon (typically icon section) of the application
     *
     * @param[out] buffer Reference to buffer to store data
     *
     * @return LoaderResultStatus result of function
     */
    virtual LoaderResultStatus ReadIcon(uint8_t * /*buffer*/, uint32_t * /*bufferSize*/)
    {
        return LoaderResultStatus::ErrorNotImplemented;
    }

    /**
     * Get the banner (typically banner section) of the application
     * In the context of NX, this is the animation that displays in the bottom right of the screen
     * when a game boots. Stored in GIF format.
     *
     * @param[out] buffer Reference to buffer to store data
     *
     * @return LoaderResultStatus result of function
     */
    virtual LoaderResultStatus ReadBanner(std::vector<u8>& buffer) {
        return LoaderResultStatus::ErrorNotImplemented;
    }

    /**
     * Get the logo (typically logo section) of the application
     * In the context of NX, this is the static image that displays in the top left of the screen
     * when a game boots. Stored in JPEG format.
     *
     * @param[out] buffer Reference to buffer to store data
     *
     * @return LoaderResultStatus result of function
     */
    virtual LoaderResultStatus ReadLogo(std::vector<u8>& buffer) {
        return LoaderResultStatus::ErrorNotImplemented;
    }

    /**
     * Get the program id of the application
     *
     * @param[out] out_program_id Reference to store program id into
     *
     * @return LoaderResultStatus result of function
     */
    virtual LoaderResultStatus ReadProgramId(uint64_t& out_program_id) {
        return LoaderResultStatus::ErrorNotImplemented;
    }

    /**
     * Get the program ids of the application
     *
     * @param[out] out_program_ids Reference to store program ids into
     *
     * @return LoaderResultStatus result of function
     */
    virtual LoaderResultStatus ReadProgramIds(uint64_t * /*buffer*/, uint32_t * /*count*/)
    {
        return LoaderResultStatus::ErrorNotImplemented;
    }

    /**
     * Get the RomFS of the application
     * Since the RomFS can be huge, we return a file reference instead of copying to a buffer
     *
     * @param[out] out_file The directory containing the RomFS
     *
     * @return LoaderResultStatus result of function
     */
    virtual LoaderResultStatus ReadRomFS(FileSys::VirtualFile& out_file) {
        return LoaderResultStatus::ErrorNotImplemented;
    }

    /**
     * Get the raw update of the application, should it come packed with one
     *
     * @param[out] out_file The raw update NCA file (Program-type)
     *
     * @return LoaderResultStatus result of function
     */
    virtual LoaderResultStatus ReadUpdateRaw(FileSys::VirtualFile& out_file) {
        return LoaderResultStatus::ErrorNotImplemented;
    }

    /**
     * Get whether or not updates can be applied to the RomFS.
     * By default, this is true, however for formats where it cannot be guaranteed that the RomFS is
     * the base game it should be set to false.
     *
     * @return bool indicating whether or not the RomFS is updatable.
     */
    virtual bool IsRomFSUpdatable() const {
        return true;
    }

    /**
     * Get the title of the application
     *
     * @param[out] title Reference to store the application title into
     *
     * @return LoaderResultStatus result of function
     */
    virtual LoaderResultStatus ReadTitle(char * /*buffer*/, uint32_t * /*bufferSize*/)
    {
        return LoaderResultStatus::ErrorNotImplemented;
    }

    /**
     * Get the control data (CNMT) of the application
     *
     * @param[out] control Reference to store the application control data into
     *
     * @return LoaderResultStatus result of function
     */
    virtual LoaderResultStatus ReadControlData(FileSys::NACP& control) {
        return LoaderResultStatus::ErrorNotImplemented;
    }

    /**
     * Get the RomFS of the manual of the application
     *
     * @param[out] out_file The raw manual RomFS of the game
     *
     * @return LoaderResultStatus result of function
     */
    virtual LoaderResultStatus ReadManualRomFS(FileSys::VirtualFile& out_file) {
        return LoaderResultStatus::ErrorNotImplemented;
    }

    using Modules = std::map<VAddr, std::string>;

    virtual LoaderResultStatus ReadNSOModules(Modules& modules) {
        return LoaderResultStatus::ErrorNotImplemented;
    }

protected:
    FileSys::VirtualFile file;
    bool is_loaded = false;
};

/**
 * Identifies a bootable file and return a suitable loader
 *
 * @param system The system context.
 * @param file   The bootable file.
 * @param program_index Specifies the index within the container of the program to launch.
 *
 * @return the best loader for this file.
 */
std::shared_ptr<AppLoader> GetLoader(Systemloader & loader, FileSys::VirtualFile file, uint64_t program_id = 0, std::size_t program_index = 0);

} // namespace Loader
