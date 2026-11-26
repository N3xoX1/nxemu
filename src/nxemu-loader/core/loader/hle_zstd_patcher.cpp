#include "core/loader/hle_zstd_patcher.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <nxemu-module-spec/hle_zstd.h>
#include <yuzu_common/hex_util.h>
#include <yuzu_common/logging/log.h>

namespace Loader::HleZstdPatcher
{
namespace
{
uint16_t ReadU16(std::span<const uint8_t> buf, size_t offset)
{
    uint16_t value = 0;
    std::memcpy(&value, buf.data() + offset, sizeof(value));
    return value;
}

uint32_t ReadU32(std::span<const uint8_t> buf, size_t offset)
{
    uint32_t value = 0;
    std::memcpy(&value, buf.data() + offset, sizeof(value));
    return value;
}

void WriteU32(std::span<uint8_t> buf, size_t offset, uint32_t value)
{
    std::memcpy(buf.data() + offset, &value, sizeof(value));
}

bool EncodeBranch26(uint32_t pc, uint32_t target, bool link, uint32_t & out_insn)
{
    const int64_t delta = static_cast<int64_t>(target) - static_cast<int64_t>(pc);
    if ((delta & 3) != 0 || delta < -(1LL << 27) || delta >= (1LL << 27))
    {
        return false;
    }
    const uint32_t imm26 = static_cast<uint32_t>((delta >> 2) & 0x03FFFFFF);
    out_insn = (link ? 0x94000000U : 0x14000000U) | imm26;
    return true;
}

int32_t DecodeBranch26Offset(uint32_t insn)
{
    int32_t imm26 = static_cast<int32_t>(insn & 0x03FFFFFFU);
    if ((imm26 & 0x02000000) != 0)
    {
        imm26 -= 0x04000000;
    }
    return imm26 * 4;
}

int32_t DecodeImm19Offset(uint32_t insn)
{
    int32_t imm19 = static_cast<int32_t>((insn >> 5) & 0x7FFFFU);
    if ((imm19 & 0x40000) != 0)
    {
        imm19 -= 0x80000;
    }
    return imm19 * 4;
}

bool IsZeroReg(uint32_t w, uint32_t reg)
{
    return w == (0xAA1F03E0U | reg) || // mov x<reg>, xzr
           w == (0x2A1F03E0U | reg) || // mov w<reg>, wzr
           w == (0xD2800000U | reg) || // mov x<reg>, #0
           w == (0x52800000U | reg);   // mov w<reg>, #0
}

bool HasNearMovLoRange(std::span<const uint8_t> text, size_t off, uint32_t rd, uint16_t lo_min, uint16_t lo_max)
{
    const size_t start = off > 32 ? off - 32 : 0;
    const size_t end = (std::min)(text.size() & ~size_t{3}, off + 32);
    for (size_t j = start; j < end; j += 4)
    {
        const uint32_t wj = ReadU32(text, j);
        if ((wj & 0x7FE0001FU) == (0x52800000U | rd))
        {
            const uint16_t imm = static_cast<uint16_t>((wj >> 5) & 0xFFFFU);
            if (imm >= lo_min && imm <= lo_max)
            {
                return true;
            }
        }
    }
    return false;
}

void FindMagicSites(std::span<const uint8_t> text,
                    uint32_t text_va,
                    std::vector<uint32_t> & fd2f_hits,
                    std::vector<uint32_t> & skip_hits)
{
    const size_t limit = text.size() & ~size_t{3};
    for (size_t off = 0; off < limit; off += 4)
    {
        const uint32_t ins = ReadU32(text, off);
        const uint32_t masked = ins & 0x7FFFFFE0U;
        if (masked == 0x72BFA5E0U)
        {
            // movk w?/x?, #0xfd2f, lsl #16 (ZSTD_MAGICNUMBER 0xFD2FB528 or legacy v0.2..v0.7 0xFD2FB522..27)
            const uint32_t rd = ins & 0x1FU;
            if (HasNearMovLoRange(text, off, rd, 0xB522, 0xB528))
            {
                fd2f_hits.push_back(text_va + static_cast<uint32_t>(off));
            }
        }
        else if (masked == 0x72A309A0U)
        {
            // movk #0x184d, lsl #16 (ZSTD_MAGIC_SKIPPABLE_START = 0x184D2A50)
            const uint32_t rd = ins & 0x1FU;
            if (HasNearMovLoRange(text, off, rd, 0x2A50, 0x2A5F))
            {
                skip_hits.push_back(text_va + static_cast<uint32_t>(off));
            }
        }
        else if (masked == 0x72A03080U)
        {
            // movk #0x0184, lsl #16 (0x184D2A50 >> 4 = 0x0184D2A5)
            const uint32_t rd = ins & 0x1FU;
            if (HasNearMovLoRange(text, off, rd, 0xD2A5, 0xD2A5))
            {
                skip_hits.push_back(text_va + static_cast<uint32_t>(off));
            }
        }
        else if (masked == 0x72BCF640U)
        {
            // movk #0xe7b2, lsl #16 (-0x184D2A50 = 0xE7B2D5B0)
            const uint32_t rd = ins & 0x1FU;
            if (HasNearMovLoRange(text, off, rd, 0xD5B0, 0xD5B0))
            {
                skip_hits.push_back(text_va + static_cast<uint32_t>(off));
            }
        }
    }
}

bool FindEnclosingFunctionStart(std::span<const uint8_t> text, uint32_t text_va, uint32_t hit_va, uint32_t & out_fva)
{
    if (hit_va < text_va)
    {
        return false;
    }
    const size_t hit_off = hit_va - text_va;
    const size_t min_off = hit_off > 0x2000 ? hit_off - 0x2000 : 0;
    for (size_t j = hit_off; j >= min_off; j -= 4)
    {
        const uint32_t w = ReadU32(text, j);
        const uint32_t prev = j >= 4 ? ReadU32(text, j - 4) : 0;
        const bool is_boundary = (prev == 0xD65F03C0U || // ret
                                  prev == 0xD65F0BFFU || // retaa
                                  prev == 0xD65F0FFFU || // retab
                                  prev == 0x00000000U || // udf #0
                                  (prev & 0xFC000000U) == 0x14000000U); // b
        const bool is_prologue = ((w & 0xFFC003E0U) == 0xA98003E0U) || // stp ..., [sp, #-imm]!
                                 ((w & 0xFF8003FFU) == 0xD10003FFU) || // sub sp, sp, #imm
                                 w == 0xD503233FU ||                   // paciasp
                                 w == 0xD503237FU ||                   // pacibsp
                                 w == 0xD503245FU ||                   // bti c
                                 w == 0xD50324DFU;                     // bti jc
        if (is_boundary && is_prologue)
        {
            out_fva = text_va + static_cast<uint32_t>(j);
            return true;
        }
        if (j < 4)
        {
            break;
        }
    }
    return false;
}

bool HasHitInRange(const std::vector<uint32_t> & sorted_hits, uint32_t start, uint32_t end)
{
    const auto it = std::lower_bound(sorted_hits.begin(), sorted_hits.end(), start);
    return it != sorted_hits.end() && *it < end;
}

// Classify a candidate function containing both ZSTD_MAGICNUMBER and
// ZSTD_MAGIC_SKIPPABLE_START. Returns 0 if not a verified Zstd one-shot
// decompression entry point, or one of HleZstd::kSvcDecompressMultiFrame /
// HleZstd::kSvcDecompressUsingDDict.
uint32_t ClassifyZstdFunction(std::span<const uint8_t> text,
                              uint32_t text_va,
                              uint32_t fva,
                              const std::vector<uint32_t> & fd2f_hits,
                              const std::vector<uint32_t> & skip_hits)
{
    if (!HasHitInRange(fd2f_hits, fva, fva + 0x2000) || !HasHitInRange(skip_hits, fva, fva + 0x3000))
    {
        return 0;
    }

    const size_t foff = fva - text_va;
    const size_t limit = text.size() & ~size_t{3};
    const size_t end_off = (std::min)(limit, foff + 56 * 4);

    // Track register aliases of input arguments x0..x7 across prologue moves,
    // plus stack-spilled ddict slots (str x5/x7, [sp, #imm]).
    std::array<int8_t, 32> arg_alias;
    arg_alias.fill(-1);
    for (int8_t r = 0; r < 8; ++r)
    {
        arg_alias[static_cast<size_t>(r)] = r;
    }
    int32_t ddict_spill_imm12[8] = {-1, -1, -1, -1, -1, -1, -1, -1};

    uint32_t written_mask = 0;
    uint32_t read_args_mask = 0;
    auto note_src = [&](uint32_t r) {
        if (r < 8 && (written_mask & (1U << r)) == 0)
        {
            read_args_mask |= (1U << r);
        }
    };

    bool has_cmp5_x4 = false;
    bool has_ddict_ldp8 = false;
    int8_t ddict_arg_for_ldp = -1;

    struct DDictBranch
    {
        int8_t arg_idx;
        uint32_t reg;
        int64_t target_off;
        bool is_cbnz;
    };
    std::vector<DDictBranch> ddict_branches;
    std::vector<int64_t> secondary_cbz_targets;

    for (size_t off = foff; off < end_off; off += 4)
    {
        const uint32_t w = ReadU32(text, off);
        if (w == 0xD65F03C0U || w == 0xD65F0BFFU || w == 0xD65F0FFFU) // ret / retaa / retab
        {
            break;
        }
        // mov Xd, Xm  (ORR Xd, XZR, Xm)
        if ((w & 0xFFE0FFE0U) == 0xAA0003E0U)
        {
            const uint32_t rd = w & 0x1FU;
            const uint32_t rm = (w >> 16) & 0x1FU;
            if (rm != 31)
            {
                note_src(rm);
                arg_alias[rd] = arg_alias[rm];
            }
            else
            {
                arg_alias[rd] = -1;
            }
            written_mask |= (1U << rd);
            continue;
        }
        // str Xt, [Xn, #imm] (64-bit unsigned offset)
        if ((w & 0xFFC00000U) == 0xF9000000U)
        {
            const uint32_t rt = w & 0x1FU;
            const uint32_t rn = (w >> 5) & 0x1FU;
            note_src(rt);
            note_src(rn);
            if (rn == 31)
            {
                const int32_t slot = static_cast<int32_t>((w >> 10) & 0xFFFU);
                for (int i = 0; i < 8; ++i)
                {
                    if (ddict_spill_imm12[i] == slot)
                    {
                        ddict_spill_imm12[i] = -1;
                    }
                }
                if (arg_alias[rt] >= 0 && arg_alias[rt] < 8)
                {
                    ddict_spill_imm12[arg_alias[rt]] = slot;
                }
            }
            continue;
        }
        // stp Xt1, Xt2, [Xn, ...]
        if ((w & 0x7F400000U) == 0x29000000U)
        {
            note_src(w & 0x1FU);
            note_src((w >> 10) & 0x1FU);
            note_src((w >> 5) & 0x1FU);
            continue;
        }
        // cmp Xn, #5  (SUBS XZR, Xn, #5)
        if ((w & 0xFFFFFC1FU) == 0xF100141FU)
        {
            const uint32_t rn = (w >> 5) & 0x1FU;
            note_src(rn);
            if (arg_alias[rn] == 4)
            {
                has_cmp5_x4 = true;
            }
            continue;
        }
        // cbz / cbnz Xt, label (64-bit)
        if ((w & 0xFE000000U) == 0xB4000000U)
        {
            const uint32_t rt = w & 0x1FU;
            note_src(rt);
            const int64_t tgt = static_cast<int64_t>(off) + DecodeImm19Offset(w);
            const bool is_cbnz = ((w >> 24) & 1U) != 0;
            if (arg_alias[rt] == 5 || arg_alias[rt] == 7)
            {
                ddict_branches.push_back({arg_alias[rt], rt, tgt, is_cbnz});
            }
            if (!is_cbnz)
            {
                secondary_cbz_targets.push_back(tgt);
            }
            continue;
        }
        // Inline ldp Xt1, Xt2, [Xn, #8]
        if ((w & 0xFFC00000U) == 0xA9400000U && ((w >> 15) & 0x7FU) == 1)
        {
            const uint32_t rn = (w >> 5) & 0x1FU;
            note_src(rn);
            if (arg_alias[rn] == 5 || arg_alias[rn] == 7)
            {
                has_ddict_ldp8 = true;
                ddict_arg_for_ldp = arg_alias[rn];
            }
        }

        // Invalidate aliases for any GPR written by this instruction so a later
        // ldr/add/sub/bl cannot falsely retain an earlier x0..x7 argument alias.
        auto clobber_gpr = [&](uint32_t r) {
            if (r < 31)
            {
                arg_alias[r] = -1;
                written_mask |= (1U << r);
            }
        };
        if ((w & 0xFC000000U) == 0x94000000U || // bl
            (w & 0xFFFFFC1FU) == 0xD63F0000U)   // blr
        {
            for (uint32_t r = 0; r <= 18; ++r)
            {
                clobber_gpr(r);
            }
            clobber_gpr(30);
            continue;
        }
        // Load pair (LDP / LDNP / LDPSW): writes Rt and Rt2
        if ((w & 0x3A400000U) == 0x28400000U)
        {
            clobber_gpr(w & 0x1FU);
            clobber_gpr((w >> 10) & 0x1FU);
            continue;
        }
        // Exclude non-GPR-writing instructions: branches, system/hint/barriers, stores, SIMD/FP.
        const uint32_t op0 = (w >> 25) & 0xFU;
        const bool is_branch_or_sys = (op0 == 0b1010 || op0 == 0b1011);
        const bool is_simd_fp = ((op0 & 0b0111) == 0b0111);
        const bool is_ldst = ((op0 & 0b0101) == 0b0100);
        if (is_branch_or_sys)
        {
            continue;
        }
        if (is_simd_fp)
        {
            // FP/SIMD-to-GPR transfers (FMOV/FCVT/UMOV/SMOV) write Rd = w & 0x1F.
            const bool is_fp_to_gpr = ((w & 0x5F200000U) == 0x1E200000U);
            const bool is_vec_to_gpr = ((w & 0xBFFFE000U) == 0x0E003C00U) || ((w & 0xBFFFE000U) == 0x0E002C00U);
            if (is_fp_to_gpr || is_vec_to_gpr)
            {
                clobber_gpr(w & 0x1FU);
            }
            continue;
        }
        if (is_ldst)
        {
            // Stores have bit 22 == 0 in regular load/store unsigned/reg/unscaled/pre/post;
            // loads (LDR/LDUR/LDRB/LDRH/LDRSW/LDRSB/LDRSH/LDR literal/LDXR/LDXP) write Rt = w & 0x1F.
            const bool is_ldr_literal = ((w & 0x3B000000U) == 0x18000000U);
            const bool is_reg_load = ((w & 0x3A000000U) == 0x38000000U) && (((w >> 22) & 0x3U) != 0);
            const bool is_exclusive_load = ((w & 0x3F400000U) == 0x08400000U);
            if (is_ldr_literal || is_reg_load || is_exclusive_load)
            {
                clobber_gpr(w & 0x1FU);
                if (is_exclusive_load && ((w >> 21) & 1U) != 0) // LDXP / LDAXP
                {
                    clobber_gpr((w >> 10) & 0x1FU);
                }
            }
            continue;
        }
        // Remaining data-processing (imm / reg): ADD/SUB/MOVZ/MOVK/ADR/ADRP/AND/ORR/EOR/BFM/CSEL/MADD/DIV...
        // All write Rd = w & 0x1F (when Rd != 31; CMP/CMN/TST use Rd == 31 and are ignored by clobber_gpr).
        clobber_gpr(w & 0x1FU);
    }

    // Inspect ddict branch targets for ZSTD_DDict_s { +0x08 dictContent, +0x10 dictSize } loads.
    for (const auto & br : ddict_branches)
    {
        auto check_spot = [&](int64_t spot) {
            if (spot < 0 || static_cast<size_t>(spot) + 24 > limit)
            {
                return;
            }
            bool saw_ldr8 = false;
            bool saw_ldr16 = false;
            uint32_t reloaded_reg = 32;
            const int32_t spill_slot = ddict_spill_imm12[br.arg_idx];
            for (size_t s = static_cast<size_t>(spot); s < static_cast<size_t>(spot) + 24; s += 4)
            {
                const uint32_t ws = ReadU32(text, s);
                // Track stack reload: ldr Xt, [sp, #spill_slot*8]
                if (spill_slot >= 0 && (ws & 0xFFC00000U) == 0xF9400000U)
                {
                    const uint32_t rn = (ws >> 5) & 0x1FU;
                    const int32_t imm12 = static_cast<int32_t>((ws >> 10) & 0xFFFU);
                    if (rn == 31 && imm12 == spill_slot)
                    {
                        reloaded_reg = ws & 0x1FU;
                    }
                }
                const auto is_ddict_reg = [&](uint32_t rn) {
                    return arg_alias[rn] == br.arg_idx || rn == br.reg || rn == reloaded_reg;
                };
                // ldp Xt1, Xt2, [Xn, #8]
                if ((ws & 0xFFC00000U) == 0xA9400000U && ((ws >> 15) & 0x7FU) == 1)
                {
                    const uint32_t rn = (ws >> 5) & 0x1FU;
                    if (is_ddict_reg(rn))
                    {
                        has_ddict_ldp8 = true;
                        ddict_arg_for_ldp = br.arg_idx;
                    }
                }
                // ldr Xt, [Xn, #8] / ldr Xt, [Xn, #16]
                if ((ws & 0xFFC00000U) == 0xF9400000U)
                {
                    const uint32_t imm12 = (ws >> 10) & 0xFFFU;
                    const uint32_t rn = (ws >> 5) & 0x1FU;
                    if (is_ddict_reg(rn))
                    {
                        if (imm12 == 1)
                        {
                            saw_ldr8 = true;
                        }
                        if (imm12 == 2)
                        {
                            saw_ldr16 = true;
                        }
                    }
                }
            }
            if (saw_ldr8 && saw_ldr16)
            {
                has_ddict_ldp8 = true;
                ddict_arg_for_ldp = br.arg_idx;
            }
        };

        if (br.is_cbnz)
        {
            check_spot(br.target_off);
        }
        else
        {
            for (const int64_t tgt : secondary_cbz_targets)
            {
                check_spot(tgt);
            }
        }
    }

    if (has_cmp5_x4 && has_ddict_ldp8 && (read_args_mask & (1U << 0)) != 0)
    {
        if (ddict_arg_for_ldp == 7)
        {
            return HleZstd::kSvcDecompressMultiFrame;
        }
        if (ddict_arg_for_ldp == 5 && (read_args_mask & ((1U << 6) | (1U << 7))) == 0)
        {
            return HleZstd::kSvcDecompressUsingDDict;
        }
    }
    return 0;
}

bool IsBoundaryInsn(uint32_t w)
{
    return w == 0xD65F03C0U || // ret
           w == 0xD65F0BFFU || // retaa
           w == 0xD65F0FFFU || // retab
           w == 0x00000000U || // udf #0
           (w & 0xFC000000U) == 0x14000000U; // b
}

// Detect the static-DCtx 4-byte-prefix probe wrapper (used across TOTK versions
// and related titles) that strips a 4-byte header (`add x3, x3, #4`), initializes
// a static DCtx, and branches on ZSTD_error_dstSize_tooSmall (`cmp w0, #0x46`).
// Guest libzstd rejects specific frames with that static DCtx that newer host
// libzstd accepts, and the caller relies on that rejection.
bool IsStaticDCtxProbeCaller(std::span<const uint8_t> text, size_t call_off)
{
    const size_t limit = text.size() & ~size_t{3};
    const size_t pre_start = call_off > 48 * 4 ? call_off - 48 * 4 : 0;
    const size_t post_end = (std::min)(limit, call_off + 16 * 4);
    bool has_add_x3_4 = false;
    for (size_t off = pre_start; off < call_off; off += 4)
    {
        if (ReadU32(text, off) == 0x91001063U) // add x3, x3, #4
        {
            has_add_x3_4 = true;
            break;
        }
    }
    if (!has_add_x3_4)
    {
        return false;
    }
    for (size_t off = call_off + 4; off < post_end; off += 4)
    {
        if (ReadU32(text, off) == 0x7101181FU) // cmp w0, #0x46 (ZSTD_error_dstSize_tooSmall)
        {
            return true;
        }
    }
    return false;
}

void FindTailCallWrappers(std::span<const uint8_t> text,
                          uint32_t text_va,
                          const std::unordered_set<uint32_t> & multiframe_vas,
                          std::unordered_map<uint32_t, uint32_t> & targets,
                          std::unordered_set<uint32_t> & wrapper_branches)
{
    if (targets.empty() || text.size() < 16)
    {
        return;
    }
    const size_t limit = (text.size() & ~size_t{3}) - 12;
    std::vector<std::pair<uint32_t, uint32_t>> extra;
    for (size_t off = 0; off < limit; off += 4)
    {
        const uint32_t w3 = ReadU32(text, off + 12);
        if ((w3 & 0xFC000000U) != 0x14000000U) // B label
        {
            continue;
        }
        const int64_t target_va = static_cast<int64_t>(text_va) + static_cast<int64_t>(off + 12) + DecodeBranch26Offset(w3);
        if (target_va < 0)
        {
            continue;
        }
        const uint32_t target_u32 = static_cast<uint32_t>(target_va);
        const auto target_it = targets.find(target_u32);
        if (target_it == targets.end())
        {
            continue;
        }
        const uint32_t w0 = ReadU32(text, off);
        const uint32_t w1 = ReadU32(text, off + 4);
        const uint32_t w2 = ReadU32(text, off + 8);
        const uint32_t branch_va = text_va + static_cast<uint32_t>(off + 12);

        if (multiframe_vas.contains(target_u32))
        {
            const auto has = [&](auto pred) { return pred(w0) || pred(w1) || pred(w2); };
            const bool has_x5_0 = has([](uint32_t w) { return IsZeroReg(w, 5); });
            const bool has_x6_0 = has([](uint32_t w) { return IsZeroReg(w, 6); });
            const bool has_x7_0 = has([](uint32_t w) { return IsZeroReg(w, 7); });
            const bool has_x7_x5 = has([](uint32_t w) { return w == 0xAA0503E7U; }); // mov x7, x5

            if (has_x7_x5 && has_x5_0 && has_x6_0)
            {
                extra.emplace_back(text_va + static_cast<uint32_t>(off), HleZstd::kSvcDecompressUsingDDict);
                wrapper_branches.insert(branch_va);
                continue;
            }
            if (has_x5_0 && has_x6_0 && has_x7_0)
            {
                extra.emplace_back(text_va + static_cast<uint32_t>(off), HleZstd::kSvcDecompressDCtx);
                wrapper_branches.insert(branch_va);
                continue;
            }
            if (IsZeroReg(w2, 7) && IsBoundaryInsn(w1))
            {
                extra.emplace_back(text_va + static_cast<uint32_t>(off + 8), HleZstd::kSvcDecompressUsingDict);
                wrapper_branches.insert(branch_va);
                continue;
            }
        }
        // 2-instruction ZSTD_decompressDCtx wrapper over usingDDict: mov x5, 0; b usingDDict
        if (target_it->second == HleZstd::kSvcDecompressUsingDDict && IsZeroReg(w2, 5) && IsBoundaryInsn(w1))
        {
            extra.emplace_back(text_va + static_cast<uint32_t>(off + 8), HleZstd::kSvcDecompressDCtx);
            wrapper_branches.insert(branch_va);
            continue;
        }
        // 1-instruction forwarding thunk (e.g. udf; b target; udf)
        if (IsBoundaryInsn(w2))
        {
            if (branch_va != target_u32)
            {
                extra.emplace_back(branch_va, target_it->second);
                wrapper_branches.insert(branch_va);
            }
        }
    }
    for (const auto & [va, svc] : extra)
    {
        targets.emplace(va, svc);
    }
}

void FindDynSymTargets(std::span<const uint8_t> rodata,
                       const DynSymInfo & dynsym,
                       uint32_t text_va,
                       uint32_t text_size,
                       std::unordered_map<uint32_t, uint32_t> & targets)
{
    if (dynsym.dynstr_size == 0 || dynsym.dynsym_size < 24)
    {
        return;
    }
    if (static_cast<uint64_t>(dynsym.dynstr_offset) + dynsym.dynstr_size > rodata.size() ||
        static_cast<uint64_t>(dynsym.dynsym_offset) + dynsym.dynsym_size > rodata.size())
    {
        return;
    }

    const std::string_view dynstr(
        reinterpret_cast<const char *>(rodata.data() + dynsym.dynstr_offset),
        dynsym.dynstr_size);
    if (dynstr.find("ZSTD_decompress") == std::string_view::npos)
    {
        return;
    }

    // In libzstd 1.4+, ZSTD_decompressDCtx honors any dictionary attached to the
    // guest DCtx via ZSTD_DCtx_refDDict / ZSTD_DCtx_loadDictionary / ZSTD_DCtx_refPrefix.
    // Similarly, ZSTD_DCtx_setParameter / ZSTD_DCtx_setFormat / ZSTD_DCtx_setMaxWindowSize
    // can configure non-default DCtx behavior (such as magicless frames).
    const bool has_dctx_dict_api =
        dynstr.find("ZSTD_DCtx_refDDict") != std::string_view::npos ||
        dynstr.find("ZSTD_DCtx_loadDictionary") != std::string_view::npos ||
        dynstr.find("ZSTD_DCtx_refPrefix") != std::string_view::npos;
    const bool has_dctx_param_api =
        dynstr.find("ZSTD_DCtx_setParameter") != std::string_view::npos ||
        dynstr.find("ZSTD_DCtx_setFormat") != std::string_view::npos ||
        dynstr.find("ZSTD_DCtx_setMaxWindowSize") != std::string_view::npos;

    constexpr size_t kElf64SymSize = 24;
    constexpr uint8_t kSttFunc = 2;
    for (size_t off = dynsym.dynsym_offset; off + kElf64SymSize <= dynsym.dynsym_offset + dynsym.dynsym_size; off += kElf64SymSize)
    {
        const uint32_t st_name = ReadU32(rodata, off);
        const uint8_t st_info = rodata[off + 4];
        const uint16_t st_shndx = ReadU16(rodata, off + 6);
        uint64_t st_value = 0;
        std::memcpy(&st_value, rodata.data() + off + 8, sizeof(st_value));
        if (st_shndx == 0 || (st_info & 0x0FU) != kSttFunc)
        {
            continue;
        }
        if (st_name >= dynstr.size() || st_value < text_va || st_value >= static_cast<uint64_t>(text_va) + text_size || (st_value & 3) != 0)
        {
            continue;
        }
        const size_t nul = dynstr.find('\0', st_name);
        const std::string_view name = (nul == std::string_view::npos)
                                          ? dynstr.substr(st_name)
                                          : dynstr.substr(st_name, nul - st_name);

        uint32_t svc_imm = 0;
        if (name == "ZSTD_decompress")
        {
            svc_imm = HleZstd::kSvcDecompress;
        }
        else if (!has_dctx_param_api)
        {
            if (name == "ZSTD_decompress_usingDDict")
            {
                svc_imm = HleZstd::kSvcDecompressUsingDDict;
            }
            else if (name == "ZSTD_decompressMultiFrame")
            {
                svc_imm = HleZstd::kSvcDecompressMultiFrame;
            }
            else if (name == "ZSTD_decompressDCtx")
            {
                if (!has_dctx_dict_api)
                {
                    svc_imm = HleZstd::kSvcDecompressDCtx;
                }
            }
            else if (name == "ZSTD_decompress_usingDict")
            {
                svc_imm = HleZstd::kSvcDecompressUsingDict;
            }
        }
        if (svc_imm != 0)
        {
            targets.emplace(static_cast<uint32_t>(st_value), svc_imm);
        }
    }
}
} // namespace

Plan Analyze(const std::array<uint8_t, 0x20> & build_id,
             const std::string & module_name,
             std::span<const uint8_t> text,
             uint32_t text_va,
             std::span<const uint8_t> rodata,
             const DynSymInfo & dynsym)
{
    Plan plan;
    plan.module_name = module_name;
    if (text.size() < 16)
    {
        return plan;
    }

    std::string build_id_raw = Common::HexToString(build_id, true);
    const size_t last_nonzero = build_id_raw.find_last_not_of('0');
    plan.build_id_hex = (last_nonzero == std::string::npos) ? std::string{} : build_id_raw.substr(0, last_nonzero + 1);

    // Map of target function VA -> HleZstd::kSvc*
    std::unordered_map<uint32_t, uint32_t> targets;

    // 1. Dynamic symbol table (.dynsym)
    FindDynSymTargets(rodata, dynsym, text_va, static_cast<uint32_t>(text.size()), targets);

    // 2. Structural ARM64 detection for statically-linked libzstd
    std::vector<uint32_t> fd2f_hits;
    std::vector<uint32_t> skip_hits;
    FindMagicSites(text, text_va, fd2f_hits, skip_hits);

    std::unordered_set<uint32_t> wrapper_branches;
    if (!fd2f_hits.empty() && !skip_hits.empty())
    {
        std::unordered_set<uint32_t> seen_funcs;
        std::unordered_set<uint32_t> multiframe_vas;
        for (const uint32_t hit_va : fd2f_hits)
        {
            uint32_t fva = 0;
            if (!FindEnclosingFunctionStart(text, text_va, hit_va, fva) || !seen_funcs.insert(fva).second)
            {
                continue;
            }
            const uint32_t svc_imm = ClassifyZstdFunction(text, text_va, fva, fd2f_hits, skip_hits);
            if (svc_imm != 0)
            {
                targets.emplace(fva, svc_imm);
                if (svc_imm == HleZstd::kSvcDecompressMultiFrame)
                {
                    multiframe_vas.insert(fva);
                }
            }
        }
        FindTailCallWrappers(text, text_va, multiframe_vas, targets, wrapper_branches);
    }

    if (targets.empty())
    {
        return plan;
    }

    // 3. Collect all BL and external tail-call B sites in .text targeting discovered functions
    const size_t limit = text.size() & ~size_t{3};
    for (size_t off = 0; off < limit; off += 4)
    {
        const uint32_t w = ReadU32(text, off);
        const uint32_t op = w & 0xFC000000U;
        if (op != 0x94000000U && op != 0x14000000U) // BL or B
        {
            continue;
        }
        const bool is_tail_call = (op == 0x14000000U);
        const int64_t target_va = static_cast<int64_t>(text_va) + static_cast<int64_t>(off) + DecodeBranch26Offset(w);
        if (target_va < 0)
        {
            continue;
        }
        const auto it = targets.find(static_cast<uint32_t>(target_va));
        if (it == targets.end())
        {
            continue;
        }
        const uint32_t call_site_va = text_va + static_cast<uint32_t>(off);
        if (is_tail_call)
        {
            // Skip wrapper/thunk internal tail-branches and any intra-function self-loops.
            if (wrapper_branches.contains(call_site_va) ||
                (call_site_va >= it->first && call_site_va < it->first + 0x3000))
            {
                continue;
            }
        }
        else if (IsStaticDCtxProbeCaller(text, off))
        {
            continue;
        }
        PatchSite site{
            .call_site_offset = call_site_va,
            .target_offset = it->first,
            .expected_insn = w,
            .svc_imm = it->second,
            .is_tail_call = is_tail_call,
        };
        if (it->first >= text_va)
        {
            const size_t tgt_off = it->first - text_va;
            for (uint32_t k = 0; k < 4 && tgt_off + (k + 1) * 4 <= limit; ++k)
            {
                site.expected_target_insns[k] = ReadU32(text, tgt_off + k * 4);
                site.expected_target_insn_count = k + 1;
            }
        }
        plan.sites.push_back(site);
    }

    return plan;
}

uint32_t Apply(const Plan & plan,
               std::span<uint8_t> module_image,
               uint32_t cave_offset)
{
    uint32_t patched = 0;
    uint32_t cursor = cave_offset;

    for (const auto & site : plan.sites)
    {
        if (static_cast<size_t>(site.call_site_offset) + 4 > module_image.size() ||
            static_cast<size_t>(cursor) + kTrampolineBytes > module_image.size())
        {
            continue;
        }
        // Leave untouched if an IPS or .nxpatch mod altered this call instruction
        // or the entry instructions of the target Zstd function.
        if (ReadU32(module_image, site.call_site_offset) != site.expected_insn)
        {
            continue;
        }
        bool target_intact = true;
        for (uint32_t k = 0; k < site.expected_target_insn_count; ++k)
        {
            const size_t check_off = static_cast<size_t>(site.target_offset) + k * 4;
            if (check_off + 4 > module_image.size() ||
                ReadU32(module_image, check_off) != site.expected_target_insns[k])
            {
                target_intact = false;
                break;
            }
        }
        if (!target_intact)
        {
            continue;
        }

        uint32_t branch_to_cave = 0;
        uint32_t b_to_guest = 0;
        if (!EncodeBranch26(site.call_site_offset, cursor, !site.is_tail_call, branch_to_cave) ||
            !EncodeBranch26(cursor + 24, site.target_offset, false, b_to_guest))
        {
            continue;
        }

        // 7-instruction AAPCS64 fallback trampoline:
        //   mov  x16, x0           ; preserve x0 in scratch IP0
        //   svc  #imm              ; host libzstd; returns decoded_size or -1 on fallback
        //   cmn  x0, #1            ; compare x0 against ~0ULL
        //   b.eq +8                ; on fallback, restore x0 and branch to guest decoder
        //   ret
        //   mov  x0, x16           ; restore x0 (x1..x7 untouched by host SVC)
        //   b    guest_fn          ; tail-call original guest decoder
        const uint32_t svc_insn = 0xD4000001U | ((site.svc_imm & 0xFFFFU) << 5);
        const std::array<uint32_t, kTrampolineInstructions> stub = {
            0xAA0003F0U, // mov x16, x0
            svc_insn,    // svc #imm
            0xB100041FU, // cmn x0, #1
            0x54000040U, // b.eq +8
            0xD65F03C0U, // ret
            0xAA1003E0U, // mov x0, x16
            b_to_guest,  // b guest_fn
        };

        for (uint32_t i = 0; i < kTrampolineInstructions; ++i)
        {
            WriteU32(module_image, cursor + i * 4, stub[i]);
        }
        WriteU32(module_image, site.call_site_offset, branch_to_cave);
        cursor += kTrampolineBytes;
        ++patched;
    }

    if (patched > 0)
    {
        LOG_INFO(Loader, "HLE Zstd: patched {} call site(s) in {} (build_id={}) via cave at 0x{:X}",
                 patched, plan.module_name, plan.build_id_hex, cave_offset);
    }
    return patched;
}

} // namespace Loader::HleZstdPatcher
