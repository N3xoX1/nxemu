#include "core/hle/kernel/hle_zstd.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/memory.h"
#include <nxemu-module-spec/cpu.h>
#include <nxemu-module-spec/hle_zstd.h>
#include <yuzu_common/logging/log.h>
#include <yuzu_common/make_unique_for_overwrite.h>
#include <yuzu_common/scope_exit.h>

namespace Kernel::HleZstd
{
namespace
{
constexpr uint64_t kMaxBufferSize = 256ULL * 1024 * 1024;
constexpr uint64_t kMaxDictSize = 64ULL * 1024 * 1024;
constexpr size_t kRetainedScratchBytes = 1ULL * 1024 * 1024;
constexpr size_t kMaxCachedDDicts = 16;
constexpr uint32_t kDisableAfterConsecutiveFailures = 8;
constexpr size_t kMaxTrackedCallers = 32;

struct DCtxDeleter
{
    void operator()(ZSTD_DCtx * p) const noexcept
    {
        if (p != nullptr)
        {
            ZSTD_freeDCtx(p);
        }
    }
};

struct DDictDeleter
{
    void operator()(ZSTD_DDict * p) const noexcept
    {
        if (p != nullptr)
        {
            ZSTD_freeDDict(p);
        }
    }
};

// Uninitialized scratch buffer (avoids std::vector zero-fill) that shrinks
// back down after oversized allocations so multi-core / Android memory stays bounded.
struct UninitBuffer
{
    std::unique_ptr<uint8_t[]> data;
    size_t capacity{};

    uint8_t * Ensure(size_t size)
    {
        if (size == 0)
        {
            return nullptr;
        }
        if (size > capacity)
        {
            data = Common::make_unique_for_overwrite<uint8_t[]>(size);
            capacity = size;
        }
        return data.get();
    }

    void Trim() noexcept
    {
        if (capacity > kRetainedScratchBytes)
        {
            data.reset();
            capacity = 0;
        }
    }
};

struct CachedDDict
{
    uint64_t dict_content{};
    std::vector<uint8_t> raw_bytes;
    std::shared_ptr<ZSTD_DDict> ddict;
};

struct CallerState
{
    uint64_t trampoline_pc{};
    uint32_t consecutive_failures{};
    bool ever_succeeded{};
    bool disabled{};
};

struct Stats
{
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> zero_copy_hits{0};
    std::atomic<uint64_t> in_place_hits{0};
    std::atomic<uint64_t> bytes_in{0};
    std::atomic<uint64_t> bytes_out{0};
    std::atomic<uint64_t> fb_invalid_dctx{0};
    std::atomic<uint64_t> fb_invalid_range{0};
    std::atomic<uint64_t> fb_overlap{0};
    std::atomic<uint64_t> fb_ddict{0};
    std::atomic<uint64_t> fb_zstd_err{0};
    std::atomic<uint64_t> fb_write_fail{0};
    std::atomic<uint64_t> fb_caller_disabled{0};

    void Reset() noexcept
    {
        hits.store(0, std::memory_order_relaxed);
        zero_copy_hits.store(0, std::memory_order_relaxed);
        in_place_hits.store(0, std::memory_order_relaxed);
        bytes_in.store(0, std::memory_order_relaxed);
        bytes_out.store(0, std::memory_order_relaxed);
        fb_invalid_dctx.store(0, std::memory_order_relaxed);
        fb_invalid_range.store(0, std::memory_order_relaxed);
        fb_overlap.store(0, std::memory_order_relaxed);
        fb_ddict.store(0, std::memory_order_relaxed);
        fb_zstd_err.store(0, std::memory_order_relaxed);
        fb_write_fail.store(0, std::memory_order_relaxed);
        fb_caller_disabled.store(0, std::memory_order_relaxed);
    }
};

std::mutex g_ddict_mutex;
std::vector<CachedDDict> g_ddict_cache;

std::mutex g_caller_mutex;
std::array<CallerState, kMaxTrackedCallers> g_callers{};
std::atomic<uint32_t> g_disabled_caller_count{0};

Stats g_stats;

struct ThreadScratch
{
    std::unique_ptr<ZSTD_DCtx, DCtxDeleter> dctx;
    UninitBuffer src;
    UninitBuffer dst;
    UninitBuffer dict;

    ZSTD_DCtx * EnsureDCtx()
    {
        if (!dctx)
        {
            dctx.reset(ZSTD_createDCtx());
        }
        return dctx.get();
    }

    void TrimAll() noexcept
    {
        src.Trim();
        dst.Trim();
        dict.Trim();
    }
};

thread_local ThreadScratch t_scratch;

uint64_t GetCurrentTrampolinePc(Core::System & system)
{
    auto & kernel = system.Kernel();
    auto & process = GetCurrentProcess(kernel);
    ICpuCore * core = process.GetCpuCore(kernel.CurrentPhysicalCoreIndex());
    if (core == nullptr)
    {
        return 0;
    }
    CpuThreadContext ctx{};
    core->GetContext(ctx);
    return ctx.pc;
}

bool IsCallerDisabled(Core::System & system)
{
    if (g_disabled_caller_count.load(std::memory_order_relaxed) == 0)
    {
        return false;
    }
    const uint64_t pc = GetCurrentTrampolinePc(system);
    if (pc == 0)
    {
        return false;
    }
    std::scoped_lock lock{g_caller_mutex};
    for (const auto & entry : g_callers)
    {
        if (entry.trampoline_pc == pc)
        {
            return entry.disabled;
        }
    }
    return false;
}

void RecordCallerFailure(Core::System & system)
{
    const uint64_t pc = GetCurrentTrampolinePc(system);
    if (pc == 0)
    {
        return;
    }
    std::scoped_lock lock{g_caller_mutex};
    CallerState * free_slot = nullptr;
    for (auto & entry : g_callers)
    {
        if (entry.trampoline_pc == pc)
        {
            if (!entry.ever_succeeded && !entry.disabled)
            {
                if (++entry.consecutive_failures >= kDisableAfterConsecutiveFailures)
                {
                    entry.disabled = true;
                    g_disabled_caller_count.fetch_add(1, std::memory_order_relaxed);
                    LOG_INFO(Kernel_SVC, "HLE Zstd: auto-disabled failing trampoline at 0x{:X} after {} consecutive fallbacks",
                             pc, entry.consecutive_failures);
                }
            }
            return;
        }
        if (free_slot == nullptr && entry.trampoline_pc == 0)
        {
            free_slot = &entry;
        }
    }
    if (free_slot != nullptr)
    {
        *free_slot = CallerState{
            .trampoline_pc = pc,
            .consecutive_failures = 1,
            .ever_succeeded = false,
            .disabled = false,
        };
    }
}

void RecordCallerSuccess(Core::System & system)
{
    // Only touch g_caller_mutex if we've ever recorded failures.
    if (g_stats.fb_zstd_err.load(std::memory_order_relaxed) == 0 &&
        g_stats.fb_ddict.load(std::memory_order_relaxed) == 0 &&
        g_stats.fb_overlap.load(std::memory_order_relaxed) == 0 &&
        g_stats.fb_invalid_dctx.load(std::memory_order_relaxed) == 0)
    {
        return;
    }
    const uint64_t pc = GetCurrentTrampolinePc(system);
    if (pc == 0)
    {
        return;
    }
    std::scoped_lock lock{g_caller_mutex};
    for (auto & entry : g_callers)
    {
        if (entry.trampoline_pc == pc)
        {
            entry.ever_succeeded = true;
            entry.consecutive_failures = 0;
            return;
        }
    }
}

bool RangeValid(const Core::Memory::Memory & mem, uint64_t addr, uint64_t size, uint64_t max_size)
{
    if (addr == 0 || size == 0 || size > max_size)
    {
        return false;
    }
    if (addr + size < addr)
    {
        return false;
    }
    return mem.IsValidVirtualAddressRange(addr, size);
}

bool GuestDCtxValid(const Core::Memory::Memory & mem, uint64_t dctx_va)
{
    // A valid guest ZSTD_DCtx* must be non-null, 8-byte aligned, and mapped.
    if (dctx_va == 0 || (dctx_va & 7ULL) != 0)
    {
        return false;
    }
    return mem.IsValidVirtualAddressRange(dctx_va, 64);
}

std::shared_ptr<ZSTD_DDict> ResolveDDict(Core::Memory::Memory & mem, uint64_t guest_ddict_va, UninitBuffer & scratch_dict)
{
    // Across libzstd 1.0..1.5+, struct ZSTD_DDict_s begins with:
    //   +0x00 void*       dictBuffer
    //   +0x08 const void* dictContent
    //   +0x10 size_t      dictSize
    if (!RangeValid(mem, guest_ddict_va, 24, 24))
    {
        return nullptr;
    }
    uint64_t header[2] = {};
    if (!mem.ReadBlock(guest_ddict_va + 8, header, sizeof(header)))
    {
        return nullptr;
    }
    const uint64_t dict_content = header[0];
    const uint64_t dict_size_u64 = header[1];
    if (!RangeValid(mem, dict_content, dict_size_u64, kMaxDictSize))
    {
        return nullptr;
    }
    const size_t dict_size = static_cast<size_t>(dict_size_u64);

    // Obtain the complete guest dictionary bytes (zero-copy when contiguous plain RAM)
    // so cache hits verify 100% of the dictionary contents even when a guest reuses
    // the same address and size for a different dictionary.
    const uint8_t * dict_ptr = mem.TryGetPlainContiguousSpan(dict_content, dict_size);
    if (dict_ptr == nullptr)
    {
        uint8_t * tmp = scratch_dict.Ensure(dict_size);
        if (tmp == nullptr || !mem.ReadBlock(dict_content, tmp, dict_size))
        {
            return nullptr;
        }
        dict_ptr = tmp;
    }

    {
        std::scoped_lock lock{g_ddict_mutex};
        for (const auto & entry : g_ddict_cache)
        {
            if (entry.dict_content == dict_content &&
                entry.raw_bytes.size() == dict_size &&
                std::memcmp(entry.raw_bytes.data(), dict_ptr, dict_size) == 0)
            {
                return entry.ddict;
            }
        }
    }

    // Cache miss: compile a host ZSTD_DDict from the full dictionary bytes.
    std::shared_ptr<ZSTD_DDict> created(ZSTD_createDDict(dict_ptr, dict_size), DDictDeleter{});
    if (!created)
    {
        return nullptr;
    }

    std::vector<uint8_t> saved_bytes(dict_ptr, dict_ptr + dict_size);
    {
        std::scoped_lock lock{g_ddict_mutex};
        for (auto & entry : g_ddict_cache)
        {
            if (entry.dict_content == dict_content &&
                entry.raw_bytes.size() == dict_size &&
                std::memcmp(entry.raw_bytes.data(), saved_bytes.data(), dict_size) == 0)
            {
                return entry.ddict;
            }
        }
        if (g_ddict_cache.size() >= kMaxCachedDDicts)
        {
            g_ddict_cache.erase(g_ddict_cache.begin());
        }
        g_ddict_cache.push_back(CachedDDict{
            .dict_content = dict_content,
            .raw_bytes = std::move(saved_bytes),
            .ddict = created,
        });
    }
    return created;
}

uint64_t DecompressImpl(Core::System & system,
                        Core::Memory::Memory & mem,
                        uint64_t guest_dctx_va,
                        bool has_guest_dctx,
                        uint64_t dst_va,
                        uint64_t dst_cap_u64,
                        uint64_t src_va,
                        uint64_t src_size_u64,
                        uint64_t dict_va,
                        uint64_t dict_size_u64,
                        uint64_t ddict_va)
{
    SCOPE_EXIT { t_scratch.TrimAll(); };

    if (IsCallerDisabled(system))
    {
        g_stats.fb_caller_disabled.fetch_add(1, std::memory_order_relaxed);
        return ::HleZstd::kFallbackReturn;
    }

    if (has_guest_dctx && !GuestDCtxValid(mem, guest_dctx_va))
    {
        g_stats.fb_invalid_dctx.fetch_add(1, std::memory_order_relaxed);
        RecordCallerFailure(system);
        return ::HleZstd::kFallbackReturn;
    }

    if (dst_va == 0 || dst_cap_u64 == 0 || (dst_va + dst_cap_u64 < dst_va) ||
        !RangeValid(mem, src_va, src_size_u64, kMaxBufferSize))
    {
        g_stats.fb_invalid_range.fetch_add(1, std::memory_order_relaxed);
        return ::HleZstd::kFallbackReturn;
    }
    // Mutually exclusive in libzstd; let guest return ERROR(GENERIC).
    if (ddict_va != 0 && dict_va != 0)
    {
        g_stats.fb_ddict.fetch_add(1, std::memory_order_relaxed);
        return ::HleZstd::kFallbackReturn;
    }

    ZSTD_DCtx * dctx = t_scratch.EnsureDCtx();
    if (dctx == nullptr)
    {
        return ::HleZstd::kFallbackReturn;
    }

    const size_t src_size = static_cast<size_t>(src_size_u64);
    const bool overlapping = (src_va < dst_va + dst_cap_u64 && dst_va < src_va + src_size_u64);

    // Zero-copy source when contiguous plain guest RAM and non-overlapping.
    const uint8_t * src_ptr = overlapping ? nullptr : mem.TryGetPlainContiguousSpan(src_va, src_size);
    const bool src_zero_copy = (src_ptr != nullptr);
    if (!src_zero_copy)
    {
        uint8_t * tmp = t_scratch.src.Ensure(src_size);
        if (tmp == nullptr || !mem.ReadBlock(src_va, tmp, src_size))
        {
            g_stats.fb_invalid_range.fetch_add(1, std::memory_order_relaxed);
            return ::HleZstd::kFallbackReturn;
        }
        src_ptr = tmp;
    }

    // Clamp dstCap to the upper bound reported by the frame header(s) when known.
    const unsigned long long bound = ZSTD_decompressBound(src_ptr, src_size);
    uint64_t effective_dst_cap_u64 = dst_cap_u64;
    if (bound != ZSTD_CONTENTSIZE_ERROR)
    {
        if (bound > dst_cap_u64)
        {
            g_stats.fb_zstd_err.fetch_add(1, std::memory_order_relaxed);
            return ::HleZstd::kFallbackReturn;
        }
        effective_dst_cap_u64 = bound;
    }

    if (effective_dst_cap_u64 > kMaxBufferSize ||
        (effective_dst_cap_u64 > 0 && !mem.IsValidVirtualAddressRange(dst_va, effective_dst_cap_u64)))
    {
        g_stats.fb_invalid_range.fetch_add(1, std::memory_order_relaxed);
        return ::HleZstd::kFallbackReturn;
    }

    // For overlapping in-place buffers, require the documented libzstd in-place layout
    // and margin so guest error behavior on insufficient margin is preserved.
    if (overlapping)
    {
        const size_t margin = ZSTD_decompressionMargin(src_ptr, src_size);
        const bool margin_ok = !ZSTD_isError(margin) &&
                               bound != ZSTD_CONTENTSIZE_ERROR &&
                               dst_va <= src_va &&
                               (src_va + src_size_u64) == (dst_va + dst_cap_u64) &&
                               margin <= dst_cap_u64 &&
                               bound <= dst_cap_u64 - margin;
        if (!margin_ok)
        {
            g_stats.fb_overlap.fetch_add(1, std::memory_order_relaxed);
            return ::HleZstd::kFallbackReturn;
        }
    }

    const size_t dst_cap = static_cast<size_t>(effective_dst_cap_u64);

    // Zero-copy destination when contiguous plain guest RAM (no RasterizerCachedMemory)
    // and non-overlapping. On overlap or GPU-cached pages, decode into scratch and write
    // back via WriteBlock (which triggers HandleRasterizerWrite) only on success.
    uint8_t dummy_byte = 0;
    uint8_t * dst_ptr = nullptr;
    bool dst_zero_copy = false;
    if (dst_cap == 0)
    {
        dst_ptr = &dummy_byte;
        dst_zero_copy = true;
    }
    else if (!overlapping)
    {
        dst_ptr = mem.TryGetPlainContiguousSpan(dst_va, dst_cap);
        dst_zero_copy = (dst_ptr != nullptr);
    }
    if (dst_ptr == nullptr)
    {
        dst_ptr = t_scratch.dst.Ensure(dst_cap);
        if (dst_ptr == nullptr)
        {
            return ::HleZstd::kFallbackReturn;
        }
    }

    size_t result = 0;
    if (ddict_va != 0)
    {
        const auto ddict = ResolveDDict(mem, ddict_va, t_scratch.dict);
        if (!ddict)
        {
            g_stats.fb_ddict.fetch_add(1, std::memory_order_relaxed);
            RecordCallerFailure(system);
            return ::HleZstd::kFallbackReturn;
        }
        result = ZSTD_decompress_usingDDict(dctx, dst_ptr, dst_cap, src_ptr, src_size, ddict.get());
    }
    else if (dict_va != 0 && dict_size_u64 != 0)
    {
        if (!RangeValid(mem, dict_va, dict_size_u64, kMaxDictSize))
        {
            g_stats.fb_ddict.fetch_add(1, std::memory_order_relaxed);
            return ::HleZstd::kFallbackReturn;
        }
        const size_t dict_size = static_cast<size_t>(dict_size_u64);
        const uint8_t * dict_ptr = mem.TryGetPlainContiguousSpan(dict_va, dict_size);
        if (dict_ptr == nullptr)
        {
            uint8_t * tmp = t_scratch.dict.Ensure(dict_size);
            if (tmp == nullptr || !mem.ReadBlock(dict_va, tmp, dict_size))
            {
                g_stats.fb_ddict.fetch_add(1, std::memory_order_relaxed);
                return ::HleZstd::kFallbackReturn;
            }
            dict_ptr = tmp;
        }
        result = ZSTD_decompress_usingDict(dctx, dst_ptr, dst_cap, src_ptr, src_size, dict_ptr, dict_size);
    }
    else
    {
        result = ZSTD_decompressDCtx(dctx, dst_ptr, dst_cap, src_ptr, src_size);
    }

    if (ZSTD_isError(result) != 0 || result > dst_cap)
    {
        g_stats.fb_zstd_err.fetch_add(1, std::memory_order_relaxed);
        RecordCallerFailure(system);
        return ::HleZstd::kFallbackReturn;
    }
    if (!dst_zero_copy && result > 0 && !mem.WriteBlock(dst_va, dst_ptr, static_cast<uint64_t>(result)))
    {
        g_stats.fb_write_fail.fetch_add(1, std::memory_order_relaxed);
        return ::HleZstd::kFallbackReturn;
    }

    g_stats.hits.fetch_add(1, std::memory_order_relaxed);
    if (src_zero_copy && dst_zero_copy)
    {
        g_stats.zero_copy_hits.fetch_add(1, std::memory_order_relaxed);
    }
    if (overlapping)
    {
        g_stats.in_place_hits.fetch_add(1, std::memory_order_relaxed);
    }
    g_stats.bytes_in.fetch_add(src_size, std::memory_order_relaxed);
    g_stats.bytes_out.fetch_add(result, std::memory_order_relaxed);
    RecordCallerSuccess(system);
    return static_cast<uint64_t>(result);
}
} // namespace

bool HandleSvc(Core::System & system, uint32_t imm, std::span<uint64_t, 8> args)
{
    if (!::HleZstd::IsZstdSvc(imm))
    {
        return false;
    }

    auto & mem = GetCurrentMemory(system.Kernel());
    uint64_t ret = ::HleZstd::kFallbackReturn;

    switch (imm)
    {
    case ::HleZstd::kSvcDecompressUsingDDict:
        // (x0=dctx, x1=dst, x2=dstCap, x3=src, x4=srcSize, x5=ddict)
        ret = DecompressImpl(system, mem, args[0], true, args[1], args[2], args[3], args[4], 0, 0, args[5]);
        break;
    case ::HleZstd::kSvcDecompressMultiFrame:
        // (x0=dctx, x1=dst, x2=dstCap, x3=src, x4=srcSize, x5=dict, x6=dictSize, x7=ddict)
        ret = DecompressImpl(system, mem, args[0], true, args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
        break;
    case ::HleZstd::kSvcDecompressDCtx:
        // (x0=dctx, x1=dst, x2=dstCap, x3=src, x4=srcSize)
        ret = DecompressImpl(system, mem, args[0], true, args[1], args[2], args[3], args[4], 0, 0, 0);
        break;
    case ::HleZstd::kSvcDecompressUsingDict:
        // (x0=dctx, x1=dst, x2=dstCap, x3=src, x4=srcSize, x5=dict, x6=dictSize)
        ret = DecompressImpl(system, mem, args[0], true, args[1], args[2], args[3], args[4], args[5], args[6], 0);
        break;
    case ::HleZstd::kSvcDecompress:
        // (x0=dst, x1=dstCap, x2=src, x3=srcSize)
        ret = DecompressImpl(system, mem, 0, false, args[0], args[1], args[2], args[3], 0, 0, 0);
        break;
    default:
        break;
    }

    args[0] = ret;
    return true;
}

void Reset()
{
    const uint64_t hits = g_stats.hits.load(std::memory_order_relaxed);
    const uint64_t fb_total =
        g_stats.fb_invalid_dctx.load(std::memory_order_relaxed) +
        g_stats.fb_invalid_range.load(std::memory_order_relaxed) +
        g_stats.fb_overlap.load(std::memory_order_relaxed) +
        g_stats.fb_ddict.load(std::memory_order_relaxed) +
        g_stats.fb_zstd_err.load(std::memory_order_relaxed) +
        g_stats.fb_write_fail.load(std::memory_order_relaxed) +
        g_stats.fb_caller_disabled.load(std::memory_order_relaxed);
    if (hits > 0 || fb_total > 0)
    {
        LOG_INFO(Kernel_SVC,
                 "HLE Zstd stats: hits={} (zero_copy={}, in_place={}), in={:.1f} MiB, out={:.1f} MiB, "
                 "fallbacks={} (dctx={}, range={}, overlap={}, ddict={}, zstd_err={}, write={}, disabled={})",
                 hits,
                 g_stats.zero_copy_hits.load(std::memory_order_relaxed),
                 g_stats.in_place_hits.load(std::memory_order_relaxed),
                 static_cast<double>(g_stats.bytes_in.load(std::memory_order_relaxed)) / (1024.0 * 1024.0),
                 static_cast<double>(g_stats.bytes_out.load(std::memory_order_relaxed)) / (1024.0 * 1024.0),
                 fb_total,
                 g_stats.fb_invalid_dctx.load(std::memory_order_relaxed),
                 g_stats.fb_invalid_range.load(std::memory_order_relaxed),
                 g_stats.fb_overlap.load(std::memory_order_relaxed),
                 g_stats.fb_ddict.load(std::memory_order_relaxed),
                 g_stats.fb_zstd_err.load(std::memory_order_relaxed),
                 g_stats.fb_write_fail.load(std::memory_order_relaxed),
                 g_stats.fb_caller_disabled.load(std::memory_order_relaxed));
    }
    g_stats.Reset();
    {
        std::scoped_lock lock{g_ddict_mutex};
        g_ddict_cache.clear();
    }
    {
        std::scoped_lock lock{g_caller_mutex};
        g_callers.fill(CallerState{});
        g_disabled_caller_count.store(0, std::memory_order_relaxed);
    }
}

} // namespace Kernel::HleZstd
