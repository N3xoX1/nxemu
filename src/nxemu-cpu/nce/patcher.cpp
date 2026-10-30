// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>

#include "arm_dynarmic.h"
#include "nce/guest_context.h"
#include "nce/instructions.h"
#include "nce/patcher.h"
#include "nce/read_thunk.h"
#include "nce/decode.h"
#include "nce/store_thunk.h"
#include "yuzu_common/alignment.h"
#include "yuzu_common/arm64/native_clock.h"
#include "yuzu_common/bit_cast.h"
#include "yuzu_common/literals.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/yuzu_assert.h"

namespace Core::NCE {

using namespace Common::Literals;
using namespace oaknut::util;

constexpr size_t MaxRelativeBranch = 128_MiB;
constexpr size_t MaxReadHandlerBytes = 160;
constexpr u32 ModuleCodeIndex = 0x24 / sizeof(u32);
constexpr u32 PstateNzcvMask = 0xF0000000;
constexpr u64 GuestCntfrq = 19'200'000;
constexpr u64 GuestCtrEl0 = 0x8444C004;
constexpr u32 MaxSvcId = 0x7F;
constexpr size_t DataWindowWords = 4_KiB / sizeof(u32);

// Some modules keep data in .text (.NET NativeAOT stores 1.7 MiB of it there). About one word
// in 250 of such data matches the exclusive encoding, and rewriting it corrupts the data. A
// 4 KiB window in which more than a quarter of the words do not decode is data. Zero words are
// left out: they also pad the end of a small text, after its code.
static bool IsDataWindow(std::span<const u32> words, size_t window) {
    const size_t begin = window * DataWindowWords;
    const size_t end = std::min(begin + DataWindowWords, words.size());
    size_t nonzero = 0, undecodable = 0;
    for (size_t i = begin; i < end; ++i) {
        if (words[i] == 0) continue;
        ++nonzero;
        undecodable += !IsA64Instruction(words[i]);
    }
    return undecodable * 4 > nonzero;
}

Patcher::Patcher() : c(m_patch_instructions) {
    // The first word of the patch section is always a branch to the first instruction of the
    // module.
    c.dw(0);

    // Write save context helper function.
    c.l(m_save_context);
    WriteSaveContext();

    // Write load context helper function.
    c.l(m_load_context);
    WriteLoadContext();
}

Patcher::~Patcher() = default;

bool Patcher::PatchText(const uint8_t * program_image, uint64_t image_size, uint64_t code_offset,
                        uint64_t code_size, uint64_t atomic_table, bool legacy_36bit, bool legacy_36bit_sdk, bool read_thunks, bool all_read_bases, bool float_reads, bool store_thunks) {
    // If we have patched modules but cannot reach the new module, then it needs its own patcher.
    if (total_program_size + image_size > MaxRelativeBranch && total_program_size > 0) {
        return false;
    }

    // Add a new module patch to our list
    modules.emplace_back();
    curr_patch = &modules.back();

    // The first word of the patch section is always a branch to the first instruction of the
    // module.
    curr_patch->m_branch_to_module_relocations.push_back({0, 0});

    // Retrieve text segment data.
    const auto text = std::span{program_image + code_offset, code_size};
    const auto text_words =
        std::span<const u32>{reinterpret_cast<const u32*>(text.data()), text.size() / sizeof(u32)};

    // Early Horizon rtld scans only [128 MiB, 2 GiB) for modules. A 36-bit
    // identity map on Darwin has to move that region above hard page zero.
    // Recognize the complete scanner sequence, including its range check, before
    // replacing its two constants. Other modules and unknown code are untouched.
    static constexpr std::array<u32, 12> LegacyModuleScan{
        0x320503F4, 0xB26593F8, 0x7289E9BB, 0xB000003C,
        0xF90002A9, 0xF9000519, 0xF9000535, 0xF9000328,
        0x8B180288, 0xD35BFD08, 0xF100391F, 0x54000748,
    };
    size_t legacy_scan = text_words.size();
    if (legacy_36bit) {
        const auto scan = std::search(text_words.begin(), text_words.end(),
                                      LegacyModuleScan.begin(), LegacyModuleScan.end());
        if (scan != text_words.end()) {
            legacy_scan = static_cast<size_t>(scan - text_words.begin());
            LOG_INFO(Core_ARM, "NCE: relocating verified legacy module scanner at text offset {:#x}",
                     legacy_scan * sizeof(u32));
        }
    }

    // Early nnSdk initializes its general and thread-stack allocators from fixed
    // bounds instead of querying GetInfo. Match both complete initializers together.
    static constexpr std::array<u32, 11> LegacyAllocators{
        0x91006260, 0x320503E1, 0xB25C03E2, 0x321203E3, 0x940AF05F,
        0x91010260, 0x320503E1, 0x320103E2, 0x321203E3, 0x940AF05A,
        0x9101A260,
    };
    size_t legacy_alloc = text_words.size();
    if (legacy_36bit_sdk) {
        const auto alloc = std::search(text_words.begin(), text_words.end(),
                                       LegacyAllocators.begin(), LegacyAllocators.end());
        if (alloc != text_words.end()) {
            legacy_alloc = static_cast<size_t>(alloc - text_words.begin());
            LOG_INFO(Core_ARM, "NCE: relocating verified legacy SDK allocators at text offset {:#x}",
                     legacy_alloc * sizeof(u32));
        }
    }

    // Keep the basic X0 coverage in an independent budget. Additional sites are
    // selected by static instruction frequency, to avoid filling the code budget
    // with uncommon offsets before frequently repeated pointer loads are reached.
    std::vector<bool> extra_reads(text_words.size());
    if (read_thunks && atomic_table && (all_read_bases || float_reads)) {
        std::unordered_map<u32, size_t> frequency;
        std::vector<u32> candidates;
        for (u32 i = ModuleCodeIndex; i < text_words.size(); ++i) {
            if (ReadThunk::Eligible(text_words[i]) && !ReadThunk::Basic(text_words[i]) &&
                (ReadThunk::Floating(text_words[i]) ? float_reads : all_read_bases)) {
                ++frequency[text_words[i]];
                candidates.push_back(i);
            }
        }
        std::stable_sort(candidates.begin(), candidates.end(), [&](u32 a, u32 b) {
            return frequency.at(text_words[a]) > frequency.at(text_words[b]);
        });
        const size_t slots = (16_MiB - std::min(m_extra_read_bytes, size_t{16_MiB})) / MaxReadHandlerBytes;
        for (size_t i = 0; i < std::min(slots, candidates.size()); ++i) extra_reads[candidates[i]] = true;
    }

    std::vector<bool> extra_stores(text_words.size());
    if (store_thunks && atomic_table) {
        size_t available = (8_MiB - std::min(m_store_bytes, size_t{8_MiB})) / 256;
        for (u32 i = ModuleCodeIndex; i < text_words.size() && available; ++i) {
            if (StoreThunk::Eligible(text_words[i])) {
                extra_stores[i] = true;
                --available;
            }
        }
    }

    // Windows are classified on first use: only those holding an exclusive pay for decoding.
    std::vector<s8> data_windows((text_words.size() + DataWindowWords - 1) / DataWindowWords, -1);
    const auto in_data = [&](size_t i) {
        s8& data = data_windows[i / DataWindowWords];
        if (data < 0) data = IsDataWindow(text_words, i / DataWindowWords);
        return data != 0;
    };
    size_t skipped_svcs = 0, skipped_exclusives = 0;

    // Loop through instructions, patching as needed.
    for (u32 i = ModuleCodeIndex; i < static_cast<u32>(text_words.size()); i++) {
        const u32 inst = text_words[i];

        const auto AddRelocations = [&] {
            const uintptr_t this_offset = i * sizeof(u32);
            const uintptr_t next_offset = this_offset + sizeof(u32);

            // Relocate from here to patch.
            this->BranchToPatch(this_offset);

            // Relocate from patch to next instruction.
            return next_offset;
        };

        if (i == legacy_scan || (legacy_scan < text_words.size() && i == legacy_scan + 1)) {
            const auto next = AddRelocations();
            const auto reg = i == legacy_scan ? X20 : X24;
            oaknut::Label relocated_start;
            c.LDR(reg, relocated_start);
            if (i != legacy_scan) c.NEG(reg, reg);
            this->BranchToModule(next);
            c.l(relocated_start);
            this->WriteModulePc(0);
            continue;
        }

        if (legacy_alloc < text_words.size() &&
            (i == legacy_alloc + 1 || i == legacy_alloc + 2 ||
             i == legacy_alloc + 6 || i == legacy_alloc + 7)) {
            const auto next = AddRelocations();
            const bool start = i == legacy_alloc + 1 || i == legacy_alloc + 6;
            oaknut::Label bound;
            c.LDR(start ? X1 : X2, bound);
            this->BranchToModule(next);
            c.l(bound);
            curr_patch->m_write_legacy_region_relocations.push_back(
                {c.offset(), start ? 0U : i == legacy_alloc + 2 ? 1U : 2U});
            c.dx(0);
            continue;
        }

        // SVC. Horizon has no SVC above 0x7F: a larger immediate is data, left untouched.
        if (auto svc = SVC{inst}; svc.Verify()) {
            if (svc.GetValue() > MaxSvcId) {
                ++skipped_svcs;
                continue;
            }
            WriteSvcTrampoline(AddRelocations(), svc.GetValue());
            continue;
        }

        // MRS Xn, TPIDR_EL0
        // MRS Xn, TPIDRRO_EL0
        if (auto mrs = MRS{inst};
            mrs.Verify() && (mrs.GetSystemReg() == TpidrroEl0 || mrs.GetSystemReg() == TpidrEl0)) {
            const auto src_reg = mrs.GetSystemReg() == TpidrroEl0 ? oaknut::SystemReg::TPIDRRO_EL0
                                                                  : oaknut::SystemReg::TPIDR_EL0;
            const auto dest_reg = oaknut::XReg{static_cast<int>(mrs.GetRt())};
            WriteMrsHandler(AddRelocations(), dest_reg, src_reg);
            continue;
        }

        // MRS Xn, CNTPCT_EL0
        // MRS Xn, CNTVCT_EL0
        if (auto mrs = MRS{inst};
            mrs.Verify() && (mrs.GetSystemReg() == CntpctEl0 || mrs.GetSystemReg() == CntvctEl0)) {
            WriteCntpctHandler(AddRelocations(), oaknut::XReg{static_cast<int>(mrs.GetRt())});
            continue;
        }

        // MRS Xn, CNTFRQ_EL0
        if (auto mrs = MRS{inst}; mrs.Verify() && mrs.GetSystemReg() == CntfrqEl0) {
            WriteConstantMrsHandler(AddRelocations(), oaknut::XReg{static_cast<int>(mrs.GetRt())},
                                    GuestCntfrq);
            continue;
        }

#if defined(__APPLE__)
        // MRS Xn, CTR_EL0 traps at EL0 on macOS. Report the Cortex-A57 value; its 64 byte
        // minimum line sizes are not larger than the host ones, so cache maintenance loops
        // still cover every line.
        if (auto mrs = MRS{inst}; mrs.Verify() && mrs.GetSystemReg() == CtrEl0) {
            WriteConstantMrsHandler(AddRelocations(), oaknut::XReg{static_cast<int>(mrs.GetRt())},
                                    GuestCtrEl0);
            continue;
        }
#endif

#if defined(__APPLE__)
        // Dormant until this instruction encounters an inaccessible host page. Ordinary
        // identity-mapped loads keep their original instruction and incur no overhead.
        // Bound extra code independently of guest content and retain the interpreter fallback.
        const bool basic_read = ReadThunk::Basic(inst);
        if (read_thunks && atomic_table &&
            ((basic_read && m_basic_read_bytes + MaxReadHandlerBytes <= 16_MiB) || extra_reads[i])) {
            curr_patch->m_read_handlers.push_back({c.offset(), i * sizeof(u32)});
            const size_t before = c.offset();
            WriteReadHandler((i + 1) * sizeof(u32), inst);
            ASSERT(c.offset() - before <= MaxReadHandlerBytes);
            (basic_read ? m_basic_read_bytes : m_extra_read_bytes) += c.offset() - before;
            continue;
        }
#endif

#if defined(__APPLE__)
        if (store_thunks && atomic_table && extra_stores[i]) {
            curr_patch->m_read_handlers.push_back({c.offset(), i * sizeof(u32)});
            const size_t before = c.offset();
            WriteStoreHandler((i + 1) * sizeof(u32), inst);
            ASSERT(c.offset() - before <= 256);
            m_store_bytes += c.offset() - before;
            continue;
        }
#endif

        // MSR TPIDR_EL0, Xn
        if (auto msr = MSR{inst}; msr.Verify() && msr.GetSystemReg() == TpidrEl0) {
            WriteMsrHandler(AddRelocations(), oaknut::XReg{static_cast<int>(msr.GetRt())});
            continue;
        }

        if (auto exclusive = Exclusive{inst}; exclusive.Verify()) {
            if (in_data(i)) {
                ++skipped_exclusives;
                continue;
            }
#if defined(__APPLE__)
            if (atomic_table) {
                curr_patch->m_atomic_handlers.push_back({c.offset(), i * sizeof(u32)});
                WriteExclusiveHandler((i + 1) * sizeof(u32), inst, atomic_table);
            }
#else
            (void)atomic_table;
#endif
            curr_patch->m_exclusives.push_back(i);
        }
    }

    if (skipped_svcs || skipped_exclusives) {
        LOG_INFO(Core_ARM, "NCE: left {} SVC and {} exclusive words untouched as data in .text",
                 skipped_svcs, skipped_exclusives);
    }

    // Determine patching mode for the final relocation step
    total_program_size += image_size;
    this->mode = image_size > MaxRelativeBranch ? PatchMode::PreText : PatchMode::PostData;
    return true;
}

bool Patcher::RelocateAndCopy(uint64_t load_base, uint64_t code_offset, uint64_t code_size,
                              uint8_t * program_image, uint64_t * image_size,
                              EntryTrampolines * out_trampolines, RestartPcs * out_restart_pcs, EntryTrampolines* out_atomics, EntryTrampolines* out_reads) {
    if (image_size == nullptr || program_image == nullptr || out_trampolines == nullptr ||
        m_relocate_module_index >= modules.size()) {
        return false;
    }

    const size_t patch_size = GetSectionSize();
    const size_t current_image_size = *image_size;

    // Retrieve text segment data.
    const auto text = std::span{program_image + code_offset, code_size};
    const auto text_words =
        std::span<u32>{reinterpret_cast<u32*>(text.data()), text.size() / sizeof(u32)};

    const u64 text_base = load_base + (mode == PatchMode::PreText ? patch_size : 0);
    const u64 patch_base = mode == PatchMode::PreText ? load_base : load_base + total_program_size;
    RestartPcs unused_restarts;
    if (!RelocateModule(text_base, patch_base, reinterpret_cast<u8*>(text_words.data()),
                        code_size, *out_trampolines,
                        out_restart_pcs ? *out_restart_pcs : unused_restarts, out_atomics, out_reads)) {
        return false;
    }

    // Remove the patched module size from the total. This is done so total_program_size
    // always represents the distance from the currently patched module to the patch section.
    total_program_size -= current_image_size;

    // Only copy to the program image of the last module
    if (m_relocate_module_index == modules.size()) {
        if (this->mode == PatchMode::PreText) {
            // The section is placed before the only module: PatchText starts a new patcher
            // for any module that does not fit in the branch range of the previous ones.
            ASSERT(modules.size() == 1);
            std::memcpy(program_image, m_patch_instructions.data(),
                        m_patch_instructions.size() * sizeof(u32));
        } else {
            std::memcpy(program_image + current_image_size, m_patch_instructions.data(),
                        m_patch_instructions.size() * sizeof(u32));
            *image_size = current_image_size + patch_size;
        }
        return true;
    }

    return false;
}

bool Patcher::RelocateModule(uint64_t text_base, uint64_t patch_base, uint8_t* text,
                            uint64_t code_size, EntryTrampolines& trampolines,
                            RestartPcs& restart_pcs, EntryTrampolines* atomics, EntryTrampolines* reads) {
    if (!text || m_relocate_module_index >= modules.size()) return false;
    auto& patch = modules[m_relocate_module_index];
    if (!patch.m_write_legacy_region_relocations.empty() &&
        (m_legacy_region_start < (1ULL << 32) ||
         m_legacy_region_start + 0x78000000ULL > m_legacy_region_end ||
         m_legacy_region_end > (1ULL << 36))) return false;
    const auto branch_fits = [](u64 source, u64 target) {
        const auto distance = static_cast<s64>(target) - static_cast<s64>(source);
        return distance >= -(1LL << 27) && distance < (1LL << 27) && (distance & 3) == 0;
    };
    for (const auto& rel : patch.m_branch_to_patch_relocations) {
        if (rel.module_offset + sizeof(u32) > code_size ||
            !branch_fits(text_base + rel.module_offset, patch_base + rel.patch_offset)) return false;
    }
    for (const auto& rel : patch.m_branch_to_module_relocations) {
        if (!branch_fits(patch_base + rel.patch_offset, text_base + rel.module_offset)) return false;
    }
    for (const auto& rel : patch.m_atomic_handlers) {
        if (!branch_fits(text_base + rel.module_offset, patch_base + rel.patch_offset)) return false;
    }
    for (const auto& rel : patch.m_read_handlers) {
        if (!branch_fits(text_base + rel.module_offset, patch_base + rel.patch_offset)) return false;
    }
    auto* words = reinterpret_cast<u32*>(text);
    for (const auto& rel : patch.m_branch_to_patch_relocations) {
        oaknut::CodeGenerator rc{words + rel.module_offset / sizeof(u32)};
        rc.B(static_cast<s64>(patch_base + rel.patch_offset) -
             static_cast<s64>(text_base + rel.module_offset));
    }
    for (const auto& rel : patch.m_branch_to_module_relocations) {
        oaknut::CodeGenerator rc{m_patch_instructions.data() + rel.patch_offset / sizeof(u32)};
        rc.B(static_cast<s64>(text_base + rel.module_offset) -
             static_cast<s64>(patch_base + rel.patch_offset));
    }
    for (const auto& rel : patch.m_write_module_pc_relocations) {
        oaknut::CodeGenerator rc{m_patch_instructions.data() + rel.patch_offset / sizeof(u32)};
        rc.dx(text_base + rel.module_offset);
    }
    for (const auto& rel : patch.m_write_legacy_region_relocations) {
        oaknut::CodeGenerator rc{m_patch_instructions.data() + rel.patch_offset / sizeof(u32)};
        rc.dx(rel.module_offset == 0 ? m_legacy_region_start :
              rel.module_offset == 1 ? m_legacy_region_end : m_legacy_region_start + 0x78000000ULL);
    }
    for (const auto& rel : patch.m_trampolines) {
        trampolines.emplace(text_base + rel.module_offset, patch_base + rel.patch_offset);
    }
    if (atomics) for (const auto& rel : patch.m_atomic_handlers)
        atomics->emplace(text_base + rel.module_offset, patch_base + rel.patch_offset);
    if (reads) for (const auto& rel : patch.m_read_handlers)
        reads->emplace(text_base + rel.module_offset, patch_base + rel.patch_offset);
    for (const auto i : patch.m_exclusives) {
        words[i] = Exclusive{words[i]}.AsOrdered();
    }
    if (++m_relocate_module_index == modules.size()) {
        for (const auto offset : m_thread_parameters_loads) {
            restart_pcs.emplace(patch_base + offset, patch_base + offset - sizeof(u32));
        }
        for (const auto& [fault, restart] : m_fault_restarts)
            restart_pcs.emplace(patch_base + fault, patch_base + restart);
    }
    return true;
}

size_t Patcher::GetSectionSize() const noexcept {
    return Common::AlignUp(m_patch_instructions.size() * sizeof(u32), NceHostPageSize);
}

void Patcher::WriteStoreHandler(ModuleDestLabel module_dest, u32 instruction) {
    const unsigned scratch = StoreThunk::Scratch(instruction);
    const auto reg = oaknut::XReg{static_cast<int>(scratch)};
    const auto base = oaknut::XReg{static_cast<int>(StoreThunk::Base(instruction))};
    const auto offset = StoreThunk::Offset(instruction);
    oaknut::Label prepare, fallback;
    const auto address = [&] {
        if (StoreThunk::Indexed(instruction)) {
            const unsigned shift = (instruction & (1u << 12)) ? instruction >> 30 : 0;
            c.dw(0x8b200000u | (instruction & (31u << 16)) | (instruction & (7u << 13)) |
                 (shift << 10) | (StoreThunk::Base(instruction) << 5) | scratch);
        } else if (offset < 0) c.SUB(reg, base, static_cast<u32>(-offset));
        else {
            c.ADD(reg, base, static_cast<u32>(offset) & 0xfff);
            if (offset >> 12) c.ADD(reg, reg, static_cast<u32>(offset >> 12) << 12);
        }
    };
    const ptrdiff_t spill = c.offset();
    c.STR(reg, SP, PRE_INDEXED, -16);
    address();
    // Unaligned stores stay native unless they cross a 4 KiB page. Flags are guest state.
    if (const unsigned bytes = StoreThunk::Bytes(instruction); bytes > 1) {
        c.AND(reg, reg, 0xfff);
        c.ADD(reg, reg, bytes - 1);
        c.TBNZ(reg, 12, fallback);
        address();
    }
    c.LSR(reg, reg, 39);
    c.CBNZ(reg, fallback);
    address();
    c.LSR(reg, reg, 9);
    c.AND(reg, reg, (1ULL << 30) - 8);
    m_atomic_table_relocations.push_back(c.offset());
    c.NOP();c.MOVK(reg, {0, oaknut::MovImm16Shift::SHL_32});c.MOVK(reg, {0, oaknut::MovImm16Shift::SHL_48});
    c.LDAR(reg, reg);
    c.TBZ(reg, 0, prepare);
    c.TBZ(reg, 1, prepare);
    c.AND(reg, reg, ~0xfffULL);
    if (StoreThunk::Indexed(instruction)) c.ADD(reg, reg, base);
    else {
        c.ADD(reg, reg, base);
        if (offset < 0) c.SUB(reg, reg, static_cast<u32>(-offset));
        else {c.ADD(reg, reg, static_cast<u32>(offset) & 0xfff);if (offset >> 12) c.ADD(reg, reg, static_cast<u32>(offset >> 12) << 12);}
    }
    const ptrdiff_t store = c.offset();
    if (StoreThunk::Cache(instruction)) c.dw((instruction & ~31u) | scratch);
    else if (StoreThunk::Pair(instruction)) c.dw((instruction & ~((127u << 15) | (31u << 5))) | (scratch << 5));
    else if (StoreThunk::Indexed(instruction)) c.dw((instruction & ~(31u << 5)) | (scratch << 5));
    else c.dw((instruction & ~((4095u << 10) | (31u << 5))) | (scratch << 5));
    const ptrdiff_t completed_restore = c.offset();
    c.LDR(reg, SP, POST_INDEXED, 16);
    BranchToModule(module_dest);
    c.l(prepare);
    const ptrdiff_t prepare_restore = c.offset();
    c.LDR(reg, SP, POST_INDEXED, 16);
    c.BRK(static_cast<u32>(StoreThunk::Trap::Prepare));WriteModulePc(module_dest - 4);c.dw(instruction);
    c.l(fallback);
    const ptrdiff_t fallback_restore = c.offset();
    c.LDR(reg, SP, POST_INDEXED, 16);
    const ptrdiff_t plain = c.offset();
    c.BRK(static_cast<u32>(StoreThunk::Trap::Plain));WriteModulePc(module_dest - 4);c.dw(instruction);
    const ptrdiff_t spilled = c.offset();
    c.BRK(static_cast<u32>(StoreThunk::Trap::Spilled));WriteModulePc(module_dest - 4);c.dw(instruction);
    const ptrdiff_t done = c.offset();
    c.BRK(static_cast<u32>(StoreThunk::Trap::Completed));WriteModulePc(module_dest - 4);c.dw(instruction);
    m_fault_restarts.emplace_back(spill, plain);
    m_fault_restarts.emplace_back(store, spilled);
    m_fault_restarts.emplace_back(prepare_restore, spilled);
    m_fault_restarts.emplace_back(fallback_restore, spilled);
    m_fault_restarts.emplace_back(completed_restore, done);
}

void Patcher::WriteReadHandler(ModuleDestLabel module_dest, u32 instruction) {
    const unsigned rt = instruction & 31;
    const unsigned rn = (instruction >> 5) & 31;
    const unsigned scratch = ReadThunk::Scratch(instruction);
    const auto base = oaknut::XReg{static_cast<int>(rn)};
    const unsigned size_log2 = instruction >> 30;
    const u32 offset = ((instruction >> 10) & 0xfff) << size_log2;
    const auto reg = oaknut::XReg{static_cast<int>(scratch)};
    oaknut::Label slow_restore;
    const auto address = [&] {
        c.ADD(reg, base, offset & 0xfff);
        if (offset >> 12) c.ADD(reg, reg, (offset >> 12) << 12);
    };
    const ptrdiff_t spill_pc = c.offset();
    c.STR(reg, SP, PRE_INDEXED, -16);
    address();
    // Aligned scalar accesses cannot cross a 4 KiB page. Unaligned loads use
    // the byte-wise slow path, including split pages and address wraparound.
    for (unsigned bit = 0; bit < size_log2; ++bit) c.TBNZ(reg, bit, slow_restore);
    c.LSR(reg, reg, 39);
    c.CBNZ(reg, slow_restore);
    address();
    c.LSR(reg, reg, 9);
    c.AND(reg, reg, (1ULL << 30) - 8);
    m_atomic_table_relocations.push_back(c.offset());
    c.NOP();
    c.MOVK(reg, {0, oaknut::MovImm16Shift::SHL_32});
    c.MOVK(reg, {0, oaknut::MovImm16Shift::SHL_48});
    c.LDAR(reg, reg);
    c.TBZ(reg, 0, slow_restore);
    c.AND(reg, reg, ~0xfffULL);
    c.ADD(reg, reg, base);
    c.ADD(reg, reg, offset & 0xfff);
    if (offset >> 12) c.ADD(reg, reg, (offset >> 12) << 12);
    const ptrdiff_t load_pc = c.offset();
    c.dw((instruction & ~((0xfffu << 10) | (31u << 5))) | (scratch << 5));
    const ptrdiff_t fast_restore_pc = c.offset();
    const bool restore_scratch = ReadThunk::Floating(instruction) || scratch != rt;
    if (!restore_scratch) c.ADD(SP, SP, 16);
    else c.LDR(reg, SP, POST_INDEXED, 16);
    BranchToModule(module_dest);
    c.l(slow_restore);
    const ptrdiff_t restore_pc = c.offset();
    c.LDR(reg, SP, POST_INDEXED, 16);
    const ptrdiff_t plain_pc = c.offset();
    c.BRK(ReadThunk::PlainTrap);
    WriteModulePc(module_dest - 4);
    c.dw(instruction);
    const ptrdiff_t spilled_pc = c.offset();
    c.BRK(ReadThunk::SpilledTrap);
    WriteModulePc(module_dest - 4);
    c.dw(instruction);
    if (restore_scratch) {
        const ptrdiff_t completed_pc = c.offset();
        c.BRK(ReadThunk::CompletedTrap);
        WriteModulePc(module_dest - 4);
        c.dw(instruction);
        m_fault_restarts.emplace_back(fast_restore_pc, completed_pc);
    }
    // A spill can itself hit a protected neighbour or a stack guard. Do not
    // treat its address as the guest load, nor leave a temporary SP/register behind.
    m_fault_restarts.emplace_back(spill_pc, plain_pc);
    m_fault_restarts.emplace_back(load_pc, spilled_pc);
    m_fault_restarts.emplace_back(restore_pc, spilled_pc);
}

void Patcher::WriteExclusiveHandler(ModuleDestLabel module_dest, u32 instruction, u64 table) {
    using namespace oaknut::util;
    const unsigned rn = (instruction >> 5) & 31;
    const unsigned rt = instruction & 31;
    const unsigned rt2 = (instruction >> 10) & 31;
    const unsigned rs = (instruction >> 16) & 31;
    const bool load = (instruction & (1u << 22)) != 0;
    const bool pair = (instruction & (1u << 21)) != 0;
    // A load normally uses its destination as scratch. A store needs a distinct address
    // register: ARM forbids overlap between its status output and its base register.
    // Spill only ordinary registers; never execute another exclusive operation in the
    // trampoline, which would replace the guest's hardware reservation.
    unsigned scratch = load ? rt : 31;
    if (load && (scratch == rn || scratch == 31) && pair && rt2 != rn && rt2 != 31)
        scratch = rt2;
    const bool spill = !load || scratch == rn || scratch == 31;
    // The spill of a store runs between the guest's exclusive load and store. Keep it out of
    // the reservation granule (64 bytes measured, at most a 128-byte line) of anything at or
    // above sp: a lock on the stack would otherwise lose its reservation on every attempt.
    constexpr int SpillDistance = 240;
    if (spill) {
        scratch = 0;
        while (scratch == rn || scratch == rt || (!load && scratch == rs) ||
               (pair && scratch == rt2)) ++scratch;
        c.STR(oaknut::XReg{static_cast<int>(scratch)}, SP, PRE_INDEXED, -SpillDistance);
    }
    const auto reg = oaknut::XReg{static_cast<int>(scratch)};
    const auto source = oaknut::XReg{static_cast<int>(rn)};
    const ptrdiff_t restart = c.offset();
    const auto virtual_address = [&] {
        if (rn == 31) c.ADD(reg, SP, spill ? SpillDistance : 0);
        else c.MOV(reg, source);
    };
    oaknut::Label fallback, done;
    virtual_address();
    c.LSR(reg, reg, 39);
    c.CBNZ(reg, fallback);
    virtual_address();
    c.LSR(reg, reg, 9);
    c.AND(reg, reg, (1ULL << 30) - 8);
    // The table occupies a 1 GiB aligned range; its low 30 bits are the index.
    (void)table;
    m_atomic_table_relocations.push_back(c.offset());
    c.NOP();
    c.MOVK(reg, {0, oaknut::MovImm16Shift::SHL_32});
    c.MOVK(reg, {0, oaknut::MovImm16Shift::SHL_48});
    c.LDAR(reg, reg);
    c.TBZ(reg, load ? 0 : 1, fallback);
    c.AND(reg, reg, ~0xfffULL);
    if (rn == 31) {
        c.ADD(reg, SP, reg);
        if (spill) c.ADD(reg, reg, SpillDistance);
    } else c.ADD(reg, reg, source);
    // Preserve hardware exclusive semantics using the same physical backing for every alias.
    const u32 translated = (instruction | (1u << 15)) & ~(31u << 5);
    c.dw(translated | (scratch << 5));
    c.B(done);
    c.l(fallback);
    virtual_address();
    // A protection fault goes through normal NCE GPU/access handling. Retry translation,
    // rather than an instruction whose temporary pointer was built before the fault.
    m_fault_restarts.emplace_back(c.offset(), restart);
    // STXR can fail without checking memory permissions after a signal/alias change.
    // Use an explicit private slow-path trap so GPU write tracking cannot be skipped.
    c.BRK(0x7c00 | scratch | (load ? 0 : 0x20));
    c.l(done);
    if (spill) c.LDR(reg, SP, POST_INDEXED, SpillDistance);
    BranchToModule(module_dest);
}

void Patcher::SetAtomicTable(u64 table) {
    ASSERT(table != 0 && (table & ((1ULL << 30) - 1)) == 0);
    for (const auto offset : m_atomic_table_relocations) {
        auto* words = m_patch_instructions.data() + offset / sizeof(u32);
        const auto reg = oaknut::XReg{static_cast<int>(words[1] & 31)};
        oaknut::CodeGenerator rc{words};
        if (table & 0xc0000000ULL) rc.ORR(reg, reg, table & 0xc0000000ULL);
        else rc.NOP();
        rc.MOVK(reg, {static_cast<u16>(table >> 32), oaknut::MovImm16Shift::SHL_32});
        rc.MOVK(reg, {static_cast<u16>(table >> 48), oaknut::MovImm16Shift::SHL_48});
    }
}

void Patcher::LoadThreadParameters(oaknut::XReg reg) {
#if defined(__APPLE__)
    // The Darwin kernel owns TPIDR_EL0, so the parameters live in a TSD slot.
    c.MRS(reg, oaknut::SystemReg::TPIDRRO_EL0);
    m_thread_parameters_loads.push_back(c.offset());
    c.LDR(reg, reg, ThreadParametersSlotOffset());
#else
    c.MRS(reg, oaknut::SystemReg::TPIDR_EL0);
#endif
}

void Patcher::WriteConstantMrsHandler(ModuleDestLabel module_dest, oaknut::XReg dest_reg,
                                      u64 value) {
    c.MOV(dest_reg, value);

    // Jump back to the instruction after the emulated MRS.
    this->BranchToModule(module_dest);
}

void Patcher::WriteLoadContext() {
    // This function was called, which modifies X30, so use that as a scratch register.
    // SP contains the guest X30, so save our return X30 to SP + 8, since we have allocated 16 bytes
    // of stack.
    c.STR(X30, SP, 8);
    this->LoadThreadParameters(X30);
    c.LDR(X30, X30, offsetof(NativeExecutionParameters, native_context));

    // Load system registers.
    c.LDR(W0, X30, offsetof(GuestContext, fpsr));
    c.MSR(oaknut::SystemReg::FPSR, X0);
    c.LDR(W0, X30, offsetof(GuestContext, fpcr));
    c.MSR(oaknut::SystemReg::FPCR, X0);
    c.LDR(W0, X30, offsetof(GuestContext, pstate));
    c.AND(W0, W0, PstateNzcvMask);
    c.MSR(oaknut::SystemReg::NZCV, X0);

    // Load all vector registers.
    static constexpr size_t VEC_OFF = offsetof(GuestContext, vector_registers);
    for (int i = 0; i <= 30; i += 2) {
        c.LDP(oaknut::QReg{i}, oaknut::QReg{i + 1}, X30, VEC_OFF + 16 * i);
    }

    // Load all general-purpose registers except X30.
    for (int i = 0; i <= 28; i += 2) {
        c.LDP(oaknut::XReg{i}, oaknut::XReg{i + 1}, X30, 8 * i);
    }

    // Reload our return X30 from the stack and return.
    // The patch code will reload the guest X30 for us.
    c.LDR(X30, SP, 8);
    c.RET();
}

void Patcher::WriteSaveContext() {
    // This function was called, which modifies X30, so use that as a scratch register.
    // SP contains the guest X30, so save our X30 to SP + 8, since we have allocated 16 bytes of
    // stack.
    c.STR(X30, SP, 8);
    this->LoadThreadParameters(X30);
    c.LDR(X30, X30, offsetof(NativeExecutionParameters, native_context));

    // Store all general-purpose registers except X30.
    for (int i = 0; i <= 28; i += 2) {
        c.STP(oaknut::XReg{i}, oaknut::XReg{i + 1}, X30, 8 * i);
    }

    // Store all vector registers.
    static constexpr size_t VEC_OFF = offsetof(GuestContext, vector_registers);
    for (int i = 0; i <= 30; i += 2) {
        c.STP(oaknut::QReg{i}, oaknut::QReg{i + 1}, X30, VEC_OFF + 16 * i);
    }

    // Store guest system registers, X30 and SP, using X0 and X1 as scratch registers.
    c.STP(X0, X1, SP, PRE_INDEXED, -16);
    c.LDR(X0, SP, 16);
    c.STR(X0, X30, 8 * 30);
    c.ADD(X0, SP, 32);
    c.STR(X0, X30, offsetof(GuestContext, sp));
    c.MRS(X0, oaknut::SystemReg::FPSR);
    c.STR(W0, X30, offsetof(GuestContext, fpsr));
    c.MRS(X0, oaknut::SystemReg::FPCR);
    c.STR(W0, X30, offsetof(GuestContext, fpcr));
    // The flags live in pstate, like on a signal exit, so GetContext sees them.
    c.MRS(X0, oaknut::SystemReg::NZCV);
    c.LDR(W1, X30, offsetof(GuestContext, pstate));
    c.AND(W1, W1, ~PstateNzcvMask);
    c.ORR(W0, W0, W1);
    c.STR(W0, X30, offsetof(GuestContext, pstate));
    c.LDP(X0, X1, SP, POST_INDEXED, 16);

    // Reload our return X30 from the stack, and return.
    c.LDR(X30, SP, 8);
    c.RET();
}

void Patcher::WriteSvcTrampoline(ModuleDestLabel module_dest, u32 svc_id) {
    // We are about to start saving state, so we need to lock the context.
    this->LockContext();

    // Store guest X30 to the stack. Then, save the context and restore the stack.
    // This will save all registers except PC, but we know PC at patch time.
    c.STR(X30, SP, PRE_INDEXED, -16);
    c.BL(m_save_context);
    c.LDR(X30, SP, POST_INDEXED, 16);

    // Now that we've saved all registers, we can use any registers as scratch.
    // Store PC + 4 to arm interface, since we know the instruction offset from the entry point.
    oaknut::Label pc_after_svc;
    this->LoadThreadParameters(X1);
    c.LDR(X1, X1, offsetof(NativeExecutionParameters, native_context));
    c.LDR(X2, pc_after_svc);
    c.STR(X2, X1, offsetof(GuestContext, pc));

    // Store SVC number to execute when we return
    c.MOV(X2, svc_id);
    c.STR(W2, X1, offsetof(GuestContext, svc));

    // We are calling a SVC. Clear esr_el1 and return it.
    static_assert(sizeof(std::underlying_type_t<Dynarmic::HaltReason>) <= sizeof(u64));
    oaknut::Label retry;
    c.ADD(X2, X1, offsetof(GuestContext, esr_el1));
    c.l(retry);
    c.LDAXR(X0, X2);
    c.STLXR(W3, XZR, X2);
    c.CBNZ(W3, retry);

    // Add "calling SVC" flag. Since this is X0, this is now our return value.
    c.ORR(X0, X0, static_cast<u64>(TranslateDynarmicHaltReason(CpuHaltReason::SupervisorCall)));

    // Offset the GuestContext pointer to the HostContext member.
    // STP has limited range of [-512, 504] which we can't reach otherwise
    // NB: Due to this all offsets below are from the start of HostContext.
    c.ADD(X1, X1, offsetof(GuestContext, host_ctx));

    // Reload host FPCR and FPSR, so host code does not run with the guest rounding mode.
    static_assert(offsetof(HostContext, host_fpcr) + 4 == offsetof(HostContext, host_fpsr));
    c.LDP(W2, W3, X1, offsetof(HostContext, host_fpcr));
    c.MSR(oaknut::SystemReg::FPCR, X2);
    c.MSR(oaknut::SystemReg::FPSR, X3);

    // Reload host TPIDR_EL0 and SP.
    static_assert(offsetof(HostContext, host_sp) + 8 == offsetof(HostContext, host_tpidr_el0));
    c.LDP(X2, X3, X1, offsetof(HostContext, host_sp));
    c.MOV(SP, X2);
#if !defined(__APPLE__)
    c.MSR(oaknut::SystemReg::TPIDR_EL0, X3);
#endif

    // Load callee-saved host registers and return to host.
    static constexpr size_t HOST_REGS_OFF = offsetof(HostContext, host_saved_regs);
    static constexpr size_t HOST_VREGS_OFF = offsetof(HostContext, host_saved_vregs);
    c.LDP(X19, X20, X1, HOST_REGS_OFF);
    c.LDP(X21, X22, X1, HOST_REGS_OFF + 2 * sizeof(u64));
    c.LDP(X23, X24, X1, HOST_REGS_OFF + 4 * sizeof(u64));
    c.LDP(X25, X26, X1, HOST_REGS_OFF + 6 * sizeof(u64));
    c.LDP(X27, X28, X1, HOST_REGS_OFF + 8 * sizeof(u64));
    c.LDP(X29, X30, X1, HOST_REGS_OFF + 10 * sizeof(u64));
    c.LDP(Q8, Q9, X1, HOST_VREGS_OFF);
    c.LDP(Q10, Q11, X1, HOST_VREGS_OFF + 2 * sizeof(u128));
    c.LDP(Q12, Q13, X1, HOST_VREGS_OFF + 4 * sizeof(u128));
    c.LDP(Q14, Q15, X1, HOST_VREGS_OFF + 6 * sizeof(u128));
    c.RET();

    // Write the post-SVC trampoline address, which will jump back to the guest after restoring its
    // state.
    curr_patch->m_trampolines.push_back({c.offset(), module_dest});

    // Host called this location. Save the return address so we can
    // unwind the stack properly when jumping back.
    this->LoadThreadParameters(X2);
    c.LDR(X2, X2, offsetof(NativeExecutionParameters, native_context));
    c.ADD(X0, X2, offsetof(GuestContext, host_ctx));
    c.STR(X30, X0, offsetof(HostContext, host_saved_regs) + 11 * sizeof(u64));

    // Reload all guest registers except X30 and PC.
    // The function also expects 16 bytes of stack already allocated.
    c.STR(X30, SP, PRE_INDEXED, -16);
    c.BL(m_load_context);
    c.LDR(X30, SP, POST_INDEXED, 16);

    // Use X1 as a scratch register to restore X30.
    c.STR(X1, SP, PRE_INDEXED, -16);
    this->LoadThreadParameters(X1);
    c.LDR(X1, X1, offsetof(NativeExecutionParameters, native_context));
    c.LDR(X30, X1, offsetof(GuestContext, cpu_registers) + sizeof(u64) * 30);
    c.LDR(X1, SP, POST_INDEXED, 16);

    // Unlock the context.
    this->UnlockContext();

    // Jump back to the instruction after the emulated SVC.
    this->BranchToModule(module_dest);

    // Store PC after call.
    c.l(pc_after_svc);
    this->WriteModulePc(module_dest);
}

void Patcher::WriteMrsHandler(ModuleDestLabel module_dest, oaknut::XReg dest_reg,
                              oaknut::SystemReg src_reg) {
    // Retrieve emulated TLS register from GuestContext.
    this->LoadThreadParameters(dest_reg);
    if (src_reg == oaknut::SystemReg::TPIDRRO_EL0) {
        c.LDR(dest_reg, dest_reg, offsetof(NativeExecutionParameters, tpidrro_el0));
    } else {
        c.LDR(dest_reg, dest_reg, offsetof(NativeExecutionParameters, tpidr_el0));
    }

    // Jump back to the instruction after the emulated MRS.
    this->BranchToModule(module_dest);
}

void Patcher::WriteMsrHandler(ModuleDestLabel module_dest, oaknut::XReg src_reg) {
    const auto scratch_reg = src_reg.index() == 0 ? X1 : X0;
    c.STR(scratch_reg, SP, PRE_INDEXED, -16);

    // Save guest value to NativeExecutionParameters::tpidr_el0.
    this->LoadThreadParameters(scratch_reg);
    c.STR(src_reg, scratch_reg, offsetof(NativeExecutionParameters, tpidr_el0));

    // Restore scratch register.
    c.LDR(scratch_reg, SP, POST_INDEXED, 16);

    // Jump back to the instruction after the emulated MSR.
    this->BranchToModule(module_dest);
}

void Patcher::WriteCntpctHandler(ModuleDestLabel module_dest, oaknut::XReg dest_reg) {
    static Common::Arm64::NativeClock clock{};
    const auto factor = clock.GetGuestCNTFRQFactor();
    const auto raw_factor = Common::BitCast<std::array<u64, 2>>(factor);

    const auto use_x2_x3 = dest_reg.index() == 0 || dest_reg.index() == 1;
    oaknut::XReg scratch0 = use_x2_x3 ? X2 : X0;
    oaknut::XReg scratch1 = use_x2_x3 ? X3 : X1;

    oaknut::Label factorlo;
    oaknut::Label factorhi;

    // Save scratches.
    c.STP(scratch0, scratch1, SP, PRE_INDEXED, -16);

    // Load counter value.
    c.MRS(dest_reg, oaknut::SystemReg::CNTVCT_EL0);

    // Load scaling factor.
    c.LDR(scratch0, factorlo);
    c.LDR(scratch1, factorhi);

    // Multiply low bits and get result.
    c.UMULH(scratch0, dest_reg, scratch0);

    // Multiply high bits and add low bit result.
    c.MADD(dest_reg, dest_reg, scratch1, scratch0);

    // Reload scratches.
    c.LDP(scratch0, scratch1, SP, POST_INDEXED, 16);

    // Jump back to the instruction after the emulated MRS.
    this->BranchToModule(module_dest);

    // Scaling factor constant values.
    c.l(factorlo);
    c.dx(raw_factor[0]);
    c.l(factorhi);
    c.dx(raw_factor[1]);
}

void Patcher::LockContext() {
    oaknut::Label retry;

    // Save scratches.
    c.STP(X0, X1, SP, PRE_INDEXED, -16);

    // Reload lock pointer.
    c.l(retry);
    c.CLREX();
    this->LoadThreadParameters(X0);
    c.ADD(X0, X0, offsetof(NativeExecutionParameters, lock));

    static_assert(SpinLockLocked == 0);

    // Load-linked with acquire ordering.
    c.LDAXR(W1, X0);

    // If the value was SpinLockLocked, clear monitor and retry.
    c.CBZ(W1, retry);

    // Store-conditional SpinLockLocked with relaxed ordering.
    c.STXR(W1, WZR, X0);

    // If we failed to store, retry.
    c.CBNZ(W1, retry);

    // We succeeded! Reload scratches.
    c.LDP(X0, X1, SP, POST_INDEXED, 16);
}

void Patcher::UnlockContext() {
    // Save scratches.
    c.STP(X0, X1, SP, PRE_INDEXED, -16);

    // Load lock pointer.
    this->LoadThreadParameters(X0);
    c.ADD(X0, X0, offsetof(NativeExecutionParameters, lock));

    // Load SpinLockUnlocked.
    c.MOV(W1, SpinLockUnlocked);

    // Store value with release ordering.
    c.STLR(W1, X0);

    // Load scratches.
    c.LDP(X0, X1, SP, POST_INDEXED, 16);
}

} // namespace Core::NCE
