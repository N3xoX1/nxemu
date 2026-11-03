// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include "core/file_sys/fsa/fs_i_file.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/service.h"

class IVirtualFilePtr;

namespace Service::FileSystem {

// Counts guest changes to files and directories, so shutdown can wait for a save to settle.
inline std::atomic<u64> g_file_system_write_count{0};

inline void NoteFileSystemWrite() {
    g_file_system_write_count.fetch_add(1, std::memory_order_relaxed);
}

inline u64 GetFileSystemWriteCount() {
    return g_file_system_write_count.load(std::memory_order_relaxed);
}

class IFile final : public ServiceFramework<IFile>
{
public:
    explicit IFile(Core::System & system_, IVirtualFilePtr && file_);

private:
    std::unique_ptr<FileSys::Fsa::IFile> backend;
    void ReadWithTiming(HLERequestContext& ctx);

    Result Read(FileSys::ReadOption option, Out<s64> out_size, s64 offset, const OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_buffer, s64 size);
    Result Write(const InBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> buffer, FileSys::WriteOption option, s64 offset, s64 size);
    Result Flush();
    Result SetSize(s64 size);
    Result GetSize(Out<s64> out_size);
};

} // namespace Service::FileSystem
