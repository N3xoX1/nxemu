// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <oaknut/code_block.hpp>
#include <oaknut/oaknut.hpp>
#include <span>
#include <unordered_map>
#include <vector>

#include "yuzu_common/common_types.h"

namespace Core::NCE
{

enum class PatchMode : u32
{
    None,
    PreText,  ///< Patch section is inserted before .text
    PostData, ///< Patch section is inserted after .data
};

using ModuleTextAddress = u64;
using PatchTextAddress = u64;
using EntryTrampolines = std::unordered_map<ModuleTextAddress, PatchTextAddress>;
using RestartPcs = std::unordered_map<PatchTextAddress, PatchTextAddress>;

class Patcher
{
public:
    explicit Patcher();
    ~Patcher();

    bool PatchText(const uint8_t * program_image, uint64_t image_size, uint64_t code_offset, uint64_t code_size, uint64_t atomic_table = 0, bool legacy_36bit = false, bool legacy_36bit_sdk = false, bool read_thunks = false, bool all_read_bases = false, std::span<const u32> read_filter = {}, bool float_reads = false, bool store_thunks = false);
    bool RelocateAndCopy(uint64_t load_base, uint64_t code_offset, uint64_t code_size, uint8_t * program_image, uint64_t * image_size, EntryTrampolines * out_trampolines, RestartPcs * out_restart_pcs = nullptr, EntryTrampolines* out_atomics = nullptr, EntryTrampolines* out_reads = nullptr);
    // Relocate one module without assuming that its patch section is part of the image.
    // All branches are checked before either buffer is modified.
    bool RelocateModule(uint64_t text_base, uint64_t patch_base, uint8_t* text,
                        uint64_t code_size, EntryTrampolines& trampolines,
                        RestartPcs& restart_pcs, EntryTrampolines* atomics = nullptr, EntryTrampolines* reads = nullptr);
    void SetAtomicTable(u64 table);
    void SetLegacy36BitRegion(u64 start, u64 end) { m_legacy_region_start = start; m_legacy_region_end = end; }
    const std::vector<u32>& GetInstructions() const { return m_patch_instructions; }
    size_t GetSectionSize() const noexcept;

    [[nodiscard]] PatchMode GetPatchMode() const noexcept
    {
        return mode;
    }

private:
    using ModuleDestLabel = uintptr_t;

    struct Trampoline
    {
        ptrdiff_t patch_offset;
        uintptr_t module_offset;
    };

    void WriteLoadContext();
    void WriteSaveContext();
    void LockContext();
    void UnlockContext();
    void WriteSvcTrampoline(ModuleDestLabel module_dest, u32 svc_id);
    void WriteMrsHandler(ModuleDestLabel module_dest, oaknut::XReg dest_reg, oaknut::SystemReg src_reg);
    void WriteMsrHandler(ModuleDestLabel module_dest, oaknut::XReg src_reg);
    void WriteCntpctHandler(ModuleDestLabel module_dest, oaknut::XReg dest_reg);
    void WriteConstantMrsHandler(ModuleDestLabel module_dest, oaknut::XReg dest_reg, u64 value);
    void LoadThreadParameters(oaknut::XReg reg);
    void WriteStoreHandler(ModuleDestLabel module_dest, u32 instruction);
    void WriteReadHandler(ModuleDestLabel module_dest, u32 instruction);
    void WriteExclusiveHandler(ModuleDestLabel module_dest, u32 instruction, u64 atomic_table);

private:
    void BranchToPatch(uintptr_t module_dest)
    {
        curr_patch->m_branch_to_patch_relocations.push_back({c.offset(), module_dest});
    }

    void BranchToModule(uintptr_t module_dest)
    {
        curr_patch->m_branch_to_module_relocations.push_back({c.offset(), module_dest});
        c.dw(0);
    }

    void WriteModulePc(uintptr_t module_dest)
    {
        curr_patch->m_write_module_pc_relocations.push_back({c.offset(), module_dest});
        c.dx(0);
    }

private:
    // List of patch instructions we have generated.
    std::vector<u32> m_patch_instructions{};
    // PCs immediately after reading the host TSD base. That base cannot survive migration.
    std::vector<ptrdiff_t> m_thread_parameters_loads{};
    std::vector<std::pair<ptrdiff_t, ptrdiff_t>> m_fault_restarts;
    std::vector<ptrdiff_t> m_atomic_table_relocations;
    size_t m_basic_read_bytes{}, m_extra_read_bytes{}, m_store_bytes{};
    u64 m_legacy_region_start{};
    u64 m_legacy_region_end{};

    // Relocation type for relative branch from module to patch.
    struct Relocation
    {
        ptrdiff_t patch_offset;  ///< Offset in bytes from the start of the patch section.
        uintptr_t module_offset; ///< Offset in bytes from the start of the text section.
    };

    struct ModulePatch
    {
        std::vector<Trampoline> m_trampolines;
        std::vector<Relocation> m_branch_to_patch_relocations{};
        std::vector<Relocation> m_branch_to_module_relocations{};
        std::vector<Relocation> m_write_module_pc_relocations{};
        // Literal kind: 0 = MapSmall start, 1 = ASLR end, 2 = MapSmall end.
        std::vector<Relocation> m_write_legacy_region_relocations{};
        std::vector<ModuleTextAddress> m_exclusives{};
        std::vector<Trampoline> m_atomic_handlers;
        std::vector<Trampoline> m_read_handlers;
    };

    oaknut::VectorCodeGenerator c;
    oaknut::Label m_save_context{};
    oaknut::Label m_load_context{};
    PatchMode mode{PatchMode::None};
    size_t total_program_size{};
    size_t m_relocate_module_index{};
    std::vector<ModulePatch> modules;
    ModulePatch * curr_patch;
};

} // namespace Core::NCE
