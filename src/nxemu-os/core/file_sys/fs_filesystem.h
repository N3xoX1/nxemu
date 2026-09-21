// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"

namespace FileSys {

enum class OpenDirectoryMode : u64 {
    Directory = (1 << 0),
    File = (1 << 1),

    All = (Directory | File),

    NotRequireFileSize = (1ULL << 31),
};
DECLARE_ENUM_FLAG_OPERATORS(OpenDirectoryMode)

enum class DirectoryEntryType : u8 {
    Directory = 0,
    File = 1,
};

enum class CreateOption : u8 {
    None = (0 << 0),
    BigFile = (1 << 0),
};

struct FileSystemAttribute {
    u8 dir_entry_name_length_max_defined;
    u8 file_entry_name_length_max_defined;
    u8 dir_path_name_length_max_defined;
    u8 file_path_name_length_max_defined;
    u8 utf16_create_dir_path_len_max_defined;
    u8 utf16_delete_dir_path_len_max_defined;
    u8 utf16_rename_src_dir_path_len_max_defined;
    u8 utf16_rename_dest_dir_path_len_max_defined;
    u8 utf16_open_dir_path_len_max_defined;
    u8 utf16_dir_entry_name_length_max_defined;
    u8 utf16_file_entry_name_length_max_defined;
    u8 utf16_dir_path_name_length_max_defined;
    u8 utf16_file_path_name_length_max_defined;
    INSERT_PADDING_BYTES_NOINIT(0x1B);
    s32 dir_entry_name_length_max;
    s32 file_entry_name_length_max;
    s32 dir_path_name_length_max;
    s32 file_path_name_length_max;
    s32 utf16_create_dir_path_length_max;
    s32 utf16_delete_dir_path_length_max;
    s32 utf16_rename_src_dir_path_length_max;
    s32 utf16_rename_dest_dir_path_length_max;
    s32 utf16_open_dir_path_length_max;
    s32 utf16_dir_entry_name_length_max;
    s32 utf16_file_entry_name_length_max;
    s32 utf16_dir_path_name_length_max;
    s32 utf16_file_path_name_length_max;
    INSERT_PADDING_BYTES_NOINIT(0x64);
};
static_assert(sizeof(FileSystemAttribute) == 0xC0, "FileSystemAttribute has incorrect size");
static_assert(offsetof(FileSystemAttribute, dir_entry_name_length_max) == 0x28,
              "FileSystemAttribute has incorrect field offsets");
static_assert(offsetof(FileSystemAttribute, utf16_create_dir_path_length_max) == 0x38,
              "FileSystemAttribute has incorrect field offsets");
static_assert(offsetof(FileSystemAttribute, utf16_dir_entry_name_length_max) == 0x4C,
              "FileSystemAttribute has incorrect field offsets");
static_assert(offsetof(FileSystemAttribute, utf16_file_path_name_length_max) == 0x58,
              "FileSystemAttribute has incorrect field offsets");

} // namespace FileSys
