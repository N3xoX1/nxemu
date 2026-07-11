// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cinttypes>
#include <cstring>
#include <vector>

#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_thread.h"
#include "core/loader/nso.h"
#include "core/memory.h"
#include "system_loader.h"
#include "yuzu_common/common_funcs.h"
#include "yuzu_common/hex_util.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/lz4_compression.h"
#include "yuzu_common/settings.h"
#include "yuzu_common/swap.h"

namespace Loader
{
namespace
{
struct MODHeader
{
    u32_le magic;
    u32_le dynamic_offset;
    u32_le bss_start_offset;
    u32_le bss_end_offset;
    u32_le eh_frame_hdr_start_offset;
    u32_le eh_frame_hdr_end_offset;
    u32_le module_offset; // Offset to runtime-generated module object. typically equal to .bss base
};
static_assert(sizeof(MODHeader) == 0x1c, "MODHeader has incorrect size.");

std::vector<u8> DecompressSegment(const std::vector<u8> & compressed_data, const NSOSegmentHeader & header)
{
    std::vector<u8> uncompressed_data = Common::Compression::DecompressDataLZ4(compressed_data, header.size);

    ASSERT_MSG(uncompressed_data.size() == header.size, "{} != {}", header.size, uncompressed_data.size());

    return uncompressed_data;
}

constexpr u32 PageAlignSize(u32 size)
{
    return static_cast<u32>((size + Core::Memory::YUZU_PAGEMASK) & ~Core::Memory::YUZU_PAGEMASK);
}
} // Anonymous namespace

bool NSOHeader::IsSegmentCompressed(size_t segment_num) const
{
    ASSERT_MSG(segment_num < 3, "Invalid segment {}", segment_num);
    return ((flags >> segment_num) & 1) != 0;
}

AppLoader_NSO::AppLoader_NSO(FileSys::VirtualFile file_) :
    AppLoader(std::move(file_))
{
}

LoaderFileType AppLoader_NSO::IdentifyType(const FileSys::VirtualFile & in_file)
{
    u32 magic = 0;
    if (in_file->ReadObject(&magic) != sizeof(magic))
    {
        return LoaderFileType::Error;
    }

    if (Common::MakeMagic('N', 'S', 'O', '0') != magic)
    {
        return LoaderFileType::Error;
    }

    return LoaderFileType::NSO;
}

std::optional<VAddr> AppLoader_NSO::LoadModule(Systemloader & loader, ISystemModules & systemModules, const FileSys::VfsFile & nso_file, VAddr load_base, bool should_pass_arguments, bool load_into_process, std::optional<FileSys::PatchManager> pm, IPatchCollection * patch_collection, int32_t patch_index)
{
    if (nso_file.GetSize() < sizeof(NSOHeader))
    {
        return std::nullopt;
    }

    NSOHeader nso_header{};
    if (sizeof(NSOHeader) != nso_file.ReadObject(&nso_header))
    {
        return std::nullopt;
    }

    if (nso_header.magic != Common::MakeMagic('N', 'S', 'O', '0'))
    {
        return std::nullopt;
    }

    const size_t module_start = patch_collection && load_into_process && patch_index >= 0 ? patch_collection->GetPreTextSize(patch_index) : 0;

    // Build program image
    Kernel::CodeSet codeset;
    Kernel::PhysicalMemory program_image;
    for (std::size_t i = 0; i < nso_header.segments.size(); ++i)
    {
        std::vector<u8> data = nso_file.ReadBytes(nso_header.segments_compressed_size[i], nso_header.segments[i].offset);
        if (nso_header.IsSegmentCompressed(i))
        {
            data = DecompressSegment(data, nso_header.segments[i]);
        }
        program_image.resize(module_start + nso_header.segments[i].location + static_cast<u32>(data.size()));
        std::memcpy(program_image.data() + module_start + nso_header.segments[i].location, data.data(), data.size());
        codeset.segments[i].addr = module_start + nso_header.segments[i].location;
        codeset.segments[i].offset = module_start + nso_header.segments[i].location;
        codeset.segments[i].size = nso_header.segments[i].size;
    }

    if (should_pass_arguments && !Settings::values.program_args.GetValue().empty())
    {
        const auto arg_data{Settings::values.program_args.GetValue()};

        codeset.DataSegment().size += NSO_ARGUMENT_DATA_ALLOCATION_SIZE;
        NSOArgumentHeader args_header{NSO_ARGUMENT_DATA_ALLOCATION_SIZE, static_cast<u32_le>(arg_data.size()), {}};
        const auto end_offset = program_image.size();
        program_image.resize(static_cast<u32>(program_image.size()) + NSO_ARGUMENT_DATA_ALLOCATION_SIZE);
        std::memcpy(program_image.data() + end_offset, &args_header, sizeof(NSOArgumentHeader));
        std::memcpy(program_image.data() + end_offset + sizeof(NSOArgumentHeader), arg_data.data(), arg_data.size());
    }

    codeset.DataSegment().size += nso_header.segments[2].bss_size;
    u32 image_size{PageAlignSize(static_cast<u32>(program_image.size()) + nso_header.segments[2].bss_size)};
    program_image.resize(image_size);

    for (std::size_t i = 0; i < nso_header.segments.size(); ++i)
    {
        codeset.segments[i].size = PageAlignSize(codeset.segments[i].size);
    }

    // Apply patches if necessary
    const auto name = nso_file.GetName();
    if (pm && (pm->HasNSOPatch(nso_header.build_id, name) || Settings::values.dump_nso))
    {
        std::span<u8> patchable_section(program_image.data() + module_start, program_image.size() - module_start);
        std::vector<u8> pi_header(sizeof(NSOHeader) + patchable_section.size());
        std::memcpy(pi_header.data(), &nso_header, sizeof(NSOHeader));
        std::memcpy(pi_header.data() + sizeof(NSOHeader), patchable_section.data(), patchable_section.size());

        pi_header = pm->PatchNSO(pi_header, name);

        std::copy(pi_header.begin() + sizeof(NSOHeader), pi_header.end(), patchable_section.data());
    }

    if (patch_collection && patch_index >= 0)
    {
        const auto & code = codeset.CodeSegment();
        if (!load_into_process)
        {
            patch_collection->PatchText(patch_index, program_image.data(), (uint32_t)program_image.size(), (uint32_t)code.offset, code.size);
        }
        else
        {
            uint64_t patch_segment_addr = 0;
            uint32_t patch_segment_size = 0;
            patch_collection->Relocate(patch_index, load_base, program_image.data(), &image_size, (uint32_t)code.offset, code.size, &patch_segment_addr, &patch_segment_size);
            if (patch_segment_size != 0)
            {
                Kernel::CodeSet::Segment & patch_segment = codeset.PatchSegment();
                patch_segment.addr = patch_segment_addr;
                patch_segment.size = patch_segment_size;
            }
        }
    }

    // If we aren't actually loading (i.e. just computing the process code layout), we are done
    if (!load_into_process)
    {
        return load_base + image_size;
    }

    // Load codeset for current process
    IOperatingSystem & operatingSystem = systemModules.OperatingSystem();
    codeset.memory = std::move(program_image);
    if (!operatingSystem.LoadModule(codeset, load_base))
    {
        return std::nullopt;
    }
    return load_base + image_size;
}

AppLoader_NSO::LoadResult AppLoader_NSO::Load(Systemloader & loader, ISystemModules & systemModules)
{
    if (is_loaded)
    {
        return {LoaderResultStatus::ErrorAlreadyLoaded, {}};
    }

    modules.clear();

    // Load module
    UNIMPLEMENTED();
    return {LoaderResultStatus::ErrorAlreadyLoaded, {}};
}

LoaderResultStatus AppLoader_NSO::ReadNSOModules(Modules & out_modules)
{
    out_modules = this->modules;
    return LoaderResultStatus::Success;
}

} // namespace Loader
