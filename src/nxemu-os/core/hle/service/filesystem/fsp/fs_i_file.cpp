// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/file_sys/errors.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_file.h"
#include "core/hle/service/filesystem/storage_read_timing.h"

namespace Service::FileSystem {

IFile::IFile(Core::System& system_, IVirtualFilePtr && file_) : 
    ServiceFramework{system_, "IFile"}, 
    backend{std::make_unique<FileSys::Fsa::IFile>(std::move(file_))}
{
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &IFile::ReadWithTiming, "Read"},
        {1, D<&IFile::Write>, "Write"},
        {2, D<&IFile::Flush>, "Flush"},
        {3, D<&IFile::SetSize>, "SetSize"},
        {4, D<&IFile::GetSize>, "GetSize"},
        {5, nullptr, "OperateRange"},
        {6, nullptr, "OperateRangeWithBuffer"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

void IFile::ReadWithTiming(HLERequestContext& ctx) {
    ReplyWithStorageReadTiming(system, ctx, [&] { CmifReplyWrap<true, &IFile::Read>(ctx); });
}

Result IFile::Read(FileSys::ReadOption option, Out<s64> out_size, s64 offset, const OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_buffer, s64 size)
{
    LOG_DEBUG(Service_FS, "called, option={}, offset=0x{:X}, length={}", option.value, offset, size);
    R_UNLESS(size >= 0 && static_cast<u64>(size) <= out_buffer.size(), FileSys::ResultInvalidSize);

    // Read the data from the Storage backend
    R_RETURN(backend->Read(reinterpret_cast<size_t*>(out_size.Get()), offset, out_buffer.data(), size));
}

Result IFile::Write(const InBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> buffer, FileSys::WriteOption option, s64 offset, s64 size)
{
    LOG_DEBUG(Service_FS, "called, option={}, offset=0x{:X}, length={}", option.value, offset, size);
    R_RETURN(backend->Write(offset, buffer.data(), size, option));
}

Result IFile::Flush()
{
    LOG_DEBUG(Service_FS, "called");
    R_RETURN(backend->Flush());
}

Result IFile::SetSize(s64 size)
{
    LOG_DEBUG(Service_FS, "called, size={}", size);
    R_RETURN(backend->SetSize(size));
}

Result IFile::GetSize(Out<s64> out_size)
{
    LOG_DEBUG(Service_FS, "called");

    R_RETURN(backend->GetSize(out_size));
}

} // namespace Service::FileSystem
