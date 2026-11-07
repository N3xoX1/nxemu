// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/patch_sets.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_thread.h"
#include "core/loader/nso.h"
#include "core/memory.h"
#include "system_loader.h"
#include "yuzu_common/common_funcs.h"
#include "yuzu_common/hex_util.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/lz4_compression.h"
#include "yuzu_common/zbic_compression.h"
#include "yuzu_common/settings.h"
#include "yuzu_common/swap.h"
#include <nxemu-cpu/cpu_settings_identifiers.h>
#include <nxemu-module-spec/base.h>


extern IModuleSettings * g_settings;
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
        const auto & segment = nso_header.segments[i];
        const bool compressed = nso_header.IsSegmentCompressed(i);
        const u32 stored_size = compressed ? nso_header.segments_compressed_size[i] : segment.size;
        const u64 file_size = nso_file.GetSize();
        // Check in wide arithmetic before allocating or calling the int-sized LZ4 API.
        const u64 image_end = module_start + static_cast<u64>(segment.location) + segment.size;
        if (segment.offset > file_size || stored_size > file_size - segment.offset ||
            segment.size > static_cast<u32>(std::numeric_limits<int>::max()) ||
            stored_size > static_cast<u32>(std::numeric_limits<int>::max()) ||
            image_end > std::numeric_limits<u32>::max() - Core::Memory::YUZU_PAGEMASK)
        {
            LOG_ERROR(Loader, "Invalid NSO segment {} in {} (offset={:#x}, stored={}, size={})",
                      i, nso_file.GetName(), segment.offset, stored_size, segment.size);
            return std::nullopt;
        }
        std::vector<u8> data = nso_file.ReadBytes(stored_size, segment.offset);
        if (data.size() != stored_size)
        {
            LOG_ERROR(Loader, "Truncated NSO segment {} in {}", i, nso_file.GetName());
            return std::nullopt;
        }
        if (compressed)
        {
            if (nso_header.IsZBICCompressed())
            {
                std::vector<u8> uncompressed_data(segment.size);
                if (!Common::Compression::DecompressDataZBIC(uncompressed_data, data))
                {
                    LOG_ERROR(Loader, "ZBIC decompression failed for segment {} in {}: expected {} bytes",
                              i, nso_file.GetName(), segment.size);
                    return std::nullopt;
                }
                data = std::move(uncompressed_data);
            }
            else
            {
                data = Common::Compression::DecompressDataLZ4(data, segment.size);
                if (data.size() != segment.size)
                {
                    LOG_ERROR(Loader, "LZ4 decompression failed for segment {} in {}: expected {} bytes",
                              i, nso_file.GetName(), segment.size);
                    return std::nullopt;
                }
            }
        }
        // Preserve earlier segments even when a later segment is empty or located below them.
        program_image.resize((std::max)(program_image.size(), static_cast<size_t>(image_end)));
        if (!data.empty())
        {
            std::memcpy(program_image.data() + module_start + segment.location, data.data(), data.size());
        }
        codeset.segments[i].addr = module_start + nso_header.segments[i].location;
        codeset.segments[i].offset = module_start + nso_header.segments[i].location;
        codeset.segments[i].size = nso_header.segments[i].size;
    }

    const u64 argument_size = should_pass_arguments && !Settings::values.program_args.GetValue().empty()
                                  ? NSO_ARGUMENT_DATA_ALLOCATION_SIZE : 0;
    if (program_image.size() + argument_size + static_cast<u64>(nso_header.segments[2].bss_size) >
        std::numeric_limits<u32>::max() - Core::Memory::YUZU_PAGEMASK ||
        (argument_size != 0 && Settings::values.program_args.GetValue().size() >
                                  NSO_ARGUMENT_DATA_ALLOCATION_SIZE - sizeof(NSOArgumentHeader)))
    {
        LOG_ERROR(Loader, "Invalid NSO BSS or argument size in {}", nso_file.GetName());
        return std::nullopt;
    }

    if (argument_size != 0)
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

    // Patch sets (core/file_sys/patch_sets.h) take the page after .bss and the cave pages after it, as more of the
    // data segment: rtld takes every executable region for a module, and fails to start the game on caves mapped
    // read-execute. The JIT runs code from any mapped page. Both passes read the sets, so the process code region
    // the layout pass sizes holds them. With native code execution the caves start on a host page after the data
    // segment, unmapped for the guest, and the process makes them executable for the host (the cave segment).
    const bool native_code = g_settings->GetBool(NXCpuSetting::NceEnabled);
#if defined(__APPLE__)
    constexpr u32 NativeCodePageSize = 0x4000;
#else
    constexpr u32 NativeCodePageSize = 0x1000;
#endif
    std::optional<FileSys::PatchSets> patch_sets;
    u32 patch_page = image_size;
    if (pm)
    {
        const std::string build_id_raw = Common::HexToString(nso_header.build_id);
        // The frame step (nxemu-os core/frame_step.h) writes the patch page
        const FileSys::PatchSets::Support support{.page_written = true,
                                                  .native_code = native_code,
                                                  .cave_alignment = native_code ? NativeCodePageSize : FileSys::PatchSets::PageSize};
        patch_sets.emplace(pm->CollectPatchSetFiles(), build_id_raw.substr(0, build_id_raw.find_last_not_of('0') + 1), nso_file.GetName(), support);
        if (load_into_process)
        {
            patch_sets->LogProblems();
        }
        if (patch_sets->Empty())
        {
            patch_sets.reset();
        }
        else
        {
            const FileSys::PatchSets::Layout layout = patch_sets->Reserve(static_cast<u32>(patch_page - module_start));
            patch_page = static_cast<u32>(module_start) + layout.page_offset;
            codeset.DataSegment().size += layout.data_growth;
            image_size += layout.image_growth;
        }
    }
#if defined(__APPLE__)
    // Native code execution maps 16 KiB host pages: start every module, and the patch section
    // after the last one, on a host page, so the end of .bss never shares one with code.
    if (g_settings->GetBool(NXCpuSetting::NceEnabled))
    {
        constexpr u32 HostPageMask = 0x3FFF;
        image_size = (image_size + HostPageMask) & ~HostPageMask;
    }
#endif
    const bool relocating = patch_collection && patch_index >= 0 && load_into_process;
    const bool patch_follows_image = relocating && module_start == 0;
    if (patch_follows_image)
    {
        program_image.resize(image_size);
    }

    // The padding to the host page stays unmapped for the guest, as nothing follows .bss on
    // Horizon: the SDK reads program arguments from the end of rtld when that memory is
    // read-write, and an empty padding there gives every application argc = 0.
    for (std::size_t i = 0; i < nso_header.segments.size(); ++i)
    {
        codeset.segments[i].size = PageAlignSize(codeset.segments[i].size);
    }

    // Apply patches if necessary. With native code execution the layout pass patches too: the patcher plans its
    // trampolines from that pass's code and writes them over the load pass's, which would undo a patch at any
    // instruction they share.
    const auto name = nso_file.GetName();
    const bool patch_code = load_into_process || (native_code && patch_collection && patch_index >= 0);
    if (pm && patch_code && (pm->HasNSOPatch(nso_header.build_id, name) || Settings::values.dump_nso))
    {
        std::span<u8> patchable_section(program_image.data() + module_start, program_image.size() - module_start);
        std::vector<u8> pi_header(sizeof(NSOHeader) + patchable_section.size());
        std::memcpy(pi_header.data(), &nso_header, sizeof(NSOHeader));
        std::memcpy(pi_header.data() + sizeof(NSOHeader), patchable_section.data(), patchable_section.size());

        pi_header = pm->PatchNSO(pi_header, name);

        const size_t patched_size = std::min<size_t>(pi_header.size() - sizeof(NSOHeader), image_size - module_start);
        if (module_start + patched_size > program_image.size())
        {
            program_image.resize(module_start + patched_size);
        }
        std::copy_n(pi_header.begin() + sizeof(NSOHeader), patched_size, program_image.begin() + module_start);
    }

    // After the IPS and IPSwitch patches, so a patch set sees what they changed and leaves out a set they touched
    bool reads_patch_page = false;
    if (patch_sets && patch_code)
    {
        program_image.resize((std::max)(program_image.size(), static_cast<size_t>(image_size)));
        const std::span<u8> module_image(program_image.data() + module_start, image_size - module_start);
        reads_patch_page = patch_sets->Apply(module_image, static_cast<u32>(patch_page - module_start), load_into_process);
        if (native_code && patch_sets->CaveSize() != 0)
        {
            Kernel::CodeSet::Segment & cave_segment = codeset.CaveSegment();
            cave_segment.addr = patch_page + patch_sets->CaveOffset();
            cave_segment.size = patch_sets->CaveSize();
        }
    }

    if (patch_collection && patch_index >= 0)
    {
        const auto & code = codeset.CodeSegment();
        if (!load_into_process)
        {
            patch_collection->PatchText(patch_index, program_image.data(), image_size, (uint32_t)code.offset, code.size);
        }
        else
        {
            uint64_t patch_segment_addr = 0;
            uint32_t patch_segment_size = 0;
            const uint32_t patch_bytes = patch_collection->GetTotalPatchSize();
            const size_t data_size = program_image.size();
            if (patch_follows_image)
            {
                program_image.resize(image_size + patch_bytes);
            }
            patch_collection->Relocate(patch_index, load_base, program_image.data(), &image_size, (uint32_t)code.offset, code.size, &patch_segment_addr, &patch_segment_size);
            program_image.resize(patch_follows_image ? image_size : data_size);
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
    if (reads_patch_page)
    {
        operatingSystem.SetFrameStepAddress(load_base + patch_page);
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
