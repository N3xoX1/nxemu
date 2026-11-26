#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Loader::HleZstdPatcher
{
constexpr uint32_t kTrampolineInstructions = 7;
constexpr uint32_t kTrampolineBytes = kTrampolineInstructions * sizeof(uint32_t);

struct DynSymInfo
{
    uint32_t dynstr_offset{};
    uint32_t dynstr_size{};
    uint32_t dynsym_offset{};
    uint32_t dynsym_size{};
};

struct PatchSite
{
    uint32_t call_site_offset{};                   // Module-relative VA of BL or tail-call B instruction
    uint32_t target_offset{};                      // Module-relative VA of guest Zstd function
    uint32_t expected_insn{};                      // Original BL/B instruction word
    std::array<uint32_t, 4> expected_target_insns{}; // First instructions of target function (mod safety check)
    uint32_t expected_target_insn_count{};
    uint32_t svc_imm{};                            // HleZstd::kSvc* immediate
    bool is_tail_call{};                           // True for tail-call B, false for BL
};

struct Plan
{
    std::string module_name;
    std::string build_id_hex;
    std::vector<PatchSite> sites;
};

// Analyze a loaded 64-bit NSO's .text and .rodata to find verified Zstd
// decompression call sites via:
//   1) .dynsym defined STT_FUNC ZSTD_decompress* symbols (if present)
//   2) Structural ARM64 matching of ZSTD_decompressMultiFrame / inlined
//      ZSTD_decompress_usingDDict and their tail-call wrappers
Plan Analyze(const std::array<uint8_t, 0x20> & build_id,
             const std::string & module_name,
             std::span<const uint8_t> text,
             uint32_t text_va,
             std::span<const uint8_t> rodata,
             const DynSymInfo & dynsym);

// Emit 7-instruction ARM64 fallback trampolines into the reserved cave and
// rewrite each verified BL site (skipping any site modified by user mods).
uint32_t Apply(const Plan & plan,
               std::span<uint8_t> module_image,
               uint32_t cave_offset);

} // namespace Loader::HleZstdPatcher
