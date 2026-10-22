// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/file_sys/errors.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_storage.h"
#include "core/hle/service/filesystem/storage_read_timing.h"
#include <nxemu-module-spec/system_loader.h>

namespace Service::FileSystem {

IStorage::IStorage(Core::System& system_, IVirtualFilePtr && backend_)
    : ServiceFramework{system_, "IStorage"}, backend(std::move(backend_)) {
    static const FunctionInfo functions[] = {
        {0, &IStorage::ReadWithTiming, "Read"},
        {1, nullptr, "Write"},
        {2, nullptr, "Flush"},
        {3, nullptr, "SetSize"},
        {4, D<&IStorage::GetSize>, "GetSize"},
        {5, nullptr, "OperateRange"},
    };
    RegisterHandlers(functions);
}

void IStorage::ReadWithTiming(HLERequestContext& ctx) {
    ReplyWithStorageReadTiming(system, ctx, [&] { CmifReplyWrap<true, &IStorage::Read>(ctx); });
}

Result IStorage::Read(OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_bytes, s64 offset, s64 length)
{
    LOG_DEBUG(Service_FS, "called, offset=0x{:X}, length={}", offset, length);

    R_UNLESS(length >= 0, FileSys::ResultInvalidSize);
    R_UNLESS(offset >= 0, FileSys::ResultInvalidOffset);
    R_UNLESS(static_cast<u64>(length) <= out_bytes.size(), FileSys::ResultInvalidSize);

    // Read the data from the Storage backend
    backend->ReadBytes(out_bytes.data(), length, offset);

    R_SUCCEED();
}

Result IStorage::GetSize(Out<u64> out_size) {
    *out_size = backend->GetSize();

    LOG_DEBUG(Service_FS, "called, size={}", *out_size);

    R_SUCCEED();
}

} // namespace Service::FileSystem
