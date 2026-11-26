#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/loader/hle_zstd_patcher.h"
#include <nxemu-module-spec/hle_zstd.h>

namespace
{
using namespace Loader::HleZstdPatcher;

void Check(bool ok, const char * msg)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::abort();
    }
}

void WriteU32(std::vector<uint8_t> & buf, uint32_t off, uint32_t val)
{
    std::memcpy(buf.data() + off, &val, sizeof(val));
}

uint32_t ReadU32(const std::vector<uint8_t> & buf, uint32_t off)
{
    uint32_t val = 0;
    std::memcpy(&val, buf.data() + off, sizeof(val));
    return val;
}

uint32_t EncodeB(uint32_t pc, uint32_t target, bool link)
{
    const int64_t delta = static_cast<int64_t>(target) - static_cast<int64_t>(pc);
    const uint32_t imm26 = static_cast<uint32_t>((delta >> 2) & 0x03FFFFFF);
    return (link ? 0x94000000U : 0x14000000U) | imm26;
}

uint32_t EncodeCbnz64(uint32_t pc, uint32_t target, uint32_t reg)
{
    const int64_t delta = static_cast<int64_t>(target) - static_cast<int64_t>(pc);
    const uint32_t imm19 = static_cast<uint32_t>((delta >> 2) & 0x7FFFF);
    return 0xB5000000U | (imm19 << 5) | (reg & 0x1FU);
}

void TestStructuralMultiFrameAndWrappers()
{
    std::vector<uint8_t> image(0x3000, 0);
    constexpr uint32_t kTextVa = 0;
    constexpr uint32_t kTextSize = 0x1000;
    constexpr uint32_t kCaveOff = 0x2000;

    // Wrapper: ZSTD_decompress_usingDDict @ 0x100 -> MultiFrame @ 0x110
    WriteU32(image, 0x100, 0xAA0503E7U); // mov x7, x5
    WriteU32(image, 0x104, 0xAA1F03E5U); // mov x5, xzr
    WriteU32(image, 0x108, 0xAA1F03E6U); // mov x6, xzr
    WriteU32(image, 0x10C, EncodeB(0x10C, 0x110, false));

    // ZSTD_decompressMultiFrame @ 0x110
    WriteU32(image, 0x110, 0xA9BA7BFDU); // stp x29, x30, [sp, #-0x60]!
    WriteU32(image, 0x114, 0xAA0003F4U); // mov x20, x0
    WriteU32(image, 0x118, 0xAA0403F9U); // mov x25, x4
    WriteU32(image, 0x11C, EncodeCbnz64(0x11C, 0x150, 7)); // cbnz x7, +ddict_load
    WriteU32(image, 0x120, 0xF100173FU); // cmp x25, #5
    WriteU32(image, 0x124, 0x5296A509U); // mov w9, #0xb528
    WriteU32(image, 0x128, 0x72BFA5E9U); // movk w9, #0xfd2f, lsl #16
    WriteU32(image, 0x12C, 0x529A54AAU); // mov w10, #0xd2a5
    WriteU32(image, 0x130, 0x72A0308AU); // movk w10, #0x0184, lsl #16
    WriteU32(image, 0x134, 0xD65F03C0U); // ret
    // ddict load block @ 0x150: ldp x5, x6, [x7, #8]
    WriteU32(image, 0x150, 0xA94098E5U); // ldp x5, x6, [x7, #8]
    WriteU32(image, 0x154, EncodeB(0x154, 0x120, false));

    // Wrapper: ZSTD_decompressDCtx @ 0x180 -> MultiFrame @ 0x110
    WriteU32(image, 0x180, 0xAA1F03E5U); // mov x5, xzr
    WriteU32(image, 0x184, 0xAA1F03E6U); // mov x6, xzr
    WriteU32(image, 0x188, 0xAA1F03E7U); // mov x7, xzr
    WriteU32(image, 0x18C, EncodeB(0x18C, 0x110, false));

    // Three BL callers: @ 0x400 -> MultiFrame, @ 0x404 -> usingDDict, @ 0x408 -> decompressDCtx
    WriteU32(image, 0x400, EncodeB(0x400, 0x110, true));
    WriteU32(image, 0x404, EncodeB(0x404, 0x100, true));
    WriteU32(image, 0x408, EncodeB(0x408, 0x180, true));

    std::array<uint8_t, 0x20> bid{};
    const Plan plan = Analyze(bid, "main", std::span<const uint8_t>(image.data(), kTextSize), kTextVa, {}, {});
    Check(plan.sites.size() == 3, "expected 3 detected call sites");
    Check(plan.sites[0].call_site_offset == 0x400 && plan.sites[0].svc_imm == HleZstd::kSvcDecompressMultiFrame, "site 0 MultiFrame");
    Check(plan.sites[1].call_site_offset == 0x404 && plan.sites[1].svc_imm == HleZstd::kSvcDecompressUsingDDict, "site 1 usingDDict");
    Check(plan.sites[2].call_site_offset == 0x408 && plan.sites[2].svc_imm == HleZstd::kSvcDecompressDCtx, "site 2 decompressDCtx");

    // Simulate a mod overwriting call site 0x408 before Apply runs.
    WriteU32(image, 0x408, 0xD503201FU); // nop
    const uint32_t patched = Apply(plan, image, kCaveOff);
    Check(patched == 2, "mod-modified call site must be skipped");
    Check(ReadU32(image, 0x408) == 0xD503201FU, "mod instruction preserved");
    Check(ReadU32(image, 0x400) == EncodeB(0x400, kCaveOff, true), "site 0 rewritten to cave");
    Check(ReadU32(image, 0x404) == EncodeB(0x404, kCaveOff + kTrampolineBytes, true), "site 1 rewritten to cave");
    Check(ReadU32(image, kCaveOff + 0) == 0xAA0003F0U, "trampoline mov x16, x0");
    Check(ReadU32(image, kCaveOff + 4) == (0xD4000001U | (HleZstd::kSvcDecompressMultiFrame << 5)), "trampoline svc MultiFrame");
    Check(ReadU32(image, kCaveOff + 24) == EncodeB(kCaveOff + 24, 0x110, false), "trampoline fallback branch");
}

void TestDynSymAndRejection()
{
    std::vector<uint8_t> text(0x400, 0);
    // Fake function with ZSTD_MAGICNUMBER only (no skippable magic, no ddict) must NOT match structurally.
    WriteU32(text, 0x100, 0xA9BA7BFDU);
    WriteU32(text, 0x104, 0x5296A509U);
    WriteU32(text, 0x108, 0x72BFA5E9U);
    WriteU32(text, 0x10C, 0xD65F03C0U);
    // Exported ZSTD_decompress @ 0x200 via .dynsym
    WriteU32(text, 0x200, 0xD65F03C0U);
    WriteU32(text, 0x300, EncodeB(0x300, 0x100, true));
    WriteU32(text, 0x304, EncodeB(0x304, 0x200, true));

    // Build minimal .rodata with dynstr + Elf64_Sym entries:
    //  1) Valid defined STT_FUNC ZSTD_decompress @ 0x200
    //  2) Imported undefined SHN_UNDEF ZSTD_decompressDCtx @ 0x0 (must be rejected)
    //  3) Non-function STT_OBJECT ZSTD_decompress_usingDDict @ 0x210 (must be rejected)
    //  4) Defined STT_FUNC ZSTD_decompressDCtx @ 0x220 paired with ZSTD_DCtx_refDDict in dynstr (must be skipped)
    std::vector<uint8_t> rodata(0x200, 0);
    auto write_str = [&](uint32_t off, const char * s) {
        std::memcpy(rodata.data() + off, s, std::strlen(s) + 1);
    };
    write_str(0x10, "ZSTD_decompress");           // rel 0x00
    write_str(0x24, "ZSTD_decompressDCtx");       // rel 0x14
    write_str(0x3C, "ZSTD_decompress_usingDDict");// rel 0x2C
    write_str(0x5C, "ZSTD_DCtx_refDDict");        // rel 0x4C

    auto write_sym = [&](size_t off, uint32_t st_name, uint8_t st_info, uint16_t st_shndx, uint64_t st_value) {
        std::memcpy(rodata.data() + off + 0, &st_name, sizeof(st_name));
        rodata[off + 4] = st_info;
        std::memcpy(rodata.data() + off + 6, &st_shndx, sizeof(st_shndx));
        std::memcpy(rodata.data() + off + 8, &st_value, sizeof(st_value));
    };
    write_sym(0xA0 + 0 * 24, 0x00, 0x12, 1, 0x200); // STB_GLOBAL | STT_FUNC, defined -> ACCEPT
    write_sym(0xA0 + 1 * 24, 0x14, 0x12, 0, 0x000); // SHN_UNDEF import -> REJECT
    write_sym(0xA0 + 2 * 24, 0x2C, 0x11, 1, 0x210); // STT_OBJECT -> REJECT
    write_sym(0xA0 + 3 * 24, 0x14, 0x12, 1, 0x220); // ZSTD_decompressDCtx w/ ZSTD_DCtx_refDDict -> SKIP

    // Callers to all 4 candidate addresses
    WriteU32(text, 0x308, EncodeB(0x308, 0x000, true));
    WriteU32(text, 0x30C, EncodeB(0x30C, 0x210, true));
    WriteU32(text, 0x310, EncodeB(0x310, 0x220, true));

    const DynSymInfo dynsym{0x10, 0x70, 0xA0, 4 * 24};
    std::array<uint8_t, 0x20> bid{};
    const Plan plan = Analyze(bid, "subsdk0", text, 0, rodata, dynsym);
    Check(plan.sites.size() == 1, "expected only valid defined STT_FUNC ZSTD_decompress site");
    Check(plan.sites[0].call_site_offset == 0x304 && plan.sites[0].svc_imm == HleZstd::kSvcDecompress, "dynsym ZSTD_decompress");

    // When ZSTD_DCtx_setParameter is present, DCtx-taking functions (e.g. usingDDict) must also
    // be skipped while stateless ZSTD_decompress remains eligible.
    write_str(0x70, "ZSTD_DCtx_setParameter");
    write_sym(0xA0 + 2 * 24, 0x2C, 0x12, 1, 0x210); // make usingDDict a valid STT_FUNC
    const DynSymInfo dynsym_param{0x10, 0x88, 0xA0, 4 * 24};
    const Plan param_plan = Analyze(bid, "subsdk0", text, 0, rodata, dynsym_param);
    Check(param_plan.sites.size() == 1 && param_plan.sites[0].svc_imm == HleZstd::kSvcDecompress,
          "ZSTD_DCtx_setParameter must skip DCtx-taking dynsym targets");
}

void TestClobberedRegisterRejectionAndModTargetCheck()
{
    std::vector<uint8_t> image(0x4000, 0);
    constexpr uint32_t kTextSize = 0x2000;
    constexpr uint32_t kCaveOff = 0x3000;

    // Function @ 0x100 looks like inlined usingDDict EXCEPT x5 is clobbered by `ldr x5, [x0, #0x7160]`
    // before `cbnz x5` + `ldp [x5, #8]` (like an inlined ZSTD_decompressDCtx reading dctx->ddict).
    // Must be REJECTED so caller's unrelated x5 is never misread as ddict.
    WriteU32(image, 0x100, 0xA9BE7BFDU); // stp x29, x30, [sp, #-0x20]!
    WriteU32(image, 0x104, 0xAA0003F4U); // mov x20, x0
    WriteU32(image, 0x108, 0xAA0403FAU); // mov x26, x4
    WriteU32(image, 0x10C, 0xF9438805U); // ldr x5, [x0, #0x710]  <-- clobbers x5!
    WriteU32(image, 0x110, EncodeCbnz64(0x110, 0x140, 5)); // cbnz x5, +ddict_load
    WriteU32(image, 0x114, 0xF100175FU); // cmp x26, #5
    WriteU32(image, 0x118, 0x5296A509U); // mov w9, #0xb528
    WriteU32(image, 0x11C, 0x72BFA5E9U); // movk w9, #0xfd2f, lsl #16
    WriteU32(image, 0x120, 0x52854A0AU); // mov w10, #0x2a50
    WriteU32(image, 0x124, 0x72A309AAU); // movk w10, #0x184d, lsl #16
    WriteU32(image, 0x128, 0xD65F03C0U); // ret
    WriteU32(image, 0x140, 0xA94098A5U); // ldp x5, x6, [x5, #8]
    WriteU32(image, 0x144, EncodeB(0x144, 0x114, false));
    WriteU32(image, 0x500, EncodeB(0x500, 0x100, true));

    std::array<uint8_t, 0x20> bid{};
    const Plan rejected_plan = Analyze(bid, "main", std::span<const uint8_t>(image.data(), kTextSize), 0, {}, {});
    Check(rejected_plan.sites.empty(), "clobbered x5 via ldr must not be classified as usingDDict");

    // Now test mod target-body check: valid MultiFrame @ 0x200, caller @ 0x600.
    WriteU32(image, 0x200, 0xA9BE7BFDU);
    WriteU32(image, 0x204, 0xAA0003F4U);
    WriteU32(image, 0x208, 0xAA0403FAU);
    WriteU32(image, 0x20C, 0xAA0703FBU); // mov x27, x7
    WriteU32(image, 0x210, 0xF100175FU); // cmp x26, #5
    WriteU32(image, 0x214, 0xA9409B65U); // ldp x5, x6, [x27, #8]
    WriteU32(image, 0x218, 0x5296A509U);
    WriteU32(image, 0x21C, 0x72BFA5E9U);
    WriteU32(image, 0x220, 0x52854A0AU);
    WriteU32(image, 0x224, 0x72A309AAU);
    WriteU32(image, 0x228, 0xD65F03C0U);
    WriteU32(image, 0x600, EncodeB(0x600, 0x200, true));

    const Plan plan = Analyze(bid, "main", std::span<const uint8_t>(image.data(), kTextSize), 0, {}, {});
    Check(plan.sites.size() == 1, "expected 1 valid MultiFrame site");

    // Simulate a mod modifying instruction #2 of the target function prologue after Analyze().
    WriteU32(image, 0x204, 0xD503201FU); // nop
    const uint32_t patched = Apply(plan, image, kCaveOff);
    Check(patched == 0, "Apply must skip site when target function prologue was modified by a mod");

    // Static-DCtx 4-byte-prefix probe caller (add x3, x3, #4 before BL; cmp w0, #0x46 after BL)
    // must be excluded generically without any build-id table.
    WriteU32(image, 0x204, 0xAA0003F4U); // restore valid prologue
    WriteU32(image, 0x700, 0x91001063U); // add x3, x3, #4
    WriteU32(image, 0x720, EncodeB(0x720, 0x200, true)); // BL MultiFrame (probe caller)
    WriteU32(image, 0x730, 0x7101181FU); // cmp w0, #0x46 (ZSTD_error_dstSize_tooSmall)
    const Plan probe_plan = Analyze(bid, "main", std::span<const uint8_t>(image.data(), kTextSize), 0, {}, {});
    Check(probe_plan.sites.size() == 1 && probe_plan.sites[0].call_site_offset == 0x600,
          "generic static-DCtx probe caller must be skipped while normal caller is kept");
}
void TestPacStackSpillThunkAndTailCall()
{
    std::vector<uint8_t> image(0x7000, 0);
    constexpr uint32_t kTextSize = 0x5000;
    constexpr uint32_t kCaveOff = 0x6000;

    // pacibsp + stack-spilled x7 MultiFrame @ 0x100
    WriteU32(image, 0x100, 0xD503237FU); // pacibsp
    WriteU32(image, 0x104, 0xA9BA7BFDU); // stp x29, x30, [sp, #-0x60]!
    WriteU32(image, 0x108, 0xAA0003F4U); // mov x20, x0
    WriteU32(image, 0x10C, 0xAA0403FAU); // mov x26, x4
    WriteU32(image, 0x110, 0xF9002FE7U); // str x7, [sp, #0x58]
    WriteU32(image, 0x114, EncodeCbnz64(0x114, 0x150, 7)); // cbnz x7, +spill_reload
    WriteU32(image, 0x118, 0xF100175FU); // cmp x26, #5
    WriteU32(image, 0x11C, 0x5296A489U); // mov w9, #0xb524 (legacy Zstd magic)
    WriteU32(image, 0x120, 0x72BFA5E9U); // movk w9, #0xfd2f, lsl #16
    WriteU32(image, 0x124, 0x529A54AAU); // mov w10, #0xd2a5
    WriteU32(image, 0x128, 0x72A0308AU); // movk w10, #0x0184, lsl #16
    WriteU32(image, 0x12C, 0xD65F0FFFU); // retab
    // Spill reload block @ 0x150: ldr x8, [sp, #0x58]; ldp x5, x6, [x8, #8]
    WriteU32(image, 0x150, 0xF9402FE8U); // ldr x8, [sp, #0x58]
    WriteU32(image, 0x154, 0xA9409905U); // ldp x5, x6, [x8, #8]
    WriteU32(image, 0x158, EncodeB(0x158, 0x118, false));

    // 1-instruction forwarding thunk @ 0x200 (preceded by udf #0)
    WriteU32(image, 0x200, EncodeB(0x200, 0x100, false));

    // Caller 1: BL @ 0x4000 -> thunk @ 0x200
    WriteU32(image, 0x4000, EncodeB(0x4000, 0x200, true));
    // Caller 2: external tail-call B @ 0x4010 -> MultiFrame @ 0x100 (preceded by add sp, sp, #0x40)
    WriteU32(image, 0x400C, 0x910103FFU); // add sp, sp, #0x40
    WriteU32(image, 0x4010, EncodeB(0x4010, 0x100, false));

    std::array<uint8_t, 0x20> bid{};
    const Plan plan = Analyze(bid, "main", std::span<const uint8_t>(image.data(), kTextSize), 0, {}, {});
    Check(plan.sites.size() == 2, "expected BL to thunk + external tail-call B");
    Check(plan.sites[0].call_site_offset == 0x4000 && !plan.sites[0].is_tail_call && plan.sites[0].svc_imm == HleZstd::kSvcDecompressMultiFrame, "thunk BL");
    Check(plan.sites[1].call_site_offset == 0x4010 && plan.sites[1].is_tail_call && plan.sites[1].svc_imm == HleZstd::kSvcDecompressMultiFrame, "tail-call B");

    const uint32_t patched = Apply(plan, image, kCaveOff);
    Check(patched == 2, "both thunk BL and tail-call B patched");
    Check(ReadU32(image, 0x4000) == EncodeB(0x4000, kCaveOff, true), "BL kept link bit");
    Check(ReadU32(image, 0x4010) == EncodeB(0x4010, kCaveOff + kTrampolineBytes, false), "tail-call B kept non-link bit");
}
} // namespace

int main()
{
    TestStructuralMultiFrameAndWrappers();
    TestDynSymAndRejection();
    TestClobberedRegisterRejectionAndModTargetCheck();
    TestPacStackSpillThunkAndTailCall();
    std::printf("hle_zstd_patcher tests passed.\n");
    return 0;
}
