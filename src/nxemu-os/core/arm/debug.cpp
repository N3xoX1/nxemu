// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "debug.h"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <string>

#include "core/hle/kernel/k_process.h"

namespace Core {

namespace {
using Modules = std::map<VAddr, std::string>;

Modules FindModules(Kernel::KProcess * process)
{
    Modules modules;

    auto & page_table = process->GetKPageTable();
    auto & memory = process->GetMemory();
    VAddr cur_addr = 0;

    while (true)
    {
        Kernel::KMemoryInfo mem_info{};
        Kernel::Svc::PageInfo page_info{};
        if (page_table.QueryInfo(std::addressof(mem_info), std::addressof(page_info), cur_addr).IsFailure())
        {
            break;
        }

        const auto svc_mem_info = mem_info.GetSvcMemoryInfo();
        if (svc_mem_info.permission == Kernel::Svc::MemoryPermission::ReadExecute &&
            (svc_mem_info.state == Kernel::Svc::MemoryState::Code ||
             svc_mem_info.state == Kernel::Svc::MemoryState::AliasCode))
        {
            constexpr s32 PathLengthMax = 0x200;
            struct ModulePath
            {
                u32 zero;
                s32 path_length;
                std::array<char, PathLengthMax> path;
            } module_path{};

            if (memory.ReadBlock(svc_mem_info.base_address + svc_mem_info.size, &module_path,
                                 sizeof(module_path)) &&
                module_path.zero == 0 && module_path.path_length > 0)
            {
                const auto path_length = std::min<s32>(PathLengthMax, module_path.path_length);
                auto path_end = std::find(module_path.path.begin(),
                                          module_path.path.begin() + path_length, '\0');
                auto path_begin = module_path.path.begin();
                for (auto it = module_path.path.begin(); it != path_end; ++it)
                {
                    if (*it == '/' || *it == '\\')
                    {
                        path_begin = it + 1;
                    }
                }

                modules.emplace(svc_mem_info.base_address, std::string(path_begin, path_end));
            }
        }

        const VAddr next_address = svc_mem_info.base_address + svc_mem_info.size;
        if (next_address <= cur_addr)
        {
            break;
        }
        cur_addr = next_address;
    }

    return modules;
}

void SymbolicateBacktrace(Kernel::KProcess * process, std::vector<BacktraceEntry> & out)
{
    static constexpr std::array<u64, 2> SegmentBases{0x60000000ULL, 0x7100000000ULL};
    const auto modules = FindModules(process);
    const bool is_64 = process->Is64Bit();

    for (auto & entry : out)
    {
        VAddr base = 0;
        for (auto iter = modules.rbegin(); iter != modules.rend(); ++iter)
        {
            if (entry.original_address >= iter->first)
            {
                entry.module = iter->second;
                base = iter->first;
                break;
            }
        }

        entry.offset = entry.original_address - base;
        entry.address = SegmentBases[is_64] + entry.offset;
        if (entry.module.empty())
        {
            entry.module = "unknown";
        }
    }
}

std::vector<BacktraceEntry> GetAArch64Backtrace(Kernel::KProcess * process, const CpuThreadContext & ctx)
{
    std::vector<BacktraceEntry> out;
    auto & memory = process->GetMemory();
    auto pc = ctx.pc, lr = ctx.lr, fp = ctx.fp;

    out.push_back({"", 0, pc, 0, ""});

    // fp (= x29) points to the previous frame record.
    // Frame records are two words long:
    // fp+0 : pointer to previous frame record
    // fp+8 : value of lr for frame
    for (size_t i = 0; i < 256; i++)
    {
        out.push_back({"", 0, lr, 0, ""});
        if (!fp || (fp % 4 != 0) || !memory.IsValidVirtualAddressRange(fp, 16))
        {
            break;
        }
        lr = memory.Read64(fp + 8);
        fp = memory.Read64(fp);
    }

    SymbolicateBacktrace(process, out);

    return out;
}

std::vector<BacktraceEntry> GetAArch32Backtrace(Kernel::KProcess * process, const CpuThreadContext & ctx)
{
    std::vector<BacktraceEntry> out;
    auto & memory = process->GetMemory();
    auto pc = ctx.pc, lr = ctx.lr, fp = ctx.fp;

    out.push_back({"", 0, pc, 0, ""});

    // fp (= r11) points to the last frame record.
    // Frame records are two words long:
    // fp+0 : pointer to previous frame record
    // fp+4 : value of lr for frame
    for (size_t i = 0; i < 256; i++)
    {
        out.push_back({"", 0, lr, 0, ""});
        if (!fp || (fp % 4 != 0) || !memory.IsValidVirtualAddressRange(fp, 8))
        {
            break;
        }
        lr = memory.Read32(fp + 4);
        fp = memory.Read32(fp);
    }

    SymbolicateBacktrace(process, out);

    return out;
}

} // namespace

std::vector<BacktraceEntry> GetBacktraceFromContext(Kernel::KProcess * process, const CpuThreadContext & ctx)
{
    if (process->Is64Bit())
    {
        return GetAArch64Backtrace(process, ctx);
    }
    else
    {
        return GetAArch32Backtrace(process, ctx);
    }
}

} // namespace Core
