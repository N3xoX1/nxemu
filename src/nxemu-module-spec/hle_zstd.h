#pragma once

#include <cstdint>

namespace HleZstd
{
// Synthetic SVC immediates reserved for host-accelerated Zstd decompression
// trampolines injected by nxemu-loader when running under JIT (non-NCE).
// Horizon OS uses SVC IDs in [0x00..0x7F].
constexpr uint32_t kSvcDecompressUsingDDict = 0x4E5A; // (dctx, dst, dstCap, src, srcSize, ddict)
constexpr uint32_t kSvcDecompressMultiFrame = 0x4E5B; // (dctx, dst, dstCap, src, srcSize, dict, dictSize, ddict)
constexpr uint32_t kSvcDecompressDCtx = 0x4E5C;       // (dctx, dst, dstCap, src, srcSize)
constexpr uint32_t kSvcDecompressUsingDict = 0x4E5D;  // (dctx, dst, dstCap, src, srcSize, dict, dictSize)
constexpr uint32_t kSvcDecompress = 0x4E5E;           // (dst, dstCap, src, srcSize)

// Sentinel returned in x0 when the host handler declines to handle a call
// (e.g. invalid parameters, overlap, or libzstd error) so the guest trampoline
// transparently falls back to the original guest function.
constexpr uint64_t kFallbackReturn = ~uint64_t{0};

constexpr bool IsZstdSvc(uint32_t imm)
{
    return imm >= kSvcDecompressUsingDDict && imm <= kSvcDecompress;
}
} // namespace HleZstd
