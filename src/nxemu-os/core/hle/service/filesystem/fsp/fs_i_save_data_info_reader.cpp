// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_save_data_info_reader.h"

namespace Service::FileSystem {

ISaveDataInfoReader::ISaveDataInfoReader(Core::System& system_)
    : ServiceFramework{system_, "ISaveDataInfoReader"} {
    static const FunctionInfo functions[] = {
        {0, D<&ISaveDataInfoReader::ReadSaveDataInfo>, "ReadSaveDataInfo"},
    };
    RegisterHandlers(functions);
}

ISaveDataInfoReader::~ISaveDataInfoReader() = default;

// Returns an empty iterator. nxemu does not currently enumerate save data;
// callers receive out_count = 0, which is a legal end-of-iteration response.
Result ISaveDataInfoReader::ReadSaveDataInfo(
    Out<u64> out_count, OutArray<SaveDataInfo, BufferAttr_HipcMapAlias> out_entries) {
    LOG_DEBUG(Service_FS, "called");

    const u64 remaining = info.size() > next_entry_index ? info.size() - next_entry_index : 0;
    const u64 to_copy = std::min<u64>(remaining, out_entries.size());

    for (u64 i = 0; i < to_copy; ++i) {
        out_entries[i] = info[next_entry_index + i];
    }
    next_entry_index += to_copy;
    *out_count = to_copy;

    R_SUCCEED();
}

} // namespace Service::FileSystem
