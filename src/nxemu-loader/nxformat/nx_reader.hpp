/* SPDX-License-Identifier: MIT

MIT License

Copyright (c) 2026 fab918

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifndef NX_READER_HPP_INCLUDED
#define NX_READER_HPP_INCLUDED

// Portable, read-only NX 0.2/0.3 reader. The distributed header is generated from
// this file without transformations; all wire values are decoded explicitly.
#include <atomic>
#include <algorithm>
#include <chrono>
#include <list>
#include <iterator>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// CRC-32C acceleration: SSE4.2 on x86-64, the ARMv8 CRC extension on AArch64,
// selected at run time; a portable slicing-by-8 table otherwise.
#if defined(__x86_64__) || defined(_M_X64)
#define NX_CRC32C_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#include <nmmintrin.h>
#include <immintrin.h>
#if defined(__GNUC__) || defined(__clang__)
#define NX_CRC32C_TARGET __attribute__((target("sse4.2")))
#else
#define NX_CRC32C_TARGET
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#define NX_CRC32C_ARM 1
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define NX_CRC32C_TARGET
#else
#include <arm_acle.h>
#if defined(__ARM_FEATURE_SHA2)
#include <arm_neon.h>
#endif
#if defined(__clang__)
#define NX_CRC32C_TARGET __attribute__((target("crc")))
#else
#define NX_CRC32C_TARGET __attribute__((target("+crc")))
#endif
#endif
#if defined(__linux__) && !defined(__ARM_FEATURE_CRC32)
#include <sys/auxv.h>
#endif
#endif

namespace nx {

namespace detail {
// Vector allocator whose resize() leaves new bytes uninitialized. Decoded
// block buffers are fully overwritten by an exact read or decode before any
// byte is used, so zero-filling up to 1 MiB per cache miss is pure cost.
template <class T> struct DefaultInitAllocator {
    using value_type = T;
    DefaultInitAllocator() noexcept = default;
    template <class U> DefaultInitAllocator(const DefaultInitAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) { return std::allocator<T>{}.allocate(n); }
    void deallocate(T* p, std::size_t n) noexcept { std::allocator<T>{}.deallocate(p, n); }
    template <class U> void construct(U* p) noexcept(std::is_nothrow_default_constructible<U>::value) { ::new (static_cast<void*>(p)) U; }
    template <class U, class... A> void construct(U* p, A&&... a) { ::new (static_cast<void*>(p)) U(std::forward<A>(a)...); }
    template <class U> bool operator==(const DefaultInitAllocator<U>&) const noexcept { return true; }
    template <class U> bool operator!=(const DefaultInitAllocator<U>&) const noexcept { return false; }
};
using LogicalBytes = std::vector<std::uint8_t, DefaultInitAllocator<std::uint8_t>>;

using Crc32cFunction = std::uint32_t (*)(std::uint32_t, const std::uint8_t*, std::size_t);

struct Crc32cTables {
    std::uint32_t t[8][256];
    Crc32cTables() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int b = 0; b < 8; ++b) c = (c >> 1) ^ ((c & 1) ? 0x82f63b78u : 0u);
            t[0][i] = c;
        }
        for (int k = 1; k < 8; ++k)
            for (std::uint32_t i = 0; i < 256; ++i) t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xff];
    }
};

inline std::uint32_t crc32c_table(std::uint32_t crc, const std::uint8_t* p, std::size_t n) {
    static const Crc32cTables tables;
    const auto& t = tables.t;
    while (n >= 8) {
        const std::uint32_t lo = crc ^ (std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24));
        const std::uint32_t hi = std::uint32_t(p[4]) | (std::uint32_t(p[5]) << 8) | (std::uint32_t(p[6]) << 16) | (std::uint32_t(p[7]) << 24);
        crc = t[7][lo & 0xff] ^ t[6][(lo >> 8) & 0xff] ^ t[5][(lo >> 16) & 0xff] ^ t[4][lo >> 24] ^
              t[3][hi & 0xff] ^ t[2][(hi >> 8) & 0xff] ^ t[1][(hi >> 16) & 0xff] ^ t[0][hi >> 24];
        p += 8; n -= 8;
    }
    while (n--) crc = t[0][(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return crc;
}

#if defined(NX_CRC32C_X86)
NX_CRC32C_TARGET inline std::uint32_t crc32c_hardware(std::uint32_t crc, const std::uint8_t* p, std::size_t n) {
    std::uint64_t c = crc;
    while (n >= 8) { std::uint64_t v; std::memcpy(&v, p, 8); c = _mm_crc32_u64(c, v); p += 8; n -= 8; }
    auto c32 = static_cast<std::uint32_t>(c);
    while (n--) c32 = _mm_crc32_u8(c32, *p++);
    return c32;
}
// CRC of 8 consecutive slices of `n` bytes (n % 8 == 0), one chain per slice.
NX_CRC32C_TARGET inline void crc32c_hardware_x8(const std::uint8_t* p, std::size_t n, std::uint32_t* out) {
    std::uint64_t c0 = 0xffffffffu, c1 = c0, c2 = c0, c3 = c0, c4 = c0, c5 = c0, c6 = c0, c7 = c0;
    for (std::size_t i = 0; i < n; i += 8) {
        std::uint64_t v[8];
        for (int k = 0; k < 8; ++k) std::memcpy(&v[k], p + k * n + i, 8);
        c0 = _mm_crc32_u64(c0, v[0]); c1 = _mm_crc32_u64(c1, v[1]);
        c2 = _mm_crc32_u64(c2, v[2]); c3 = _mm_crc32_u64(c3, v[3]);
        c4 = _mm_crc32_u64(c4, v[4]); c5 = _mm_crc32_u64(c5, v[5]);
        c6 = _mm_crc32_u64(c6, v[6]); c7 = _mm_crc32_u64(c7, v[7]);
    }
    const std::uint64_t c[8] = {c0, c1, c2, c3, c4, c5, c6, c7};
    for (int k = 0; k < 8; ++k) out[k] = static_cast<std::uint32_t>(c[k]) ^ 0xffffffffu;
}
inline bool crc32c_hardware_available() {
#if defined(_MSC_VER)
    int info[4] = {};
    __cpuid(info, 1);
    return (info[2] & (1 << 20)) != 0;
#else
    unsigned a = 0, b = 0, c = 0, d = 0;
    return __get_cpuid(1, &a, &b, &c, &d) && (c & (1u << 20)) != 0;
#endif
}
#elif defined(NX_CRC32C_ARM)
NX_CRC32C_TARGET inline std::uint32_t crc32c_hardware(std::uint32_t crc, const std::uint8_t* p, std::size_t n) {
    while (n >= 8) { std::uint64_t v; std::memcpy(&v, p, 8); crc = __crc32cd(crc, v); p += 8; n -= 8; }
    while (n--) crc = __crc32cb(crc, *p++);
    return crc;
}
NX_CRC32C_TARGET inline void crc32c_hardware_x8(const std::uint8_t* p, std::size_t n, std::uint32_t* out) {
    std::uint32_t c0 = 0xffffffffu, c1 = c0, c2 = c0, c3 = c0, c4 = c0, c5 = c0, c6 = c0, c7 = c0;
    for (std::size_t i = 0; i < n; i += 8) {
        std::uint64_t v[8];
        for (int k = 0; k < 8; ++k) std::memcpy(&v[k], p + k * n + i, 8);
        c0 = __crc32cd(c0, v[0]); c1 = __crc32cd(c1, v[1]);
        c2 = __crc32cd(c2, v[2]); c3 = __crc32cd(c3, v[3]);
        c4 = __crc32cd(c4, v[4]); c5 = __crc32cd(c5, v[5]);
        c6 = __crc32cd(c6, v[6]); c7 = __crc32cd(c7, v[7]);
    }
    const std::uint32_t c[8] = {c0, c1, c2, c3, c4, c5, c6, c7};
    for (int k = 0; k < 8; ++k) out[k] = c[k] ^ 0xffffffffu;
}
inline bool crc32c_hardware_available() {
#if defined(__ARM_FEATURE_CRC32) || defined(__APPLE__) || defined(_WIN32)
    return true;  // Apple silicon and Windows on Arm always implement it.
#elif defined(__linux__)
#ifndef HWCAP_CRC32
#define HWCAP_CRC32 (1 << 7)
#endif
    return (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0;
#else
    return false;
#endif
}
#endif

inline Crc32cFunction crc32c_function() {
#if defined(NX_CRC32C_X86) || defined(NX_CRC32C_ARM)
    static const Crc32cFunction selected = crc32c_hardware_available() ? &crc32c_hardware : &crc32c_table;
    return selected;
#else
    return &crc32c_table;
#endif
}

// 8 slice CRCs at once when the CPU has CRC instructions, else nullptr.
using Crc32cX8Function = void (*)(const std::uint8_t*, std::size_t, std::uint32_t*);
inline Crc32cX8Function crc32c_x8_function() {
#if defined(NX_CRC32C_X86) || defined(NX_CRC32C_ARM)
    static const Crc32cX8Function selected = crc32c_hardware_available() ? &crc32c_hardware_x8 : nullptr;
    return selected;
#else
    return nullptr;
#endif
}

inline std::uint32_t crc32c(const std::uint8_t* p, std::size_t n) {
    return crc32c_function()(0xffffffffu, p, n) ^ 0xffffffffu;
}
} // namespace detail

// Integrity granule of NX 0.2: one CRC-32C per 16 KiB logical slice.
constexpr std::size_t check_size = 16 * 1024;

enum class ErrorCode {
    io, bounds, unsupported, integrity, format, resource_limit,
    decompression, not_found, invalid_argument, cancelled
};

struct Error {
    ErrorCode code = ErrorCode::format;
    std::string message;
};

template<class T>
struct Result {
    std::optional<T> value;
    Error error;
    explicit operator bool() const noexcept { return value.has_value(); }
    bool ok() const noexcept { return value.has_value(); }
    static Result success(T v) { Result r; r.value.emplace(std::move(v)); return r; }
    static Result failure(ErrorCode c, std::string m) { Result r; r.error = {c, std::move(m)}; return r; }
};

using Status = Result<bool>;

// Result of a file read: a Status (so `!read(...)`, `.error` and storing it
// in a Status keep working) plus `bytes`, the verified file bytes written to
// output[0, bytes). output[bytes, length) never holds unverified data: it is
// untouched, or zeroed where RAW slices were read into it directly. It is ok
// exactly when bytes == length, so there is no third state.
struct PartialRead : Status {
    std::size_t bytes = 0;
    PartialRead() = default;
    PartialRead(Status status, std::size_t verified) : Status(std::move(status)), bytes(verified) {}
};

// Backing bytes must remain immutable from open() until Archive destruction.
// File adapters must prevent writes; remote adapters must pin an object version.
struct RandomAccessSource {
    virtual ~RandomAccessSource() = default;
    virtual std::uint64_t size() const noexcept = 0;
    virtual bool read_at(std::uint64_t offset, std::uint8_t* destination, std::size_t length) = 0;
};

// The callback must decode exactly one standard frame, reject dictionaries,
// skippable/concatenated frames and trailing bytes, cap the frame window at
// 1 MiB, and write no more than output_capacity bytes.
using ZstdDecoder = bool (*)(void* context, const std::uint8_t* frame,
                             std::size_t frame_size, std::uint8_t* output,
                             std::size_t output_capacity, std::size_t* written,
                             std::size_t* consumed);

// Optional caller-owned SHA-256 implementation. NX 0.2 verifies blocks with
// per-slice CRC-32C, so the reader no longer calls it; the field is kept for
// source compatibility with 0.1 callers.
using BlockSha256 = bool (*)(void* context, const std::uint8_t* bytes,
                             std::size_t size, std::uint8_t output[32]);

struct OpenOptions {
    std::uint64_t max_manifest_bytes = 128ull * 1024 * 1024;
    std::uint64_t max_directories = 250'000;
    std::uint64_t max_files = 500'000;
    std::uint64_t max_entries = 1'000'000;
    std::uint64_t max_block_references = 4'000'000;
    std::uint64_t max_blocks = 4'000'000;
    std::uint64_t max_snapshot_nodes = 1'000'000;
    std::uint64_t max_snapshot_path_bytes = 64ull * 1024 * 1024;
    std::uint64_t max_index_bytes = 256ull * 1024 * 1024;
    std::uint64_t max_total_snapshot_nodes = 16'000'000;
    std::uint64_t max_total_snapshot_path_bytes = 512ull * 1024 * 1024;
    // Optional verified-block LRU cache; both payload and entry counts are capped.
    std::uint64_t max_cached_block_bytes = 0;
    std::size_t max_cached_blocks = 128;
    bool (*is_cancelled)(void*) = nullptr;
    void* cancellation_context = nullptr;
    std::uint64_t max_read_bytes = 16ull * 1024 * 1024;
    // false (default): read_file() writes each verified block straight to the
    // output, so a failed read may leave verified bytes of earlier blocks in
    // it (counted by PartialRead::bytes); unverified bytes are zeroed before
    // returning. true: multi-block ranges are staged and the output is left
    // unchanged on failure (slower: one range-sized allocation and copy per
    // multi-block read).
    bool atomic_reads = false;
    bool verify_read_blocks = true;
    ZstdDecoder zstd = nullptr;
    void* zstd_context = nullptr;
    BlockSha256 block_sha256 = nullptr;
    void* block_sha256_context = nullptr;
    bool collect_read_stats = false;
    // Diagnostic clocks require NX_READER_DIAGNOSTICS at build time as well.
    // Normal builds contain no added clock calls or hot copy-path branches.
    bool collect_read_timing = false;
};

// Per mounted archive; keys are physical block IDs shared by every file/snapshot.
struct CacheStats {
    std::uint64_t hits = 0, misses = 0, load_waits = 0, coalesced = 0;
    std::uint64_t decompressions = 0, decompression_ns = 0, raw_reads = 0;
    std::uint64_t stored_bytes_read = 0, evictions = 0;
    std::uint64_t bytes = 0, peak_bytes = 0, entries = 0;
    std::uint64_t io_ns = 0, hash_ns = 0, copy_ns = 0;
};

// Source data has independent roots, never executable snapshot ownership.
struct SourceResource {
    std::uint32_t application_index = 0xffffffffu;
    std::uint32_t kind = 0; // 0 opaque source files, 1 original system-update HFS0
    std::uint32_t root_directory = 0xffffffffu;
};

struct Application {
    std::uint64_t id = 0;
    std::uint8_t platform = 0;
    std::uint8_t flags = 0;
    std::uint32_t first_program = 0xffffffffu, program_count = 0;
    std::uint32_t first_snapshot = 0, snapshot_count = 0;
    std::uint32_t title_offset = 0xffffffffu, title_length = 0;
    std::uint32_t publisher_offset = 0xffffffffu, publisher_length = 0;
    std::uint32_t icon_index = 0xffffffffu;
    std::uint32_t first_localized = 0, localized_count = 0;
    std::uint32_t nacp_index = 0xffffffffu;
    std::uint32_t first_opaque = 0, opaque_count = 0;
    std::uint32_t metadata_snapshot = 0xffffffffu;
};

struct Program {
    std::uint64_t id = 0;
    std::uint32_t application_index = 0;
    std::uint8_t kind = 0, flags = 0;
    std::array<std::uint8_t, 16> content_id{};
    std::uint32_t first_snapshot = 0, snapshot_count = 0;
    std::uint32_t npdm_index = 0xffffffffu;
};

struct Snapshot {
    std::uint32_t program_index = 0xffffffffu;
    std::uint8_t type = 0, flags = 0;
    std::uint32_t numeric_version = 0, root_directory = 0;
    std::uint64_t logical_size = 0;
    std::uint32_t display_version_offset = 0xffffffffu, display_version_length = 0;
    std::array<std::uint8_t, 16> content_id{};
    std::uint32_t first_dependency = 0, dependency_count = 0;
    std::array<std::uint8_t, 32> identity{}, build_id{};
    std::uint32_t npdm_index = 0xffffffffu;
    // Resolved from existing Application/history metadata; no new wire field.
    std::uint32_t nacp_index = 0xffffffffu;
    std::optional<std::uint32_t> required_firmware;
    // Flag bit 1: a DLC stored by its add-on title ID (no native Content ID),
    // as converted from NUP packages and extracted dumps.
    std::optional<std::uint64_t> addon_title_id() const {
        if (!(flags&2)) return std::nullopt;
        std::uint64_t id=0;
        for (int i=7;i>=0;--i) id=(id<<8)|content_id[static_cast<std::size_t>(i)];
        return id;
    }
};

struct Dependency {
    std::uint8_t kind = 0;
    std::optional<std::array<std::uint8_t,16>> content_id;
    std::array<std::uint8_t,32> target_identity{};
};

struct LocalizedEntry {
    std::uint32_t language_offset = 0, language_length = 0;
    std::uint32_t title_offset = 0xffffffffu, title_length = 0;
    std::uint32_t icon_index = 0xffffffffu;
};

enum class EntryType : std::uint8_t { directory = 1, file = 2 };

// Handles are numeric indexes scoped to one mounted Archive. Do not retain
// them after that Archive is destroyed or use them with another Archive.
struct FileId { std::uint32_t value = 0xffffffffu; };
struct DirectoryHandle {
    std::uint32_t snapshot_index = 0xffffffffu;
    std::uint32_t directory_index = 0xffffffffu;
};
struct FileInfo {
    FileId id{};
    EntryType type = EntryType::file;
    std::uint64_t size = 0;
    std::uint8_t chunk_log2 = 0;
};
struct DirectoryEntryView {
    std::string_view name;
    EntryType type = EntryType::file;
    FileId file{};
    DirectoryHandle directory{};
    std::uint64_t size = 0;
};

namespace detail {
// SHA-256 compression of whole 64-byte blocks: SHA-NI on x86-64 (selected at
// run time), the ARMv8 SHA2 extension when the build targets it, else the
// portable code in Sha256. The Manifest digest dominates mount time.
using Sha256BlocksFunction = void (*)(std::uint32_t*, const std::uint8_t*, std::size_t);
alignas(16) inline constexpr std::uint32_t sha256_k[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};
#if defined(NX_CRC32C_X86)
#if defined(__GNUC__) || defined(__clang__)
#define NX_SHA_TARGET __attribute__((target("sha,sse4.1,ssse3")))
#else
#define NX_SHA_TARGET
#endif
NX_SHA_TARGET inline void sha256_blocks_hardware(std::uint32_t* state, const std::uint8_t* data, std::size_t blocks) {
    const __m128i mask = _mm_set_epi64x(0x0c0d0e0f08090a0bll, 0x0405060700010203ll);
    __m128i tmp = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(state)), 0xb1);
    __m128i state1 = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(state + 4)), 0x1b);
    __m128i state0 = _mm_alignr_epi8(tmp, state1, 8);   // ABEF
    state1 = _mm_blend_epi16(state1, tmp, 0xf0);         // CDGH
    while (blocks--) {
        const __m128i abef = state0, cdgh = state1;
        __m128i w[4];
        for (int i = 0; i < 16; ++i) {
            if (i < 4) {
                w[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16 * i)), mask);
            } else {
                __m128i next = _mm_sha256msg1_epu32(w[i & 3], w[(i + 1) & 3]);
                next = _mm_add_epi32(next, _mm_alignr_epi8(w[(i + 3) & 3], w[(i + 2) & 3], 4));
                w[i & 3] = _mm_sha256msg2_epu32(next, w[(i + 3) & 3]);
            }
            __m128i msg = _mm_add_epi32(w[i & 3], _mm_load_si128(reinterpret_cast<const __m128i*>(sha256_k + 4 * i)));
            state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
            msg = _mm_shuffle_epi32(msg, 0x0e);
            state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        }
        state0 = _mm_add_epi32(state0, abef);
        state1 = _mm_add_epi32(state1, cdgh);
        data += 64;
    }
    tmp = _mm_shuffle_epi32(state0, 0x1b);       // FEBA
    state1 = _mm_shuffle_epi32(state1, 0xb1);    // DCHG
    state0 = _mm_blend_epi16(tmp, state1, 0xf0); // DCBA
    state1 = _mm_alignr_epi8(state1, tmp, 8);    // HGFE
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state), state0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state + 4), state1);
}
inline bool sha256_hardware_available() {
#if defined(_MSC_VER)
    int info[4] = {};
    __cpuid(info, 0);
    if (info[0] < 7) return false;
    __cpuid(info, 1);
    const bool sse = (info[2] & (1 << 9)) && (info[2] & (1 << 19));
    __cpuidex(info, 7, 0);
    return sse && (info[1] & (1 << 29));
#else
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & (1u << 9)) || !(c & (1u << 19))) return false;
    if (__get_cpuid_max(0, nullptr) < 7) return false;
    __cpuid_count(7, 0, a, b, c, d);
    return (b & (1u << 29)) != 0;
#endif
}
#define NX_SHA256_HARDWARE 1
#elif defined(NX_CRC32C_ARM) && defined(__ARM_FEATURE_SHA2) && !defined(_MSC_VER)
// Compiled only when the target guarantees the extension (Apple silicon,
// or -march=armv8-a+crypto); other ARM builds use the portable code.
inline void sha256_blocks_hardware(std::uint32_t* state, const std::uint8_t* data, std::size_t blocks) {
    uint32x4_t state0 = vld1q_u32(state), state1 = vld1q_u32(state + 4);
    while (blocks--) {
        const uint32x4_t abcd = state0, efgh = state1;
        uint32x4_t w[4];
        for (int i = 0; i < 16; ++i) {
            if (i < 4) w[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 16 * i)));
            else w[i & 3] = vsha256su1q_u32(vsha256su0q_u32(w[i & 3], w[(i + 1) & 3]), w[(i + 2) & 3], w[(i + 3) & 3]);
            const uint32x4_t msg = vaddq_u32(w[i & 3], vld1q_u32(sha256_k + 4 * i));
            const uint32x4_t previous = state0;
            state0 = vsha256hq_u32(state0, state1, msg);
            state1 = vsha256h2q_u32(state1, previous, msg);
        }
        state0 = vaddq_u32(state0, abcd);
        state1 = vaddq_u32(state1, efgh);
        data += 64;
    }
    vst1q_u32(state, state0);
    vst1q_u32(state + 4, state1);
}
inline bool sha256_hardware_available() { return true; }
#define NX_SHA256_HARDWARE 1
#endif
inline Sha256BlocksFunction sha256_blocks_function() {
#if defined(NX_SHA256_HARDWARE)
    static const Sha256BlocksFunction selected = sha256_hardware_available() ? &sha256_blocks_hardware : nullptr;
    return selected;
#else
    return nullptr;
#endif
}
}  // namespace detail

class Sha256 {
public:
    void update(const std::uint8_t* data, std::size_t size) {
        total_ += size;
        if (const auto blocks = detail::sha256_blocks_function()) {
            if (used_) {
                const std::size_t take = std::min(size, block_.size() - used_);
                std::memcpy(block_.data() + used_, data, take);
                used_ += take; data += take; size -= take;
                if (used_ < block_.size()) return;
                blocks(state_.data(), block_.data(), 1); used_ = 0;
            }
            blocks(state_.data(), data, size / 64);
            data += size / 64 * 64; size %= 64;
            std::memcpy(block_.data(), data, size); used_ = size;
            return;
        }
        while (size) {
            const std::size_t take = std::min(size, block_.size() - used_);
            std::memcpy(block_.data() + used_, data, take);
            used_ += take; data += take; size -= take;
            if (used_ == block_.size()) { transform(block_.data()); used_ = 0; }
        }
    }
    std::array<std::uint8_t, 32> finish() {
        const std::uint64_t bits = total_ * 8;
        block_[used_++] = 0x80;
        if (used_ > 56) { std::fill(block_.begin() + used_, block_.end(), std::uint8_t{0}); transform(block_.data()); used_ = 0; }
        std::fill(block_.begin() + used_, block_.begin() + 56, std::uint8_t{0});
        for (int i = 0; i < 8; ++i) block_[63 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
        transform(block_.data());
        std::array<std::uint8_t, 32> out{};
        for (std::size_t i = 0; i < state_.size(); ++i) {
            out[4 * i] = static_cast<std::uint8_t>(state_[i] >> 24);
            out[4 * i + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
            out[4 * i + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
            out[4 * i + 3] = static_cast<std::uint8_t>(state_[i]);
        }
        return out;
    }
    static std::array<std::uint8_t, 32> digest(const std::uint8_t* data, std::size_t size) {
        Sha256 h; h.update(data, size); return h.finish();
    }
private:
    static constexpr std::array<std::uint32_t, 64> k_ = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    static std::uint32_t rotr(std::uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
    void transform(const std::uint8_t* p) {
        if (const auto blocks = detail::sha256_blocks_function()) { blocks(state_.data(), p, 1); return; }
        std::uint32_t w[64]{};
        for (int i = 0; i < 16; ++i) w[i] = (std::uint32_t(p[i*4]) << 24) | (std::uint32_t(p[i*4+1]) << 16) | (std::uint32_t(p[i*4+2]) << 8) | p[i*4+3];
        for (int i = 16; i < 64; ++i) {
            const auto s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
            const auto s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        auto a=state_[0], b=state_[1], c=state_[2], d=state_[3], e=state_[4], f=state_[5], g=state_[6], h=state_[7];
        for (int i = 0; i < 64; ++i) {
            const auto s1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            const auto ch = (e & f) ^ (~e & g);
            const auto t1 = h + s1 + ch + k_[i] + w[i];
            const auto s0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            const auto maj = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = s0 + maj;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        state_[0]+=a; state_[1]+=b; state_[2]+=c; state_[3]+=d;
        state_[4]+=e; state_[5]+=f; state_[6]+=g; state_[7]+=h;
    }
    std::array<std::uint32_t, 8> state_ = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    std::array<std::uint8_t, 64> block_{};
    std::size_t used_ = 0;
    std::uint64_t total_ = 0;
};

struct Archive {
    Archive() = default;

    static Result<Archive> open(RandomAccessSource& source, const OpenOptions& options = {}) {
        try {
            Archive archive;
            archive.source_ = &source;
            archive.options_ = options;
            Status st = archive.open_impl();
            if (!st) return Result<Archive>::failure(st.error.code, st.error.message);
            return Result<Archive>::success(std::move(archive));
        } catch (const std::bad_alloc&) {
            return Result<Archive>::failure(ErrorCode::resource_limit, "allocation failed within configured reader limits");
        }
    }

    Status mount() {
        if (!source_) return fail_status(ErrorCode::invalid_argument, "archive has no source");
        if (mounted_) return ok_status();
        const auto reset_mount = [this]() noexcept {
            std::vector<std::uint8_t>().swap(manifest_bytes_);
            std::vector<Directory>().swap(directories_);
            std::vector<Entry>().swap(entries_);
            std::vector<File>().swap(files_);
            std::vector<std::uint32_t>().swap(block_refs_);
            std::vector<std::uint32_t>().swap(block_logical_sizes_);
            std::vector<std::uint8_t>().swap(index_bytes_);
            block_cache_.reset();
        };
        reset_mount();
        try {
            Status st = mount_impl();
            if (st) mounted_ = true;
            else reset_mount();
            return st;
        } catch (const std::bad_alloc&) {
            reset_mount();
            return fail_status(ErrorCode::resource_limit, "mount allocation failed within configured reader limits");
        }
    }

    bool mounted() const noexcept { return mounted_; }
    const std::vector<Application>& applications() const noexcept { return applications_; }
    const std::vector<Program>& programs() const noexcept { return programs_; }
    const std::vector<Snapshot>& snapshots() const noexcept { return snapshots_; }
    const std::vector<SourceResource>& source_resources() const noexcept { return source_resources_; }
    Result<FileInfo> source_resource_file(std::uint32_t index, std::string_view name) const {
        if (!mounted_) return Result<FileInfo>::failure(ErrorCode::invalid_argument,"mount() must succeed before resource access");
        if (index>=source_resources_.size()) return Result<FileInfo>::failure(ErrorCode::bounds,"source resource index out of range");
        // Resource roots are flat file sets validated at mount: one sorted lookup.
        const auto found=find_entry(source_resources_[index].root_directory,name);
        if (found==no_index) return Result<FileInfo>::failure(ErrorCode::not_found,"source resource file not found");
        return file_info({entries_[found].target});
    }
    std::string_view snapshot_display_version(std::uint32_t index) const noexcept {
        if (index>=snapshots_.size()) return {};
        const auto& row=snapshots_[index];
        return text(row.display_version_offset,row.display_version_length);
    }
    const std::vector<LocalizedEntry>& localized_entries() const noexcept { return localized_; }
    std::uint64_t block_count() const noexcept { return block_count_; }
    // Header generation selected at open(); 0 for a freshly written archive.
    std::uint64_t generation() const noexcept { return generation_; }
    // Mounted immutable metadata capacities, including the Archive object.
    // Excludes allocator bookkeeping, mutable cache nodes and read temporaries;
    // callers must also measure process RSS rather than treating this as total RAM.
    std::uint64_t metadata_capacity_bytes() const noexcept {
        auto bytes=[](const auto& vector) {
            return std::uint64_t(vector.capacity())*sizeof(typename std::decay_t<decltype(vector)>::value_type);
        };
        return sizeof(*this)+bytes(catalog_bytes_)+bytes(manifest_bytes_)+bytes(index_bytes_)+
            bytes(applications_)+bytes(programs_)+bytes(snapshots_)+bytes(localized_)+
            bytes(snapshot_owner_)+bytes(media_expected_mask_)+bytes(media_use_count_)+
            bytes(directories_)+bytes(entries_)+bytes(files_)+bytes(block_refs_)+bytes(block_logical_sizes_);
    }
    CacheStats cache_stats() const {
        if (!block_cache_) return {};
        std::lock_guard<std::shared_mutex> lock(block_cache_->mutex);
        auto result = block_cache_->stats;
        for (const auto& slot : block_cache_->counters) {
            const auto get=[](const std::atomic<std::uint64_t>& v) { return v.load(std::memory_order_relaxed); };
            result.hits+=get(slot.hits); result.misses+=get(slot.misses);
            result.coalesced+=get(slot.coalesced); result.load_waits+=get(slot.load_waits);
            result.raw_reads+=get(slot.raw_reads); result.decompressions+=get(slot.decompressions);
            result.decompression_ns+=get(slot.decompression_ns);
            result.stored_bytes_read+=get(slot.raw_bytes)+get(slot.stored_bytes_read);
        }
        result.bytes = block_cache_->bytes;
        result.entries = block_cache_->entries.size();
        return result;
    }
    // Relationships may refer to omitted provenance snapshots. A native loader
    // must additionally resolve all required targets before starting a process.
    template<class Visitor>
    Status dependencies(std::uint32_t snapshot, Visitor&& visitor) const {
        if (!mounted_ || snapshot>=snapshots_.size()) return fail_status(ErrorCode::bounds,"invalid mounted snapshot");
        const auto& row=snapshots_[snapshot];
        const auto table=cat64(80);
        for (std::uint32_t i=0;i<row.dependency_count;++i) {
            const auto at=table+std::uint64_t(row.first_dependency+i)*56;
            Dependency dependency;
            dependency.kind=catalog_bytes_[at];
            if (catalog_bytes_[at+1]&1) {
                std::array<std::uint8_t,16> id{};
                std::copy_n(catalog_bytes_.data()+at+4,16,id.data()); dependency.content_id=id;
            }
            std::copy_n(catalog_bytes_.data()+at+20,32,dependency.target_identity.data());
            if (!visitor(dependency)) break;
        }
        return ok_status();
    }

    std::string_view title(std::uint32_t application_index) const noexcept {
        return application_index < applications_.size() ? text(applications_[application_index].title_offset, applications_[application_index].title_length) : std::string_view{};
    }
    std::string_view publisher(std::uint32_t application_index) const noexcept {
        return application_index < applications_.size() ? text(applications_[application_index].publisher_offset, applications_[application_index].publisher_length) : std::string_view{};
    }
    std::string_view localized_language(std::uint32_t index) const noexcept {
        if (index >= localized_.size()) return {};
        const auto& x = localized_[index]; return text(x.language_offset, x.language_length);
    }
    std::string_view localized_title(std::uint32_t index) const noexcept {
        if (index >= localized_.size()) return {};
        const auto& x = localized_[index]; return text(x.title_offset, x.title_length);
    }

    Result<DirectoryHandle> root_directory(std::uint32_t snapshot_index) const {
        if (!mounted_) return Result<DirectoryHandle>::failure(ErrorCode::invalid_argument, "mount() must succeed before directory access");
        if (snapshot_index >= snapshots_.size()) return Result<DirectoryHandle>::failure(ErrorCode::bounds, "snapshot index is out of range");
        return Result<DirectoryHandle>::success({snapshot_index, snapshots_[snapshot_index].root_directory});
    }

    // The callback receives string_views into Manifest storage owned by this
    // Archive. Enumeration itself allocates no memory. Return false to stop.
    // All views and handles expire when this Archive is destroyed.
    template<class Visitor>
    Status enumerate(DirectoryHandle directory, Visitor&& visitor) const {
        if (!mounted_) return fail_status(ErrorCode::invalid_argument, "mount() must succeed before directory enumeration");
        if (directory.snapshot_index >= snapshots_.size() || directory.directory_index >= directories_.size())
            return fail_status(ErrorCode::bounds, "directory handle is out of range");
        const auto& row = directories_[directory.directory_index];
        for (std::uint32_t i = 0; i < row.count; ++i) {
            const auto& entry = entries_[row.first + i];
            DirectoryEntryView view;
            view.name = {reinterpret_cast<const char*>(manifest_bytes_.data() + entry.name_offset), entry.name_length};
            view.type = entry.kind == 1 ? EntryType::directory : EntryType::file;
            if (entry.kind == 1) {
                view.directory = {directory.snapshot_index, entry.target};
            } else {
                view.file = {entry.target};
                view.size = files_[entry.target].size;
            }
            if (!visitor(view)) break;
        }
        return ok_status();
    }

    // One child of a directory by exact name, in O(log n): entries are stored
    // sorted by byte order. Use this instead of enumerate() to open one file
    // or subdirectory (VFS GetFile/GetSubdirectory). not_found if absent.
    Result<DirectoryEntryView> lookup(DirectoryHandle directory, std::string_view name) const {
        using R = Result<DirectoryEntryView>;
        if (!mounted_) return R::failure(ErrorCode::invalid_argument, "mount() must succeed before directory lookup");
        if (directory.snapshot_index >= snapshots_.size() || directory.directory_index >= directories_.size())
            return R::failure(ErrorCode::bounds, "directory handle is out of range");
        const auto index = find_entry(directory.directory_index, name);
        if (index == no_index) return R::failure(ErrorCode::not_found, "directory entry was not found");
        const auto& entry = entries_[index];
        DirectoryEntryView view;
        view.name = {reinterpret_cast<const char*>(manifest_bytes_.data() + entry.name_offset), entry.name_length};
        view.type = entry.kind == 1 ? EntryType::directory : EntryType::file;
        if (entry.kind == 1) {
            view.directory = {directory.snapshot_index, entry.target};
        } else {
            view.file = {entry.target};
            view.size = files_[entry.target].size;
        }
        return R::success(view);
    }

    Result<FileInfo> resolve_file(std::uint32_t snapshot_index, std::string_view path) const {
        if (!mounted_) return Result<FileInfo>::failure(ErrorCode::invalid_argument, "mount() must succeed before path resolution");
        std::uint32_t file_id = no_index;
        Status st = find_file(snapshot_index, path, file_id);
        if (!st) return Result<FileInfo>::failure(st.error.code, st.error.message);
        return file_info({file_id});
    }

    Result<FileInfo> file_info(FileId id) const {
        if (!mounted_) return Result<FileInfo>::failure(ErrorCode::invalid_argument, "mount() must succeed before file access");
        if (id.value >= files_.size()) return Result<FileInfo>::failure(ErrorCode::bounds, "file ID is out of range");
        const auto& file = files_[id.value];
        return Result<FileInfo>::success({id, EntryType::file, file.size, file.chunk_log2});
    }

    // True when the block holding `offset` is Zstandard and not cached yet,
    // so warming it ahead of a demand read saves a decode on that thread.
    bool block_needs_decode(FileId id, std::uint64_t offset) const {
        if (!mounted_ || !block_cache_ || !options_.verify_read_blocks || id.value>=files_.size()) return false;
        const auto& file=files_[id.value];
        if (offset>=file.size) return false;
        const auto block_id=block_refs_[file.first+static_cast<std::uint32_t>(offset>>file.chunk_log2)];
        Block block;
        if (!block_record(block_id,block) || block.codec==0) return false;
        std::shared_lock<std::shared_mutex> lock(block_cache_->mutex);
        return block_cache_->entries.find(block_id)==block_cache_->entries.end();
    }

    // Authenticated Manifest digest for optional access-profile binding. Does
    // not read game payloads or enlarge FileInfo on the ordinary read path.
    Result<std::array<std::uint8_t,32>> file_digest(FileId id) const {
        if (!mounted_ || id.value>=files_.size()) return Result<std::array<std::uint8_t,32>>::failure(ErrorCode::bounds,"file digest unavailable");
        return Result<std::array<std::uint8_t,32>>::success(files_[id.value].digest);
    }

    // Icons and other media are intentionally loaded on demand and are
    // available after open(), before a full Manifest mount.
    Result<std::vector<std::uint8_t>> read_media(std::uint32_t media_index) {
        try { return read_media_impl(media_index); }
        catch (const std::bad_alloc&) { return Result<std::vector<std::uint8_t>>::failure(ErrorCode::resource_limit, "media allocation failed"); }
    }

    // Reads one file range into caller-owned storage. `ok` exactly when
    // `bytes == length`. Every byte reaches `output` only after its block (or
    // 16 KiB RAW slice) has verified. By default verified blocks are written
    // straight to `output` in file order, with no range-sized staging; on any
    // error (corruption, IO, cancellation, decoder) `bytes` leading bytes are
    // verified and written, and the rest of `output` is untouched or zeroed:
    // never unverified. With OpenOptions::atomic_reads a multi-block range is
    // staged instead, `output` is unchanged on failure and `bytes` is 0.
    PartialRead read_file(std::uint32_t snapshot_index, std::string_view path,
                          std::uint64_t offset, std::size_t length,
                          std::uint8_t* output, std::size_t output_capacity) {
        if (!mounted_) return failed_read(ErrorCode::invalid_argument, "mount() must succeed before file reads");
        std::uint32_t file_id = no_index;
        try {
            Status st = find_file(snapshot_index, path, file_id);
            if (!st) return PartialRead(std::move(st), 0);
        }
        catch (const std::bad_alloc&) { return failed_read(ErrorCode::resource_limit, "read allocation failed"); }
        return read_file(FileId{file_id}, offset, length, output, output_capacity);
    }

    // Resolve once with resolve_file(), then use this overload for repeated
    // random reads without repeating path-component binary searches.
    PartialRead read_file(FileId file_id, std::uint64_t offset, std::size_t length,
                          std::uint8_t* output, std::size_t output_capacity) {
        if (!mounted_) return failed_read(ErrorCode::invalid_argument, "mount() must succeed before file reads");
        if (length > output_capacity || (length && output == nullptr) || length > options_.max_read_bytes)
            return failed_read(ErrorCode::resource_limit, "read buffer or configured read limit is too small");
        std::size_t done = 0;
        try {
            if (options_.atomic_reads) {
                Status st = read_file_impl(file_id.value, offset, length, output);
                const bool ok = st.ok();
                return PartialRead(std::move(st), ok ? length : 0);
            }
            Status st = read_range(file_id.value, offset, length, output, Sink::partial, done);
            return PartialRead(std::move(st), done);
        }
        catch (const std::bad_alloc&) { return PartialRead(fail_status(ErrorCode::resource_limit, "read allocation failed"), done); }
    }

    Status verify_block(std::uint32_t block_id) {
        if (!mounted_) return fail_status(ErrorCode::invalid_argument, "mount() must succeed before block verification");
        try {
            detail::LogicalBytes logical;
            Status st = load_block(block_id, true, logical, nullptr);
            return st ? ok_status() : st;
        } catch (const std::bad_alloc&) { return fail_status(ErrorCode::resource_limit, "block allocation failed"); }
    }

    Status verify_all() {
        if (!mounted_) return fail_status(ErrorCode::invalid_argument, "mount() must succeed before full verification");
        try {
            for (std::uint32_t i=0;i<media_record_count_;++i) {
                auto media=read_media_impl(i);
                if (!media) return fail_status(media.error.code,media.error.message);
            }
            // Verify each stored block once, while still hashing every file view.
            std::vector<std::uint8_t> checked(static_cast<std::size_t>(block_count_), 0);
            // One decode buffer and one stored-bytes scratch for the whole pass.
            detail::LogicalBytes logical;
            ReadBuffer stored;
            for (const auto& file : files_) {
                Sha256 hash;
                for (std::uint32_t j = 0; j < file.count; ++j) {
                    const std::uint32_t id = block_refs_[file.first + j];
                    Status st = load_block(id, checked[id] == 0, logical, nullptr, &stored);
                    if (!st) return st;
                    checked[id] = 1;
                    hash.update(logical.data(), logical.size());
                }
                if (hash.finish() != file.digest) return fail_status(ErrorCode::integrity, "file SHA-256 mismatch");
            }
            // Full verification also covers physical blocks not referenced by files.
            for (std::uint64_t i = 0; i < block_count_; ++i) {
                if (checked[static_cast<std::size_t>(i)]) continue;
                Status st = load_block(static_cast<std::uint32_t>(i), true, logical, nullptr, &stored);
                if (!st) return st;
            }
            return ok_status();
        } catch (const std::bad_alloc&) { return fail_status(ErrorCode::resource_limit, "full verification allocation failed"); }
    }

private:
    static constexpr std::uint32_t no_index = 0xffffffffu;
    // Normative NX limit (SECURITY.md): deeper snapshot trees reject.
    static constexpr std::size_t max_directory_depth = 256;
    struct File { std::uint64_t size = 0; std::uint32_t first = 0, count = 0; std::uint8_t chunk_log2 = 16; std::array<std::uint8_t, 32> digest{}; };
    struct Directory { std::uint32_t first = 0, count = 0; };
    struct Entry { std::uint64_t name_offset = 0; std::uint32_t name_length = 0, target = 0; std::uint8_t kind = 0; };
    struct Block { std::uint64_t offset = 0; std::uint32_t stored = 0, logical = 0, first_check = 0; std::uint8_t codec = 0; };
    struct Media { std::uint16_t type = 0; std::uint64_t stored = 0, offset = 0; std::array<std::uint8_t, 32> digest{}; };

    // Call-owned scratch: no retained TLS, cross-thread aliasing or global pool.
    // new[] deliberately leaves bytes uninitialized; no byte is consumed until
    // a successful exact read/decode has produced it.
    struct ReadBuffer {
        std::unique_ptr<std::uint8_t[]> bytes;
        std::size_t capacity = 0;
        void reserve(std::size_t size) {
            if (size>capacity) {
                bytes.reset();
                capacity=0;
                bytes.reset(new std::uint8_t[size]);
                capacity=size;
            }
        }
    };
    // Readers pin a block (shared_ptr copy under the shared cache lock) and copy
    // from it after unlocking. Eviction only drops the cache's reference, so a
    // block being copied stays alive; at most one block per in-flight read
    // outlives its eviction, which keeps memory bounded by the admission limit.
    // `pins` counts readers still copying: taken under the shared lock, dropped
    // with release after the copy. A block is recycled only when it reads 0
    // with acquire under the exclusive lock, so every finished copy happens
    // before the buffer is reused and no new pin can start meanwhile.
    // `referenced` is the second-chance bit: a hit only sets it (no list
    // update), so hits run concurrently under the shared lock. Eviction moves
    // referenced blocks to the back once before it drops them (CLOCK).
    struct BlockData {
        detail::LogicalBytes bytes;
        std::atomic<std::uint32_t> pins{0};
        std::atomic<bool> referenced{false};
    };
    using BlockBytes = std::shared_ptr<BlockData>;
    struct CachedBlock { BlockBytes bytes; };
    // Hot counters are spread over per-thread slots, so concurrent hits do not
    // share a written cache line; cache_stats() sums them. The trailing pad
    // keeps two slots' counters out of one 64-byte line without alignas
    // (which MSVC /W4 reports as C4324).
    struct CounterSlot {
        std::atomic<std::uint64_t> hits{0}, misses{0}, coalesced{0}, load_waits{0};
        std::atomic<std::uint64_t> raw_reads{0}, raw_bytes{0}, decompressions{0}, decompression_ns{0}, stored_bytes_read{0};
        char pad[64];
    };
    static std::size_t counter_slot() {
        static std::atomic<std::size_t> next{0};
        thread_local const std::size_t slot=next.fetch_add(1,std::memory_order_relaxed);
        return slot;
    }
    struct BlockCache {
        std::shared_mutex mutex;
        char mutex_pad[64];  // the lock word is written by every hit
        std::array<CounterSlot, 16> counters;
        CounterSlot& counter() { return counters[counter_slot()%counters.size()]; }
        static void add(std::atomic<std::uint64_t>& value, std::uint64_t amount=1) {
            value.fetch_add(amount,std::memory_order_relaxed);
        }
        // Hashed: measured faster than an ordered map under concurrent hits.
        std::unordered_map<std::uint32_t, CachedBlock> entries;
        std::list<std::uint32_t> lru;
        // Fixed-size lock striping bounds synchronization storage. IO/decode
        // never holds the cache lock; the same physical block cannot decode
        // twice concurrently. A stripe collision makes one miss wait for an
        // unrelated decode; with 32 stripes 8 threads collided often enough
        // to show in p99, 256 makes it rare.
        std::array<std::mutex, 256> loads;
        std::uint64_t bytes = 0;
        // The last evicted block nobody still pins, kept with its shared
        // control block, map node and list node, so a steady-state miss
        // allocates nothing.
        BlockBytes spare;
        std::unordered_map<std::uint32_t, CachedBlock>::node_type spare_node;
        std::list<std::uint32_t> spare_position;
        // Counters not in CounterSlot; written under the exclusive lock.
        CacheStats stats;
    };
    std::shared_ptr<BlockCache> block_cache_;
    std::vector<std::uint8_t> index_bytes_;
    Status check_cancelled() const {
        return options_.is_cancelled && options_.is_cancelled(options_.cancellation_context)
            ? fail(ErrorCode::cancelled,"archive operation cancelled") : ok_status();
    }
    RandomAccessSource* source_ = nullptr;
    OpenOptions options_{};
    bool mounted_ = false;
    std::uint64_t archive_size_ = 0, catalog_offset_ = 0, catalog_size_ = 0;
    std::uint64_t media_offset_ = 0, media_size_ = 0, data_offset_ = 0, data_size_ = 0;
    std::uint64_t manifest_offset_ = 0, manifest_size_ = 0, index_offset_ = 0, block_count_ = 0;
    std::uint64_t integrity_offset_ = 0, integrity_size_ = 0, generation_ = 0;
    std::uint64_t media_record_count_ = 0, media_payload_offset_ = 0;
    std::array<std::uint8_t, 32> index_digest_{}, manifest_digest_{};
    std::vector<std::uint8_t> catalog_bytes_, manifest_bytes_;
    std::uint64_t string_offset_ = 0, string_size_ = 0;
    std::vector<Application> applications_;
    std::vector<Program> programs_;
    std::vector<Snapshot> snapshots_;
    std::vector<SourceResource> source_resources_;
    std::uint16_t format_minor_ = 2;
    std::vector<LocalizedEntry> localized_;
    std::vector<std::uint32_t> snapshot_owner_;
    std::vector<std::uint8_t> media_expected_mask_;
    std::vector<std::uint32_t> media_use_count_;
    std::vector<Directory> directories_;
    std::vector<Entry> entries_;
    std::vector<File> files_;
    std::vector<std::uint32_t> block_refs_;
    std::vector<std::uint32_t> block_logical_sizes_;

    static Status ok_status() { return Status::success(true); }
    static Status fail_status(ErrorCode code, std::string message) { return Status::failure(code, std::move(message)); }
    Status fail(ErrorCode code, std::string message) const { return fail_status(code, std::move(message)); }

    static std::uint16_t u16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (std::uint16_t(p[1]) << 8)); }
    static std::uint32_t u32(const std::uint8_t* p) { return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24); }
    static std::uint64_t u64(const std::uint8_t* p) { return std::uint64_t(u32(p)) | (std::uint64_t(u32(p + 4)) << 32); }
    static bool add_ok(std::uint64_t a, std::uint64_t b, std::uint64_t& out) {
        if (b > std::numeric_limits<std::uint64_t>::max() - a) return false;
        out = a + b; return true;
    }
    static bool range_ok(std::uint64_t at, std::uint64_t length, std::uint64_t limit) {
        return at <= limit && length <= limit - at;
    }
    static std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }
    static bool all_zero(const std::uint8_t* p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) if (p[i]) return false;
        return true;
    }
    static std::uint32_t crc32c(const std::uint8_t* p, std::size_t n) { return detail::crc32c(p, n); }
    static bool utf8_valid(const std::uint8_t* p, std::size_t n) {
        std::size_t i = 0;
        while (i < n) {
            const std::uint8_t a = p[i++];
            if (a < 0x80) { if (a == 0) return false; continue; }
            std::uint32_t cp = 0; std::size_t extra = 0;
            if (a >= 0xc2 && a <= 0xdf) { cp = a & 0x1f; extra = 1; }
            else if (a >= 0xe0 && a <= 0xef) { cp = a & 0x0f; extra = 2; }
            else if (a >= 0xf0 && a <= 0xf4) { cp = a & 0x07; extra = 3; }
            else return false;
            if (extra > n - i) return false;
            for (std::size_t j = 0; j < extra; ++j) {
                const auto b = p[i++]; if ((b & 0xc0) != 0x80) return false;
                cp = (cp << 6) | (b & 0x3f);
            }
            if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
                (extra == 3 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        }
        return true;
    }

    bool read(std::uint64_t at, std::uint8_t* out, std::size_t n) const {
        return range_ok(at, n, archive_size_) && source_->read_at(at, out, n);
    }
    Status zero_range(std::uint64_t at, std::uint64_t n) const {
        std::array<std::uint8_t, 4096> bytes{};
        while (n) {
            const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(n, bytes.size()));
            if (!read(at, bytes.data(), amount)) return fail(ErrorCode::io, "source read failed while validating padding");
            if (!all_zero(bytes.data(), amount)) return fail(ErrorCode::format, "nonzero alignment padding");
            at += amount; n -= amount;
        }
        return ok_status();
    }
    std::string_view text(std::uint32_t off, std::uint32_t len) const noexcept {
        if (off == no_index || off > string_size_ || len > string_size_ - off) return {};
        return {reinterpret_cast<const char*>(catalog_bytes_.data() + string_offset_ + off), len};
    }
    std::uint32_t cat32(std::uint64_t off) const { return u32(catalog_bytes_.data() + off); }
    std::uint64_t cat64(std::uint64_t off) const { return u64(catalog_bytes_.data() + off); }
    std::uint32_t man32(std::uint64_t off) const { return u32(manifest_bytes_.data() + off); }
    std::uint64_t man64(std::uint64_t off) const { return u64(manifest_bytes_.data() + off); }

    Status open_impl();
    Status open_slot(const std::uint8_t* header, std::uint64_t physical);
    Status check_mount_limits() const;
    Status check_tail_digests() const;
    Status parse_catalog();
    Status mount_impl();
    Status parse_manifest();
    Status validate_snapshot_identities(std::uint64_t total_nodes, std::uint64_t total_paths);
    Status validate_media();
    Result<std::vector<std::uint8_t>> read_media_impl(std::uint32_t index);
    Status decode_block_record(const std::uint8_t* row, Block& block) const;
    Status block_record(std::uint32_t id, Block& block) const;
    // `verified`, when given, receives how many leading bytes passed before a mismatch.
    Status verify_slices(const Block& block, const std::uint8_t* bytes, std::size_t first_slice, std::size_t size,
                         std::size_t* verified = nullptr) const;
    Status read_cached_slice(std::uint32_t id, std::size_t offset, std::size_t wanted,
                             std::uint8_t* output, std::size_t& copied, ReadBuffer& stored) const;
    Status load_block(std::uint32_t id, bool verify, detail::LogicalBytes& logical,
                      const Block* known_record = nullptr, ReadBuffer* stored_scratch = nullptr) const;
    Status find_file(std::uint32_t snapshot_index, std::string_view path, std::uint32_t& file_id) const;
    std::uint32_t find_entry(std::uint32_t directory, std::string_view name) const;
    Status read_file_impl(std::uint32_t file_id, std::uint64_t offset,
                          std::size_t length, std::uint8_t* output);
    PartialRead failed_read(ErrorCode code, const char* message) {
        return PartialRead(fail_status(code, message), 0);
    }
    // Where read_range() writes. `caller`: the caller's buffer, one atomic
    // piece (single-block read_file). `staging`: private memory, so RAW
    // slices may be read and verified in place. `partial`: the caller's
    // buffer, committed block by block, with a RAW slice-prefix on mismatch.
    enum class Sink { caller, staging, partial };
    struct RawPiece { Block block; std::size_t in_chunk = 0, amount = 0, begin = 0, span = 0; };
    static constexpr std::size_t max_raw_run = 64;
    static constexpr std::size_t raw_run_bytes = 1024 * 1024;
    Status read_range(std::uint32_t file_id, std::uint64_t offset, std::size_t length,
                      std::uint8_t* destination, Sink sink, std::size_t& done);
};

inline Status Archive::parse_catalog() {
    if (catalog_bytes_.size()<128 || std::memcmp(catalog_bytes_.data(),"NXCT",4) || u16(catalog_bytes_.data()+4)!=1 ||
        u16(catalog_bytes_.data()+6)!=128 || u64(catalog_bytes_.data()+8)!=catalog_size_ || u32(catalog_bytes_.data()+16)!=0 ||
        u32(catalog_bytes_.data()+44)!=0 || !all_zero(catalog_bytes_.data()+112,16))
        return fail(ErrorCode::format,"invalid NxCatalogHeader");
    const std::uint32_t na=cat32(20), np=cat32(24), ns=cat32(28), nl=cat32(32), nd=cat32(36), ne=cat32(40);
    if (!na || na>4096 || np>16384 || !ns || ns>65536 || nl>16384 || nd>32768 || ne>4096)
        return fail(ErrorCode::resource_limit,"Catalog count outside version 0.2 limits");
    const std::uint64_t offsets[6]={cat64(48),cat64(56),cat64(64),cat64(72),cat64(80),cat64(88)};
    const std::uint64_t counts[6]={na,np,ns,nl,nd,ne};
    const std::uint64_t widths[6]={80,64,120,24,56,32};
    std::uint64_t cursor=128;
    for (int i=0;i<6;++i) {
        if (!counts[i]) { if (offsets[i]) return fail(ErrorCode::format,"empty Catalog table has nonzero offset"); continue; }
        const auto aligned=align_up(cursor,8);
        if (aligned>catalog_size_) return fail(ErrorCode::bounds,"Catalog table alignment exceeds section");
        if (aligned>cursor && !all_zero(catalog_bytes_.data()+cursor,static_cast<std::size_t>(aligned-cursor))) return fail(ErrorCode::format,"nonzero Catalog table padding");
        cursor=aligned;
        if (offsets[i]!=cursor || cursor>catalog_size_ || counts[i]>(catalog_size_-cursor)/widths[i]) return fail(ErrorCode::bounds,"Catalog table offset or size is invalid");
        cursor += counts[i]*widths[i];
    }
    const auto aligned_strings=align_up(cursor,8);
    if (aligned_strings>catalog_size_) return fail(ErrorCode::bounds,"Catalog string alignment exceeds section");
    if (aligned_strings>cursor && !all_zero(catalog_bytes_.data()+cursor,static_cast<std::size_t>(aligned_strings-cursor))) return fail(ErrorCode::format,"nonzero string-pool padding");
    cursor=aligned_strings;
    string_offset_=cat64(96); string_size_=cat64(104);
    if (string_offset_!=cursor || !range_ok(string_offset_,string_size_,catalog_size_)) return fail(ErrorCode::bounds,"Catalog string pool range is invalid");
    cursor=string_offset_+string_size_;

    auto string_ref=[&](std::uint32_t off,std::uint32_t len,std::uint32_t maximum,bool required)->bool {
        if (off==no_index) return len==0 && !required;
        if (!len || len>maximum || off>string_size_ || len>string_size_-off) return false;
        return utf8_valid(catalog_bytes_.data()+string_offset_+off,len);
    };
    applications_.reserve(na); programs_.reserve(np); snapshots_.reserve(ns); localized_.reserve(nl);
    snapshot_owner_.assign(ns, no_index);
    media_expected_mask_.assign(static_cast<std::size_t>(media_record_count_),0);
    media_use_count_.assign(static_cast<std::size_t>(media_record_count_),0);
    auto media_use=[&](std::uint32_t index,std::uint8_t mask)->bool {
        if (index==no_index) return true;
        if (index>=media_record_count_) return false;
        // Every owner must accept the actual Media record type.
        // A union incorrectly permits sharing an icon as NPDM/NACP.
        auto& allowed = media_expected_mask_[index];
        allowed = media_use_count_[index] ? static_cast<std::uint8_t>(allowed & mask) : mask;
        if (!allowed) return false;
        ++media_use_count_[index];
        return true;
    };

    std::tuple<std::uint8_t,std::uint64_t> prior_app{}; bool have_app=false;
    std::uint32_t app_program_cursor=0, app_snapshot_cursor=0, local_cursor=0;
    std::vector<std::uint32_t> localized_owner(nl,no_index);
    for (std::uint32_t i=0;i<na;++i) {
        const auto at=offsets[0]+std::uint64_t(i)*80;
        Application a;
        a.id=cat64(at); a.platform=catalog_bytes_[at+8]; a.flags=catalog_bytes_[at+9];
        if (u16(catalog_bytes_.data()+at+10)!=0 || !all_zero(catalog_bytes_.data()+at+72,8) || !a.id || a.platform>2 || (a.flags&~3))
            return fail(ErrorCode::format,"invalid NxApplication fields");
        const auto app_key=std::make_tuple(a.platform,a.id);
        if (have_app && !(prior_app<app_key)) return fail(ErrorCode::format,"application keys are not sorted and unique");
        prior_app=app_key; have_app=true;
        a.first_program=cat32(at+12); a.program_count=cat32(at+16); a.first_snapshot=cat32(at+20); a.snapshot_count=cat32(at+24);
        a.title_offset=cat32(at+28); a.title_length=cat32(at+32); a.publisher_offset=cat32(at+36); a.publisher_length=cat32(at+40);
        a.icon_index=cat32(at+44); a.first_localized=cat32(at+48); a.localized_count=cat32(at+52);
        a.nacp_index=cat32(at+56); a.first_opaque=cat32(at+60); a.opaque_count=cat32(at+64); a.metadata_snapshot=cat32(at+68);
        if (a.program_count) {
            if (a.first_program!=app_program_cursor || a.program_count>np-app_program_cursor) return fail(ErrorCode::format,"application program range is invalid");
            app_program_cursor+=a.program_count;
        } else if (a.first_program!=no_index) return fail(ErrorCode::format,"empty program range must use absent sentinel");
        if (a.first_snapshot!=app_snapshot_cursor || !a.snapshot_count || a.snapshot_count>ns-app_snapshot_cursor)
            return fail(ErrorCode::format,"application snapshot range is invalid");
        for (std::uint32_t j=a.first_snapshot;j<a.first_snapshot+a.snapshot_count;++j) snapshot_owner_[j]=i;
        app_snapshot_cursor+=a.snapshot_count;
        if (a.first_localized!=local_cursor || a.localized_count>nl-local_cursor) return fail(ErrorCode::format,"application localized range is invalid");
        for (std::uint32_t j=a.first_localized;j<a.first_localized+a.localized_count;++j) localized_owner[j]=i;
        local_cursor+=a.localized_count;
        if (!string_ref(a.title_offset,a.title_length,4096,false) || !string_ref(a.publisher_offset,a.publisher_length,4096,false))
            return fail(ErrorCode::format,"invalid application display string");
        if (!media_use(a.icon_index,(1u<<1)|(1u<<2)) || !media_use(a.nacp_index,(1u<<4))) return fail(ErrorCode::bounds,"application media reference is out of range");
        if (a.opaque_count) {
            if (a.first_opaque==no_index || a.first_opaque>media_record_count_ || a.opaque_count>media_record_count_-a.first_opaque)
                return fail(ErrorCode::bounds,"opaque metadata range is invalid");
            for (std::uint32_t j=0;j<a.opaque_count;++j) if (!media_use(a.first_opaque+j,1u<<5)) return fail(ErrorCode::bounds,"opaque media reference is invalid");
        } else if (a.first_opaque!=0 && a.first_opaque!=no_index) return fail(ErrorCode::format,"empty opaque range has invalid start");
        applications_.push_back(a);
    }
    if (app_program_cursor!=np || app_snapshot_cursor!=ns || local_cursor!=nl) return fail(ErrorCode::format,"Catalog application ranges do not cover their tables");

    std::tuple<std::uint32_t,std::uint64_t> prior_program{}; bool have_program=false;
    for (std::uint32_t i=0;i<np;++i) {
        const auto at=offsets[1]+std::uint64_t(i)*64;
        Program p; p.id=cat64(at); p.application_index=cat32(at+8); p.kind=catalog_bytes_[at+12]; p.flags=catalog_bytes_[at+13];
        if (u16(catalog_bytes_.data()+at+14)!=0 || !p.id || p.application_index>=na || p.kind>2 || (p.flags&~1))
            return fail(ErrorCode::format,"invalid NxProgram fields");
        std::copy(catalog_bytes_.begin()+at+16,catalog_bytes_.begin()+at+32,p.content_id.begin());
        if (bool(p.flags&1)!=(p.content_id!=std::array<std::uint8_t,16>{})) return fail(ErrorCode::format,"program content ID flag/value mismatch");
        p.first_snapshot=cat32(at+32); p.snapshot_count=cat32(at+36); p.npdm_index=cat32(at+40);
        if (cat32(at+44)!=0 || !all_zero(catalog_bytes_.data()+at+48,16)) return fail(ErrorCode::format,"nonzero NxProgram reserved field");
        const auto key=std::make_tuple(p.application_index,p.id);
        if (have_program && !(prior_program<key)) return fail(ErrorCode::format,"program rows are not sorted");
        prior_program=key; have_program=true;
        if (!media_use(p.npdm_index,1u<<3)) return fail(ErrorCode::bounds,"NPDM media reference is out of range");
        if (p.npdm_index!=no_index && p.snapshot_count!=1) return fail(ErrorCode::format,"legacy Program NPDM needs exactly one snapshot");
        programs_.push_back(p);
    }

    using SnapshotKey=std::tuple<std::uint32_t,std::uint32_t,std::uint8_t,std::uint32_t,std::array<std::uint8_t,16>,std::array<std::uint8_t,32>>;
    SnapshotKey prior_snapshot{}; bool have_snapshot=false; std::uint32_t dep_cursor=0;
    std::vector<std::array<std::uint8_t, 32>> identities;
    if (ns>1) identities.reserve(ns);
    for (std::uint32_t i=0;i<ns;++i) {
        const auto at=offsets[2]+std::uint64_t(i)*120;
        Snapshot s; s.program_index=cat32(at); s.type=catalog_bytes_[at+4]; s.flags=catalog_bytes_[at+5];
        if (u16(catalog_bytes_.data()+at+6)!=0 || s.type>4 || (s.flags&~3)) return fail(ErrorCode::format,"invalid NxSnapshot fields");
        s.numeric_version=cat32(at+8); s.root_directory=cat32(at+12); s.logical_size=cat64(at+16);
        s.display_version_offset=cat32(at+24); s.display_version_length=cat32(at+28);
        std::copy(catalog_bytes_.begin()+at+32,catalog_bytes_.begin()+at+48,s.content_id.begin());
        s.first_dependency=cat32(at+48); s.dependency_count=cat32(at+52);
        std::copy(catalog_bytes_.begin()+at+56,catalog_bytes_.begin()+at+88,s.identity.begin());
        std::copy(catalog_bytes_.begin()+at+88,catalog_bytes_.begin()+at+120,s.build_id.begin());
        if ((s.program_index==no_index && s.type!=2) || (s.program_index!=no_index && s.program_index>=np) || bool(s.flags&1)!=(s.build_id!=std::array<std::uint8_t,32>{}))
            return fail(ErrorCode::format,"snapshot program or Build ID association is invalid");
        if ((s.flags&2) && (s.type!=2 || s.program_index!=no_index || all_zero(s.content_id.data(),8) || !all_zero(s.content_id.data()+8,8)))
            return fail(ErrorCode::format,"add-on title ID needs an unassociated DLC");
        if (snapshot_owner_[i]>=na || !string_ref(s.display_version_offset,s.display_version_length,256,false)) return fail(ErrorCode::format,"snapshot owner or display version is invalid");
        if (s.first_dependency!=dep_cursor || s.dependency_count>nd-dep_cursor) return fail(ErrorCode::format,"snapshot dependency range is invalid");
        dep_cursor+=s.dependency_count;
        const std::uint32_t order_program=s.program_index==no_index?np:s.program_index;
        const auto key=std::make_tuple(snapshot_owner_[i],order_program,s.type,s.numeric_version,s.content_id,s.build_id);
        if (have_snapshot && !(prior_snapshot<key)) return fail(ErrorCode::format,"snapshot rows are not sorted");
        prior_snapshot=key; have_snapshot=true;
        if (all_zero(s.identity.data(), s.identity.size()))
            return fail(ErrorCode::format,"zero snapshot identity");
        if (ns>1) identities.push_back(s.identity);
        if (s.program_index!=no_index) s.npdm_index=programs_[s.program_index].npdm_index;
        snapshots_.push_back(s);
    }
    if (dep_cursor!=nd) return fail(ErrorCode::format,"snapshot ranges do not cover dependencies");
    if (ns>1) {
        std::sort(identities.begin(), identities.end());
        if (std::adjacent_find(identities.begin(), identities.end()) != identities.end())
            return fail(ErrorCode::format,"duplicate snapshot identity");
    }

    for (std::uint32_t ai=0;ai<na;++ai) {
        const auto& a=applications_[ai];
        std::uint32_t previous_program=0; bool have_owned=false, saw_unassociated=false, has_base=false, has_dlc=false;
        if (a.program_count) for (std::uint32_t pi=a.first_program;pi<a.first_program+a.program_count;++pi)
            if (programs_[pi].application_index!=ai) return fail(ErrorCode::format,"program is outside its application range");
        for (std::uint32_t si=a.first_snapshot;si<a.first_snapshot+a.snapshot_count;++si) {
            const auto& s=snapshots_[si]; has_base|=(s.type==0); has_dlc|=(s.type==2);
            if (s.program_index==no_index) { if (s.type!=2) return fail(ErrorCode::format,"only DLC may omit Program ID"); saw_unassociated=true; }
            else {
                if (saw_unassociated || s.program_index<a.first_program || s.program_index>=a.first_program+a.program_count || programs_[s.program_index].application_index!=ai)
                    return fail(ErrorCode::format,"snapshot/program/application association is invalid");
                if (have_owned && s.program_index<previous_program) return fail(ErrorCode::format,"snapshot program groups are not ordered");
                previous_program=s.program_index; have_owned=true;
            }
        }
        if (bool(a.flags&1)!=has_base || bool(a.flags&2)!=has_dlc) return fail(ErrorCode::format,"application base/DLC flags disagree with snapshots");
        // Even opaque native bytes need an exact source snapshot. Conversely,
        // a source pointer with no metadata is a misleading provenance claim.
        const bool has_native_metadata = a.title_offset!=no_index || a.publisher_offset!=no_index ||
            a.icon_index!=no_index || a.nacp_index!=no_index || a.localized_count || a.opaque_count;
        if (a.metadata_snapshot!=no_index) {
            if (a.metadata_snapshot<a.first_snapshot || a.metadata_snapshot>=a.first_snapshot+a.snapshot_count || snapshots_[a.metadata_snapshot].type==2)
                return fail(ErrorCode::format,"metadata source snapshot is not an eligible snapshot of this application");
            if (!has_native_metadata) return fail(ErrorCode::format,"metadata source snapshot has no native metadata");
            snapshots_[a.metadata_snapshot].nacp_index=a.nacp_index;
        } else if (has_native_metadata)
            return fail(ErrorCode::format,"native metadata exists without a source snapshot");
    }
    std::vector<std::uint8_t> program_snapshot_seen(ns,0);
    for (std::uint32_t pi=0;pi<np;++pi) {
        const auto& p=programs_[pi];
        if (p.first_snapshot>=ns || !p.snapshot_count || p.snapshot_count>ns-p.first_snapshot) return fail(ErrorCode::format,"program snapshot range is invalid");
        for (std::uint32_t si=p.first_snapshot;si<p.first_snapshot+p.snapshot_count;++si) {
            if (snapshots_[si].program_index!=pi || program_snapshot_seen[si]) return fail(ErrorCode::format,"program snapshot range is not an exact association");
            program_snapshot_seen[si]=1;
        }
    }
    for (std::uint32_t si=0;si<ns;++si)
        if ((snapshots_[si].program_index!=no_index)!=bool(program_snapshot_seen[si])) return fail(ErrorCode::format,"program snapshot table has an unassociated row");

    std::uint32_t prior_owner=no_index; std::string_view prior_language{};
    for (std::uint32_t i=0;i<nl;++i) {
        const auto at=offsets[3]+std::uint64_t(i)*24;
        LocalizedEntry l{cat32(at),cat32(at+4),cat32(at+8),cat32(at+12),cat32(at+16)};
        if (cat32(at+20)!=0 || !string_ref(l.language_offset,l.language_length,63,true) || !string_ref(l.title_offset,l.title_length,4096,false) ||
            (l.title_offset==no_index && l.icon_index==no_index)) return fail(ErrorCode::format,"invalid localized entry");
        const auto lang=text(l.language_offset,l.language_length);
        bool segment_start=true; std::size_t segment=0;
        for (unsigned char c:lang) {
            if (c=='-') { if (segment_start || segment>8) return fail(ErrorCode::format,"invalid language tag"); segment=0; segment_start=true; }
            else if ((c>='a'&&c<='z')||(c>='0'&&c<='9')) { segment_start=false; ++segment; }
            else return fail(ErrorCode::format,"language tag is not lowercase ASCII BCP 47");
        }
        if (segment_start || segment>8) return fail(ErrorCode::format,"invalid language tag segment");
        const auto owner=localized_owner[i];
        if (owner==no_index || (owner==prior_owner && !(prior_language<lang))) return fail(ErrorCode::format,"localized languages are duplicate or unsorted");
        if (owner!=prior_owner) prior_language={};
        prior_owner=owner; prior_language=lang;
        if (!media_use(l.icon_index,(1u<<1)|(1u<<2))) return fail(ErrorCode::bounds,"localized icon index is out of range");
        localized_.push_back(l);
    }

    for (std::uint32_t i=0;i<nd;++i) {
        const auto at=offsets[4]+std::uint64_t(i)*56;
        const auto kind=catalog_bytes_[at], flags=catalog_bytes_[at+1];
        if (kind<1 || kind>3 || (flags&~1) || u16(catalog_bytes_.data()+at+2)!=0 || !all_zero(catalog_bytes_.data()+at+52,4))
            return fail(ErrorCode::format,"invalid dependency record");
        if (bool(flags&1)!=!all_zero(catalog_bytes_.data()+at+4,16) || all_zero(catalog_bytes_.data()+at+20,32))
            return fail(ErrorCode::format,"dependency ID or target identity is invalid");
    }
    std::uint64_t extension_cursor=string_offset_+string_size_;
    std::tuple<std::uint32_t,std::uint16_t,std::uint32_t> prior_extension{}; bool have_extension=false;
    for (std::uint32_t i=0;i<ne;++i) {
        const auto at=offsets[5]+std::uint64_t(i)*32;
        const auto type=cat32(at); const auto flags=cat32(at+4);
        const auto owner=u16(catalog_bytes_.data()+at+8); const auto reserved=u16(catalog_bytes_.data()+at+10);
        const auto owner_index=cat32(at+12); const auto data_off=cat64(at+16); const auto data_size=cat64(at+24);
        if ((flags&~1) || owner>3 || reserved || (owner==0 && owner_index!=no_index) ||
            (owner==1 && owner_index>=na) || (owner==2 && owner_index>=np) || (owner==3 && owner_index>=ns))
            return fail(ErrorCode::format,"invalid extension descriptor");
        const auto key=std::make_tuple(type,owner,owner_index);
        if (have_extension && !(prior_extension<key)) return fail(ErrorCode::format,"extensions are duplicate or unsorted");
        prior_extension=key; have_extension=true;
        const auto aligned=align_up(extension_cursor,8);
        if (aligned>catalog_size_) return fail(ErrorCode::bounds,"Catalog extension alignment exceeds section");
        if (aligned>extension_cursor && !all_zero(catalog_bytes_.data()+extension_cursor,static_cast<std::size_t>(aligned-extension_cursor))) return fail(ErrorCode::format,"nonzero extension padding");
        extension_cursor=aligned;
        if (data_off!=extension_cursor || !range_ok(data_off,data_size,catalog_size_)) return fail(ErrorCode::bounds,"extension payload range is invalid");
        if (type>=0x00010001 && type<=0x00010003 &&
            (owner!=1 || applications_[owner_index].metadata_snapshot==no_index))
            return fail(ErrorCode::format,"application discovery extension needs an exact metadata source");
        if (type==0x00010001 && (owner!=1 || data_size!=4)) return fail(ErrorCode::format,"invalid firmware extension");
        if (type==0x00010002 && (owner!=1 || data_size<3)) return fail(ErrorCode::format,"invalid supported-language extension");
        if (type==0x00010003 && (owner!=1 || data_size<4)) return fail(ErrorCode::format,"invalid regions extension");
        if (type==0x00010008) {
            if (owner!=1 || flags!=1 || data_size<16) return fail(ErrorCode::format,"invalid source resource descriptor");
            const auto* p=catalog_bytes_.data()+data_off;
            const auto count=u32(p+8);
            if (std::memcmp(p,"NXSR",4) || u16(p+4)!=1 || u16(p+6)!=16 || u32(p+12) || !count ||
                count>4096 || data_size!=16ull+8ull*count || source_resources_.size()+count>4096)
                return fail(ErrorCode::format,"invalid source resource table");
            std::pair<std::uint32_t,std::uint32_t> prior{};
            for (std::uint32_t j=0;j<count;++j) {
                const auto kind=u32(p+16+j*8), root=u32(p+20+j*8);
                const auto row=std::make_pair(kind,root);
                if (kind>1 || (j && !(prior<row))) return fail(ErrorCode::format,"invalid or unsorted source resource");
                source_resources_.push_back({owner_index,kind,root}); prior=row;
            }
        }
        if (type==0x00010005) {
            if (owner!=1 || flags!=1 || data_size<16) return fail(ErrorCode::format,"invalid Snapshot Native Metadata descriptor");
            const auto* p=catalog_bytes_.data()+data_off;
            const auto count=u32(p+8);
            if (std::memcmp(p,"NXNM",4) || u16(p+4)!=1 || u16(p+6)!=16 || u32(p+12) ||
                !count || count>applications_[owner_index].snapshot_count || data_size!=16ull+std::uint64_t(count)*16)
                return fail(ErrorCode::format,"invalid Snapshot Native Metadata header");
            std::uint32_t previous=no_index;
            for (std::uint32_t j=0;j<count;++j) {
                const auto* row=p+16+std::uint64_t(j)*16;
                const auto si=u32(row), npdm=u32(row+4), firmware=u32(row+8), row_flags=u32(row+12);
                if (si>=snapshots_.size() || snapshot_owner_[si]!=owner_index || (j && si<=previous) ||
                    (row_flags&~1u) || (!(row_flags&1) && firmware) || (npdm==no_index && !(row_flags&1)))
                    return fail(ErrorCode::format,"invalid Snapshot Native Metadata row");
                auto& snapshot=snapshots_[si];
                if (npdm!=no_index && (snapshot.program_index==no_index || programs_[snapshot.program_index].npdm_index!=no_index))
                    return fail(ErrorCode::format,"ambiguous or unassociated snapshot NPDM");
                if (!media_use(npdm,1u<<3)) return fail(ErrorCode::bounds,"snapshot NPDM media index out of range");
                if (npdm!=no_index) snapshot.npdm_index=npdm;
                if (row_flags&1) snapshot.required_firmware=firmware;
                previous=si;
            }
        }
        if (type==0x00010004) {
            if (owner!=1 || flags!=1 || data_size<24) return fail(ErrorCode::format,"invalid Snapshot Metadata History extension descriptor");
            const auto* payload=catalog_bytes_.data()+data_off;
            if (std::memcmp(payload,"NXSM",4)!=0 || u16(payload+4)!=1 || u16(payload+6)!=24 || u32(payload+20)!=0)
                return fail(ErrorCode::format,"invalid Snapshot Metadata History header");
            const auto& app=applications_[owner_index];
            const auto records=u32(payload+8), locales=u32(payload+12), opaque=u32(payload+16);
            const std::uint64_t expected_size=24ull+std::uint64_t(records)*48ull+std::uint64_t(locales)*24ull+std::uint64_t(opaque)*4ull;
            if (!records || app.metadata_snapshot==no_index || app.snapshot_count<2 || records>app.snapshot_count-1 || locales>16384 || expected_size!=data_size)
                return fail(ErrorCode::bounds,"Snapshot Metadata History counts or size are invalid");
            const std::uint64_t record_base=data_off+24;
            const std::uint64_t locale_base=record_base+std::uint64_t(records)*48ull;
            const std::uint64_t opaque_base=locale_base+std::uint64_t(locales)*24ull;
            std::uint32_t locale_cursor=0, opaque_cursor=0, history_prior_snapshot=no_index;
            for (std::uint32_t ri=0;ri<records;++ri) {
                const auto row=record_base+std::uint64_t(ri)*48ull;
                const auto snapshot_index=u32(catalog_bytes_.data()+row);
                const auto title_off=u32(catalog_bytes_.data()+row+4), title_len=u32(catalog_bytes_.data()+row+8);
                const auto publisher_off=u32(catalog_bytes_.data()+row+12), publisher_len=u32(catalog_bytes_.data()+row+16);
                const auto icon_index=u32(catalog_bytes_.data()+row+20), nacp_index=u32(catalog_bytes_.data()+row+24);
                const auto first_local=u32(catalog_bytes_.data()+row+28), local_count=u32(catalog_bytes_.data()+row+32);
                const auto first_opaque=u32(catalog_bytes_.data()+row+36), opaque_count=u32(catalog_bytes_.data()+row+40);
                if (u32(catalog_bytes_.data()+row+44)!=0 || snapshot_index<app.first_snapshot || snapshot_index>=app.first_snapshot+app.snapshot_count ||
                    snapshot_index==app.metadata_snapshot || (ri && snapshot_index<=history_prior_snapshot) ||
                    !string_ref(title_off,title_len,4096,false) || !string_ref(publisher_off,publisher_len,4096,false) ||
                    first_local!=locale_cursor || local_count>locales-locale_cursor || first_opaque!=opaque_cursor || opaque_count>opaque-opaque_cursor ||
                    (title_off==no_index && publisher_off==no_index && icon_index==no_index && nacp_index==no_index && !local_count && !opaque_count))
                    return fail(ErrorCode::format,"invalid or misassociated snapshot metadata row");
                snapshots_[snapshot_index].nacp_index=nacp_index;
                history_prior_snapshot=snapshot_index;
                locale_cursor+=local_count;
                opaque_cursor+=opaque_count;
                if (!media_use(icon_index,(1u<<1)|(1u<<2)) || !media_use(nacp_index,1u<<4))
                    return fail(ErrorCode::bounds,"snapshot metadata Media reference is out of range");
                std::string_view history_prior_language{};
                for (std::uint32_t li=0;li<local_count;++li) {
                    const auto local=locale_base+std::uint64_t(first_local+li)*24ull;
                    const auto lang_off=u32(catalog_bytes_.data()+local), lang_len=u32(catalog_bytes_.data()+local+4);
                    const auto local_title_off=u32(catalog_bytes_.data()+local+8), local_title_len=u32(catalog_bytes_.data()+local+12);
                    const auto local_icon=u32(catalog_bytes_.data()+local+16);
                    if (u32(catalog_bytes_.data()+local+20)!=0 || !string_ref(lang_off,lang_len,63,true) ||
                        !string_ref(local_title_off,local_title_len,4096,false) || (local_title_off==no_index && local_icon==no_index))
                        return fail(ErrorCode::format,"invalid historical localized metadata entry");
                    const auto language=text(lang_off,lang_len);
                    bool segment_start=true; std::size_t segment=0;
                    for (unsigned char c:language) {
                        if (c=='-') { if (segment_start || segment>8) return fail(ErrorCode::format,"invalid historical language tag"); segment=0; segment_start=true; }
                        else if ((c>='a'&&c<='z')||(c>='0'&&c<='9')) { segment_start=false; ++segment; }
                        else return fail(ErrorCode::format,"invalid historical language tag");
                    }
                    if (segment_start || segment>8 || (li && !(history_prior_language<language)))
                        return fail(ErrorCode::format,"historical language tags are invalid or unsorted");
                    history_prior_language=language;
                    if (!media_use(local_icon,(1u<<1)|(1u<<2))) return fail(ErrorCode::bounds,"historical localized icon index is out of range");
                }
                for (std::uint32_t oi=0;oi<opaque_count;++oi) {
                    const auto media_index=u32(catalog_bytes_.data()+opaque_base+std::uint64_t(first_opaque+oi)*4ull);
                    if (!media_use(media_index,1u<<5)) return fail(ErrorCode::bounds,"historical opaque media index is out of range");
                }
            }
            if (locale_cursor!=locales || opaque_cursor!=opaque) return fail(ErrorCode::format,"snapshot metadata ranges do not cover payload arrays");
        }
        if ((flags&1) && type!=0x00010001 && type!=0x00010002 && type!=0x00010003 && type!=0x00010004 && type!=0x00010005 && type!=0x00010008) return fail(ErrorCode::unsupported,"unknown required Catalog extension");
        if (type==0x00010002) {
            const auto* payload=catalog_bytes_.data()+data_off;
            const auto languages=u16(payload);
            if (!languages || languages>64) return fail(ErrorCode::format,"supported-language count is invalid");
            std::uint64_t p=2; std::string_view previous{};
            for (std::uint16_t j=0;j<languages;++j) {
                if (p>=data_size) return fail(ErrorCode::bounds,"truncated language extension");
                const auto n=payload[p++];
                if (!n || n>63 || n>data_size-p) return fail(ErrorCode::bounds,"language extension tag length is invalid");
                const std::string_view language(reinterpret_cast<const char*>(payload+p),n);
                bool segment_start=true; std::size_t segment=0;
                for (unsigned char c:language) {
                    if (c=='-') { if (segment_start || segment>8) return fail(ErrorCode::format,"language extension tag is invalid"); segment=0; segment_start=true; }
                    else if ((c>='a'&&c<='z')||(c>='0'&&c<='9')) { segment_start=false; ++segment; }
                    else return fail(ErrorCode::format,"language extension tag is invalid");
                }
                if (segment_start || segment>8 || (j && !(previous<language))) return fail(ErrorCode::format,"language extension tags are invalid or unsorted");
                previous=language; p+=n;
            }
            if (p!=data_size) return fail(ErrorCode::format,"language extension has trailing bytes");
        }
        if (type==0x00010003) {
            const auto* payload=catalog_bytes_.data()+data_off;
            const auto regions=u16(payload);
            if (!regions || regions>249 || data_size!=2ull+std::uint64_t(regions)*2) return fail(ErrorCode::format,"regions extension count or size is invalid");
            std::array<std::uint8_t,2> prior{};
            for (std::uint16_t j=0;j<regions;++j) {
                const std::array<std::uint8_t,2> code={payload[2+2*j],payload[3+2*j]};
                if (code[0]<'A'||code[0]>'Z'||code[1]<'A'||code[1]>'Z'||(j && !(prior<code)))
                    return fail(ErrorCode::format,"region codes are invalid or unsorted");
                prior=code;
            }
        }
        extension_cursor=data_off+data_size;
    }
    if ((format_minor_==3) != !source_resources_.empty()) return fail(ErrorCode::format,"format version disagrees with source resources");
    if (extension_cursor!=catalog_size_) return fail(ErrorCode::format,"Catalog has unaccounted trailing bytes");
    return ok_status();
}

inline Status Archive::open_impl() {
    constexpr std::size_t page_size = 4096;
    const auto physical = source_->size();
    if (physical < 2*page_size) return fail(ErrorCode::bounds, "archive is shorter than its header area");
    if (physical > 0x7fffffffffffffffull) return fail(ErrorCode::resource_limit, "archive exceeds INT64_MAX");
    std::array<std::uint8_t, 2*page_size> area{};
    bool readable[2] = {true, true};
    if (!source_->read_at(0, area.data(), area.size())) {
        for (std::size_t slot = 0; slot < 2; ++slot)
            readable[slot] = source_->read_at(slot*page_size, area.data()+slot*page_size, page_size);
        if (!readable[0] && !readable[1]) return fail(ErrorCode::io, "failed to read either header page");
    }
    // Two header slots: the valid one with the highest generation wins.
    const std::uint8_t* chosen = nullptr;
    const std::uint8_t* older = nullptr;
    bool any_magic = false;
    for (std::size_t slot = 0; slot < 2; ++slot) {
        if (!readable[slot]) continue;
        const std::uint8_t* candidate = area.data() + slot*page_size;
        if (std::memcmp(candidate, "NXAR", 4) != 0) continue;
        any_magic = true;
        if (crc32c(candidate, 252) != u32(candidate+252) || !all_zero(candidate+256, page_size-256)) continue;
        if (!chosen) chosen = candidate;
        else if (u64(candidate+216) > u64(chosen+216)) {
            older = chosen;
            chosen = candidate;
        } else if (u64(candidate+216) < u64(chosen+216)) older = candidate;
        else if (std::memcmp(candidate, chosen, 256) != 0)
            return fail(ErrorCode::format, "both header slots are valid with the same generation but differ");
    }
    if (!chosen) return any_magic ? fail(ErrorCode::integrity, "no header page has a valid CRC-32C and padding")
                                  : fail(ErrorCode::unsupported, "unsupported NX identity or version");
    Status st = open_slot(chosen, physical);
    if (!older) return st;
    // Resolve recovery before exposing Catalog indices to callers. Ordinary
    // single-generation archives still defer Manifest and index I/O to mount().
    if (st) st = check_tail_digests();
    // Only damage (format/integrity/bounds) falls back: an I/O error, a limit
    // or a cancellation says nothing about the newer generation's validity.
    if (st || st.error.code == ErrorCode::unsupported || st.error.code == ErrorCode::io ||
        st.error.code == ErrorCode::resource_limit || st.error.code == ErrorCode::cancelled)
        return st;

    Archive previous;
    previous.source_ = source_;
    previous.options_ = options_;
    auto recovered = previous.open_slot(older, physical);
    if (recovered) recovered = previous.check_tail_digests();
    if (!recovered) return st;
    *this = std::move(previous);
    return ok_status();
}

inline Status Archive::open_slot(const std::uint8_t* header, std::uint64_t physical) {
    std::array<std::uint8_t, 256> h{};
    std::memcpy(h.data(), header, h.size());
    if (u16(h.data()+4) != 0 || (u16(h.data()+6) != 2 && u16(h.data()+6) != 3) || u32(h.data()+8) != 256)
        return fail(ErrorCode::unsupported, "unsupported NX identity or version");
    if (u16(h.data()+12) != 1 || u16(h.data()+14) != 0) return fail(ErrorCode::unsupported, "unsupported NX header layout or feature bits");
    if (u32(h.data()+224) != check_size || !all_zero(h.data()+228, 24))
        return fail(ErrorCode::format, "invalid check size or nonzero reserved header bytes");
    format_minor_ = u16(h.data()+6);
    generation_ = u64(h.data()+216);
    const auto declared_size = u64(h.data()+16);
    catalog_offset_ = u64(h.data()+24); catalog_size_ = u64(h.data()+32);
    media_offset_ = u64(h.data()+40); media_size_ = u64(h.data()+48);
    data_offset_ = u64(h.data()+56); data_size_ = u64(h.data()+64);
    manifest_offset_ = u64(h.data()+72); manifest_size_ = u64(h.data()+80);
    index_offset_ = u64(h.data()+88); block_count_ = u64(h.data()+96);
    integrity_offset_ = u64(h.data()+104); integrity_size_ = u64(h.data()+112);
    std::copy(h.begin()+184, h.begin()+216, index_digest_.begin());
    std::copy(h.begin()+152, h.begin()+184, manifest_digest_.begin());
    // Bytes after archive_size are residue of an interrupted edit; ignore them.
    if (declared_size > physical) return fail(ErrorCode::bounds, "declared archive size exceeds the physical file");
    archive_size_ = declared_size;
    if (catalog_size_ < 128 || catalog_size_ > 16ull*1024*1024 || !range_ok(catalog_offset_, catalog_size_, archive_size_))
        return fail(ErrorCode::bounds, "Catalog range or size is invalid");
    if (manifest_size_ < 96 || manifest_size_ > 4ull*1024*1024*1024 || !range_ok(manifest_offset_, manifest_size_, archive_size_))
        return fail(ErrorCode::bounds, "Manifest range or size is invalid");
    if (block_count_ > (1ull<<32) || integrity_size_ % 4)
        return fail(ErrorCode::format, "block count and integrity size disagree");
    if (block_count_ == 0) {
        if (index_offset_ || integrity_offset_ || integrity_size_ || data_offset_ || data_size_)
            return fail(ErrorCode::format, "empty block tables disagree with Data fields");
    } else if (data_offset_ != 8192 || !index_offset_ || !integrity_offset_ || !data_size_ || !integrity_size_ ||
               !range_ok(data_offset_, data_size_, archive_size_)) {
        return fail(ErrorCode::format, "nonempty block table is missing a region");
    }
    if (media_size_ && (media_size_ < 32 || media_size_ > 4ull*1024*1024*1024))
        return fail(ErrorCode::format, "Media region offset or size is invalid");
    if (!media_size_ && media_offset_) return fail(ErrorCode::format, "absent Media region has nonzero offset");

    // Tail: [Media] Catalog Manifest [index integrity], packed on 16 bytes.
    // The first region may follow free space left by in-place edits.
    struct Region { std::uint64_t offset, size; const char* name; };
    Region tail[5]; std::size_t regions = 0;
    if (media_size_) tail[regions++] = {media_offset_, media_size_, "Media region"};
    tail[regions++] = {catalog_offset_, catalog_size_, "Catalog"};
    tail[regions++] = {manifest_offset_, manifest_size_, "Manifest"};
    if (block_count_) {
        tail[regions++] = {index_offset_, block_count_*24ull, "hot block index"};
        tail[regions++] = {integrity_offset_, integrity_size_, "integrity table"};
    }
    const auto data_end = block_count_ ? data_offset_ + data_size_ : 8192;
    if (tail[0].offset % 16 || tail[0].offset < align_up(data_end, 16))
        return fail(ErrorCode::format, "archive tail does not start after the Data region");
    std::uint64_t cursor = tail[0].offset;
    for (std::size_t i = 0; i < regions; ++i) {
        const auto& r = tail[i];
        if (i && r.offset != align_up(cursor, 16))
            return fail(ErrorCode::format, std::string(r.name) + " is not at its packed tail offset");
        if (!range_ok(r.offset, r.size, archive_size_)) return fail(ErrorCode::bounds, std::string(r.name) + " range is invalid");
        Status st = zero_range(cursor, r.offset - cursor); if (!st) return st;
        cursor = r.offset + r.size;
    }
    if (cursor != archive_size_) return fail(ErrorCode::bounds, "archive has trailing or unaccounted bytes");

    if (media_size_) {
        std::array<std::uint8_t, 32> mh{};
        if (!read(media_offset_, mh.data(), mh.size())) return fail(ErrorCode::io, "failed to read Media header");
        media_record_count_ = u32(mh.data()+16);
        media_payload_offset_ = u32(mh.data()+28);
        if (std::memcmp(mh.data(), "NXMD", 4) || u16(mh.data()+4)!=1 || u16(mh.data()+6)!=32 ||
            u64(mh.data()+8)!=media_size_ || !media_record_count_ || media_record_count_>65535 ||
            u16(mh.data()+20)!=64 || u16(mh.data()+22)!=0 || u32(mh.data()+24)!=32 ||
            media_payload_offset_!=align_up(32+media_record_count_*64,16) ||
            media_payload_offset_>media_size_)
            return fail(ErrorCode::format, "invalid Media region header");
    }

    catalog_bytes_.resize(static_cast<std::size_t>(catalog_size_));
    if (!read(catalog_offset_, catalog_bytes_.data(), catalog_bytes_.size())) return fail(ErrorCode::io, "failed to read Catalog");
    std::array<std::uint8_t,32> catalog_digest{};
    std::copy(h.begin()+120,h.begin()+152,catalog_digest.begin());
    if (Sha256::digest(catalog_bytes_.data(), catalog_bytes_.size()) != catalog_digest)
        return fail(ErrorCode::integrity, "Catalog SHA-256 mismatch");
    return parse_catalog();
}

inline Status Archive::parse_manifest() {
    if (manifest_bytes_.size()<96 || std::memcmp(manifest_bytes_.data(),"NXMF",4) || u16(manifest_bytes_.data()+4)!=1 ||
        u16(manifest_bytes_.data()+6)!=96 || u64(manifest_bytes_.data()+8)!=manifest_size_ || !all_zero(manifest_bytes_.data()+80,16))
        return fail(ErrorCode::format,"invalid NxManifestHeader");
    const std::uint32_t nd=man32(16), nf=man32(20), ne=man32(24), nr=man32(28);
    const std::uint64_t off[4]={man64(32),man64(40),man64(48),man64(56)};
    const std::uint64_t count[4]={nd,nf,ne,nr}, width[4]={16,64,24,4}, alignment[4]={8,8,8,4};
    if (!nd || nd>16'777'216 || nf>33'554'432 || ne>50'331'648 || nr>0xffffffffu || man64(72)>2ull*1024*1024*1024)
        return fail(ErrorCode::resource_limit,"Manifest count or path pool exceeds format limits");
    if (nd>options_.max_directories || nf>options_.max_files || ne>options_.max_entries || nr>options_.max_block_references)
        return fail(ErrorCode::resource_limit,"Manifest count exceeds configured mount budget");
    std::uint64_t cursor=96;
    for (int i=0;i<4;++i) {
        if (!count[i]) { if (off[i]) return fail(ErrorCode::format,"empty Manifest table has nonzero offset"); continue; }
        const auto aligned=align_up(cursor,alignment[i]);
        if (aligned>manifest_size_) return fail(ErrorCode::bounds,"Manifest table alignment exceeds section");
        if (aligned>cursor && !all_zero(manifest_bytes_.data()+cursor,static_cast<std::size_t>(aligned-cursor))) return fail(ErrorCode::format,"nonzero Manifest table padding");
        cursor=aligned;
        if (off[i]!=cursor || cursor>manifest_size_ || count[i]>(manifest_size_-cursor)/width[i]) return fail(ErrorCode::bounds,"Manifest table offset or range is invalid");
        cursor+=count[i]*width[i];
    }
    const auto aligned_path=align_up(cursor,8);
    if (aligned_path>manifest_size_) return fail(ErrorCode::bounds,"Manifest path alignment exceeds section");
    if (aligned_path>cursor && !all_zero(manifest_bytes_.data()+cursor,static_cast<std::size_t>(aligned_path-cursor))) return fail(ErrorCode::format,"nonzero path-pool padding");
    cursor=aligned_path;
    const std::uint64_t path_off=man64(64), path_size=man64(72);
    if (path_off!=cursor || !range_ok(path_off,path_size,manifest_size_) || path_off+path_size!=manifest_size_)
        return fail(ErrorCode::bounds,"Manifest path pool range or trailing bytes are invalid");

    directories_.reserve(nd); entries_.reserve(ne); files_.reserve(nf); block_refs_.reserve(nr);
    std::uint64_t entry_cursor=0;
    for (std::uint32_t i=0;i<nd;++i) {
        const auto at=off[0]+std::uint64_t(i)*16;
        const auto first=man32(at), n=man32(at+4), flags=man32(at+8), reserved=man32(at+12);
        if (first!=entry_cursor || n>ne-entry_cursor || flags || reserved) return fail(ErrorCode::format,"invalid directory record or entry range");
        directories_.push_back({first,n});
        std::string_view prior{}; bool have_prior=false;
        for (std::uint32_t j=first;j<first+n;++j) {
            const auto e_at=off[2]+std::uint64_t(j)*24;
            Entry e; e.name_offset=man64(e_at); e.name_length=man32(e_at+8); e.target=man32(e_at+12); e.kind=manifest_bytes_[e_at+16];
            if (e.kind<1 || e.kind>2 || manifest_bytes_[e_at+17] || u16(manifest_bytes_.data()+e_at+18) || man32(e_at+20) || !e.name_length || e.name_length>4096)
                return fail(ErrorCode::format,"invalid directory entry descriptor");
            const std::uint64_t pool_end=path_off+path_size;
            if (e.name_offset<path_off || e.name_offset>pool_end || e.name_length>pool_end-e.name_offset)
                return fail(ErrorCode::bounds,"path component lies outside path pool");
            const auto* name=manifest_bytes_.data()+e.name_offset;
            if (!utf8_valid(name,e.name_length)) return fail(ErrorCode::format,"path component is invalid UTF-8 or contains NUL");
            for (std::uint32_t k=0;k<e.name_length;++k) if (name[k]=='/' || name[k]=='\\') return fail(ErrorCode::format,"path component contains a separator");
            if ((e.name_length==1 && name[0]=='.') || (e.name_length==2 && name[0]=='.' && name[1]=='.')) return fail(ErrorCode::format,"dot path component is forbidden");
            const std::string_view current(reinterpret_cast<const char*>(name),e.name_length);
            auto byte_less=[](std::string_view a,std::string_view b) {
                return std::lexicographical_compare(a.begin(),a.end(),b.begin(),b.end(),[](char x,char y){return static_cast<unsigned char>(x)<static_cast<unsigned char>(y);});
            };
            if (have_prior && !byte_less(prior,current)) return fail(ErrorCode::format,"directory entries are duplicate or unsorted");
            prior=current; have_prior=true;
            if ((e.kind==1 && e.target>=nd) || (e.kind==2 && e.target>=nf)) return fail(ErrorCode::bounds,"directory entry target is out of range");
            entries_.push_back(e);
        }
        entry_cursor+=n;
    }
    if (entry_cursor!=ne) return fail(ErrorCode::format,"directory ranges do not cover entry table");

    std::uint64_t ref_cursor=0;
    for (std::uint32_t i=0;i<nf;++i) {
        const auto at=off[1]+std::uint64_t(i)*64;
        File f; f.size=man64(at); f.first=man32(at+8); f.count=man32(at+12); f.chunk_log2=manifest_bytes_[at+16];
        if (f.size>0x7fffffffffffffffull || (f.chunk_log2<16 || f.chunk_log2>20) || manifest_bytes_[at+17] || u16(manifest_bytes_.data()+at+18) || !all_zero(manifest_bytes_.data()+at+52,12))
            return fail(ErrorCode::format,"invalid file record");
        std::copy(manifest_bytes_.begin()+at+20,manifest_bytes_.begin()+at+52,f.digest.begin());
        const std::uint64_t chunk=1ull<<f.chunk_log2;
        const std::uint64_t expected=f.size/chunk+(f.size%chunk?1:0);
        if (f.count!=expected || f.first!=ref_cursor || f.count>nr-ref_cursor) return fail(ErrorCode::format,"file block-reference range/count mismatch");
        files_.push_back(f); ref_cursor+=f.count;
    }
    if (ref_cursor!=nr) return fail(ErrorCode::format,"file records do not cover block-reference table");
    for (std::uint32_t i=0;i<nr;++i) {
        const auto id=man32(off[3]+std::uint64_t(i)*4);
        if (id>=block_count_) return fail(ErrorCode::bounds,"block reference names a missing block");
        block_refs_.push_back(id);
    }

    if (block_count_>options_.max_blocks) return fail(ErrorCode::resource_limit,"block count exceeds configured mount budget");
    block_logical_sizes_.reserve(static_cast<std::size_t>(block_count_));
    std::uint64_t previous_end=data_offset_, next_check=0;
    std::array<std::uint8_t,64*1024> descriptor_batch{};
    std::uint64_t block_cursor=0;
    constexpr std::uint64_t descriptors_per_batch=descriptor_batch.size()/24;
    while (block_cursor<block_count_) {
        const auto descriptor_count=std::min<std::uint64_t>(descriptors_per_batch,block_count_-block_cursor);
        const auto byte_count=static_cast<std::size_t>(descriptor_count*24);
        auto cancel=check_cancelled(); if (!cancel) return cancel;
        std::memcpy(descriptor_batch.data(),index_bytes_.data()+static_cast<std::size_t>(block_cursor*24),byte_count);
        for (std::uint64_t j=0;j<descriptor_count;++j) {
            Block b; Status st=decode_block_record(descriptor_batch.data()+j*24,b); if (!st) return st;
            // Gaps between blocks are free space from in-place edits.
            if (b.offset<align_up(previous_end,16) || b.offset>data_offset_+data_size_ || b.stored>data_offset_+data_size_-b.offset)
                return fail(ErrorCode::format,"block data offsets overlap or are misaligned");
            if (b.first_check!=next_check) return fail(ErrorCode::format,"block first_check is not the running slice count");
            next_check+=(b.logical+check_size-1)/check_size;
            block_logical_sizes_.push_back(b.logical);
            previous_end=b.offset+b.stored;
        }
        block_cursor+=descriptor_count;
    }
    if (previous_end!=data_offset_+data_size_) return fail(ErrorCode::format,"Data section has trailing or missing bytes");
    if (next_check*4ull!=integrity_size_) return fail(ErrorCode::format,"integrity size disagrees with the block slices");
    for (const auto& f:files_) {
        const std::uint64_t chunk=1ull<<f.chunk_log2;
        for (std::uint32_t j=0;j<f.count;++j) {
            const auto expected=static_cast<std::uint32_t>(std::min<std::uint64_t>(chunk,f.size-std::uint64_t(j)*chunk));
            if (block_logical_sizes_[block_refs_[f.first+j]]!=expected) return fail(ErrorCode::format,"referenced block has the wrong logical chunk length");
        }
    }
    return ok_status();
}

inline Status Archive::check_mount_limits() const {
    if (block_count_>options_.max_blocks) return fail(ErrorCode::resource_limit,"block count exceeds configured mount budget");
    if (manifest_size_>options_.max_manifest_bytes || manifest_size_>std::numeric_limits<std::size_t>::max())
        return fail(ErrorCode::resource_limit,"Manifest exceeds configured memory budget");
    const std::uint64_t index_size=block_count_*24ull+integrity_size_;
    if (index_size>options_.max_index_bytes || index_size>std::numeric_limits<std::size_t>::max())
        return fail(ErrorCode::resource_limit,"authenticated index exceeds configured memory budget");
    return ok_status();
}

inline Status Archive::check_tail_digests() const {
    auto st = check_mount_limits(); if (!st) return st;
    std::array<std::uint8_t, 64*1024> buffer;
    auto hash_region = [&](Sha256& hash, std::uint64_t at, std::uint64_t size) -> Status {
        while (size) {
            auto cancelled = check_cancelled(); if (!cancelled) return cancelled;
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(size, buffer.size()));
            if (!read(at, buffer.data(), n)) return fail(ErrorCode::io, "failed to read generation metadata");
            hash.update(buffer.data(), n);
            at += n;
            size -= n;
        }
        return ok_status();
    };
    Sha256 manifest_hash;
    st = hash_region(manifest_hash, manifest_offset_, manifest_size_); if (!st) return st;
    if (manifest_hash.finish() != manifest_digest_) return fail(ErrorCode::integrity, "Manifest SHA-256 mismatch");
    Sha256 index_hash;
    st = hash_region(index_hash, index_offset_, block_count_*24ull); if (!st) return st;
    st = hash_region(index_hash, integrity_offset_, integrity_size_); if (!st) return st;
    if (index_hash.finish() != index_digest_) return fail(ErrorCode::integrity, "index_sha256 mismatch");
    return ok_status();
}

inline Status Archive::mount_impl() {
    Status limits = check_mount_limits(); if (!limits) return limits;
    const std::uint64_t index_size=block_count_*24ull+integrity_size_;
    index_bytes_.resize(static_cast<std::size_t>(index_size));
    Sha256 index_hash;
    auto capture=[&](std::uint64_t at,std::uint64_t size,std::uint64_t destination)->Status {
        while (size) {
            auto st=check_cancelled(); if (!st) return st;
            const auto n=static_cast<std::size_t>(std::min<std::uint64_t>(size,64*1024));
            auto* out=index_bytes_.data()+static_cast<std::size_t>(destination);
            if (!read(at,out,n)) return fail(ErrorCode::io,"failed to capture authenticated index");
            index_hash.update(out,n); at+=n; destination+=n; size-=n;
        }
        return ok_status();
    };
    Status st=capture(index_offset_,block_count_*24ull,0); if (!st) return st;
    st=capture(integrity_offset_,integrity_size_,block_count_*24ull); if (!st) return st;
    if (index_hash.finish()!=index_digest_) return fail(ErrorCode::integrity,"index_sha256 mismatch");
    if (options_.collect_read_stats || options_.collect_read_timing || (options_.max_cached_block_bytes && options_.max_cached_blocks)) {
        block_cache_=std::make_shared<BlockCache>();
        // No rehash while filling the cache up to its entry limit.
        block_cache_->entries.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(options_.max_cached_blocks,65536)));
    }

    manifest_bytes_.resize(static_cast<std::size_t>(manifest_size_));
    if (!read(manifest_offset_,manifest_bytes_.data(),manifest_bytes_.size())) return fail(ErrorCode::io,"failed to read Manifest");
    // Use the digest captured from the validated header during open().
    // Never trust a second, unchecked header read.
    if (Sha256::digest(manifest_bytes_.data(),manifest_bytes_.size())!=manifest_digest_)
        return fail(ErrorCode::integrity,"Manifest SHA-256 mismatch");
    st=parse_manifest(); if (!st) return st;
    std::vector<std::uint32_t> roots;
    roots.reserve(source_resources_.size());
    std::uint64_t resource_nodes=0, resource_paths=0;
    for (const auto& resource:source_resources_) {
        if (resource.root_directory>=directories_.size())
            return fail(ErrorCode::format,"source resource root out of range");
        roots.push_back(resource.root_directory);
        const auto& dir=directories_[resource.root_directory];
        if (!dir.count || (resource.kind==1 && dir.count!=1)) return fail(ErrorCode::format,"invalid source resource file set");
        std::vector<std::uint32_t> used;
        for (std::uint32_t i=dir.first;i<dir.first+dir.count;++i) {
            const auto& e=entries_[i];
            if (e.kind!=2) return fail(ErrorCode::format,"source resource is not a flat unique file set");
            used.push_back(e.target);
            const auto name=std::string_view(reinterpret_cast<const char*>(manifest_bytes_.data()+e.name_offset),e.name_length);
            if (resource.kind==1 && name!="partition.hfs0") return fail(ErrorCode::format,"system update resource lacks partition.hfs0");
            if (++resource_nodes>options_.max_total_snapshot_nodes || e.name_length>options_.max_total_snapshot_path_bytes-resource_paths)
                return fail(ErrorCode::resource_limit,"source resource traversal budget exceeded");
            resource_paths+=e.name_length;
        }
        std::sort(used.begin(),used.end());
        if (std::adjacent_find(used.begin(),used.end())!=used.end()) return fail(ErrorCode::format,"repeated source resource file");
    }
    std::sort(roots.begin(),roots.end());
    if (!roots.empty()) {
        if (std::adjacent_find(roots.begin(),roots.end())!=roots.end())
            return fail(ErrorCode::format,"shared source resource root");
        for (const auto& snapshot:snapshots_) if (std::binary_search(roots.begin(),roots.end(),snapshot.root_directory))
            return fail(ErrorCode::format,"source resource root belongs to a snapshot");
        for (const auto& entry:entries_) if (entry.kind==1 && std::binary_search(roots.begin(),roots.end(),entry.target))
            return fail(ErrorCode::format,"source resource root is a child directory");
    }
    st=validate_media(); if (!st) return st;
    return validate_snapshot_identities(resource_nodes,resource_paths);
}

// Recomputes every snapshot identity (§12) without materializing paths: a
// first pass checks the tree and budgets and counts nodes, a second hashes
// the nodes in full-path byte order. Directory entries are already sorted by
// name, so that order is a per-directory merge of each child's own node (key
// `name`) and its subtree (key `name/`). Both passes use explicit stacks: no
// tree depth can exhaust the caller's thread stack. `total_nodes` and
// `total_paths` start from the Source Resource charges.
inline Status Archive::validate_snapshot_identities(std::uint64_t total_nodes, std::uint64_t total_paths) {
    std::vector<std::uint32_t> seen_dirs(directories_.size(),0), seen_files(files_.size(),0);
    struct Item { std::string_view name; std::uint32_t entry; bool subtree; };
    struct CheckFrame { std::uint32_t next, end; std::uint64_t prefix; };
    struct HashFrame { std::uint32_t depth; std::size_t next, mark; };
    std::vector<std::vector<Item>> levels;
    std::vector<CheckFrame> checks;
    std::vector<HashFrame> hashes;
    std::string path;
    auto name_of=[&](const Entry& e) {
        return std::string_view(reinterpret_cast<const char*>(manifest_bytes_.data()+e.name_offset),e.name_length);
    };
    // Order of `name` (or `name/` for a subtree) among siblings.
    auto before=[](const Item& a,const Item& b) {
        const auto n=std::min(a.name.size(),b.name.size());
        for (std::size_t i=0;i<n;++i) {
            const auto x=static_cast<unsigned char>(a.name[i]), y=static_cast<unsigned char>(b.name[i]);
            if (x!=y) return x<y;
        }
        if (a.name.size()==b.name.size()) return !a.subtree && b.subtree;
        // The shorter name continues with '/' for a subtree, else ends.
        if (a.name.size()<b.name.size()) return !a.subtree || '/'<static_cast<unsigned char>(b.name[n]);
        return b.subtree && static_cast<unsigned char>(a.name[n])<'/';
    };
    for (std::uint32_t si=0;si<snapshots_.size();++si) {
        const auto& snapshot=snapshots_[si];
        if (snapshot.root_directory>=directories_.size()) return fail(ErrorCode::bounds,"snapshot root directory is out of range");
        const std::uint32_t generation=si+1;
        std::uint64_t node_count=0, logical_size=0, identity_path_bytes=0;
        auto charge=[&](std::uint64_t length)->Status {
            if (length>options_.max_snapshot_path_bytes-identity_path_bytes) return fail(ErrorCode::resource_limit,"snapshot identity path budget exceeded");
            if (total_nodes>=options_.max_total_snapshot_nodes || length>options_.max_total_snapshot_path_bytes-total_paths)
                return fail(ErrorCode::resource_limit,"cumulative snapshot traversal budget exceeded");
            ++total_nodes; total_paths+=length; identity_path_bytes+=length;
            if (++node_count>options_.max_snapshot_nodes || node_count>0xffffffffu) return fail(ErrorCode::resource_limit,"snapshot node count exceeds configured budget");
            return ok_status();
        };
        // Enters one directory: cycle check and its own node; its entries
        // are then visited from the new top frame. The root has depth 0.
        auto enter=[&](std::uint32_t dir_id,std::uint64_t prefix)->Status {
            auto cancellation=check_cancelled(); if (!cancellation) return cancellation;
            if (checks.size()>max_directory_depth) return fail(ErrorCode::resource_limit,"directory depth exceeds 256");
            if (dir_id>=directories_.size() || seen_dirs[dir_id]==generation) return fail(ErrorCode::format,"directory cycle or repeated directory in snapshot");
            seen_dirs[dir_id]=generation;
            Status st=charge(prefix); if (!st) return st;
            const auto& dir=directories_[dir_id];
            checks.push_back({dir.first,dir.first+dir.count,prefix});
            return ok_status();
        };
        checks.clear();
        Status st=enter(snapshot.root_directory,0); if (!st) return st;
        while (!checks.empty()) {
            auto& frame=checks.back();
            if (frame.next==frame.end) { checks.pop_back(); continue; }
            const auto& entry=entries_[frame.next++];
            const std::uint64_t length=frame.prefix ? frame.prefix+1+entry.name_length : entry.name_length;
            if (entry.kind==1) {
                st=enter(entry.target,length); if (!st) return st;  // may invalidate `frame`
            } else {
                if (entry.target>=files_.size() || seen_files[entry.target]==generation) return fail(ErrorCode::format,"file record is repeated within a snapshot");
                seen_files[entry.target]=generation;
                const auto size=files_[entry.target].size;
                if (size>std::numeric_limits<std::uint64_t>::max()-logical_size) return fail(ErrorCode::bounds,"snapshot logical size overflows");
                logical_size+=size;
                st=charge(length); if (!st) return st;
            }
        }
        if (logical_size!=snapshot.logical_size) return fail(ErrorCode::format,"snapshot logical size mismatch");
        Sha256 identity;
        static constexpr std::uint8_t tag[]={'N','X','-','S','N','A','P','S','H','O','T','-','I','D',0};
        identity.update(tag,sizeof(tag));
        auto put_u8=[&](std::uint8_t v){ identity.update(&v,1); };
        auto put_u16=[&](std::uint16_t v){ const std::uint8_t b[2]={std::uint8_t(v),std::uint8_t(v>>8)}; identity.update(b,2); };
        auto put_u32=[&](std::uint32_t v){ const std::uint8_t b[4]={std::uint8_t(v),std::uint8_t(v>>8),std::uint8_t(v>>16),std::uint8_t(v>>24)}; identity.update(b,4); };
        auto put_u64=[&](std::uint64_t v){ const std::uint8_t b[8]={std::uint8_t(v),std::uint8_t(v>>8),std::uint8_t(v>>16),std::uint8_t(v>>24),std::uint8_t(v>>32),std::uint8_t(v>>40),std::uint8_t(v>>48),std::uint8_t(v>>56)}; identity.update(b,8); };
        const auto& app=applications_[snapshot_owner_[si]];
        const std::uint64_t program_id=snapshot.program_index==no_index?0xffffffffffffffffull:programs_[snapshot.program_index].id;
        put_u16(3); put_u8(app.platform); put_u64(app.id); put_u64(program_id); put_u8(snapshot.type); put_u32(snapshot.numeric_version);
        identity.update(snapshot.content_id.data(), snapshot.content_id.size());
        put_u32(static_cast<std::uint32_t>(node_count));
        auto node=[&](std::uint8_t kind) {
            put_u8(kind); put_u32(static_cast<std::uint32_t>(path.size()));
            identity.update(reinterpret_cast<const std::uint8_t*>(path.data()),path.size());
        };
        // Sorted node keys of one directory, in its depth's reusable level.
        auto fill=[&](std::uint32_t dir_id,std::uint32_t depth) {
            if (levels.size()<=depth) levels.resize(depth+1);
            auto& items=levels[depth];
            items.clear();
            const auto& dir=directories_[dir_id];
            for (std::uint32_t i=dir.first;i<dir.first+dir.count;++i) {
                items.push_back({name_of(entries_[i]),i,false});
                if (entries_[i].kind==1) items.push_back({name_of(entries_[i]),i,true});
            }
            std::sort(items.begin(),items.end(),before);
        };
        path.clear();
        node(1);  // root, empty path
        fill(snapshot.root_directory,0);
        hashes.clear();
        hashes.push_back({0,0,0});
        while (!hashes.empty()) {
            auto& frame=hashes.back();
            if (frame.next==levels[frame.depth].size()) { path.resize(frame.mark); hashes.pop_back(); continue; }
            // Deeper frames use deeper levels only, so this copy stays valid.
            const Item item=levels[frame.depth][frame.next++];
            const auto depth=frame.depth;
            const auto& entry=entries_[item.entry];
            const auto mark=path.size();
            if (mark) path.push_back('/');
            path.append(item.name.data(),item.name.size());
            if (item.subtree) {
                fill(entry.target,depth+1);
                hashes.push_back({depth+1,0,mark});  // invalidates `frame`
                continue;
            }
            if (entry.kind==1) node(1);
            else {
                node(2); put_u64(files_[entry.target].size);
                identity.update(files_[entry.target].digest.data(),files_[entry.target].digest.size());
            }
            path.resize(mark);
        }
        if (identity.finish()!=snapshot.identity) return fail(ErrorCode::integrity,"snapshot identity mismatch");
    }
    return ok_status();
}

// Binary search of one directory's sorted entries; no_index when absent.
inline std::uint32_t Archive::find_entry(std::uint32_t directory,std::string_view name) const {
    const auto& d=directories_[directory];
    std::uint32_t low=d.first, high=d.first+d.count;
    while (low<high) {
        const auto mid=low+(high-low)/2;
        const auto& entry=entries_[mid];
        const std::string_view candidate(reinterpret_cast<const char*>(manifest_bytes_.data()+entry.name_offset),entry.name_length);
        // Byte order: memcmp compares as unsigned char.
        const auto n=std::min(candidate.size(),name.size());
        int order=n ? std::memcmp(candidate.data(),name.data(),n) : 0;
        if (!order) order=candidate.size()<name.size() ? -1 : candidate.size()>name.size() ? 1 : 0;
        if (!order) return mid;
        if (order<0) low=mid+1; else high=mid;
    }
    return no_index;
}

inline Status Archive::find_file(std::uint32_t snapshot_index,std::string_view path,std::uint32_t& file_id) const {
    if (snapshot_index>=snapshots_.size()) return fail(ErrorCode::bounds,"snapshot index is out of range");
    if (path.empty() || !utf8_valid(reinterpret_cast<const std::uint8_t*>(path.data()),path.size())) return fail(ErrorCode::invalid_argument,"path is empty or invalid UTF-8");
    std::uint32_t directory=snapshots_[snapshot_index].root_directory;
    std::size_t start=0;
    for (;;) {
        const auto slash=path.find('/',start);
        const auto end=slash==std::string_view::npos?path.size():slash;
        if (end==start || end-start>4096) return fail(ErrorCode::invalid_argument,"path contains an empty or oversized component");
        const auto component=path.substr(start,end-start);
        if (component=="." || component==".." || component.find('\\')!=std::string_view::npos) return fail(ErrorCode::invalid_argument,"path contains an unsafe component");
        const auto found=find_entry(directory,component);
        if (found==no_index) return fail(ErrorCode::not_found,"file path was not found in selected snapshot");
        const auto& entry=entries_[found];
        if (slash==std::string_view::npos) {
            if (entry.kind!=2) return fail(ErrorCode::not_found,"path names a directory, not a file");
            file_id=entry.target; return ok_status();
        }
        if (entry.kind!=1) return fail(ErrorCode::not_found,"path component is not a directory");
        directory=entry.target; start=slash+1;
        if (start==path.size()) return fail(ErrorCode::invalid_argument,"path has a trailing separator");
    }
}

inline Status Archive::read_file_impl(std::uint32_t file_id,
                                      std::uint64_t offset,std::size_t length,std::uint8_t* output) {
    if (file_id>=files_.size()) return fail(ErrorCode::bounds,"file ID is out of range");
    const auto& file=files_[file_id];
    if (offset>file.size || length>file.size-offset) return fail(ErrorCode::bounds,"requested file range is outside logical file");
    if (!length) return ok_status();
    std::size_t done=0;
    // A single-block read is fully verified before its one copy, so it goes
    // straight to the caller. A multi-block read is staged in private memory
    // so the output stays unchanged when a later block fails.
    if ((offset>>file.chunk_log2)==((offset+length-1)>>file.chunk_log2))
        return read_range(file_id,offset,length,output,Sink::caller,done);
    ReadBuffer staging;  // uninitialized: every byte is written before use
    staging.reserve(length);
    Status st=read_range(file_id,offset,length,staging.bytes.get(),Sink::staging,done);
    if (!st) return st;
#if defined(NX_READER_DIAGNOSTICS)
    const auto start=options_.collect_read_timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#endif
    std::memcpy(output,staging.bytes.get(),length);
#if defined(NX_READER_DIAGNOSTICS)
    if (options_.collect_read_timing && block_cache_) {
        const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
        std::lock_guard<std::shared_mutex> lock(block_cache_->mutex); block_cache_->stats.copy_ns+=static_cast<std::uint64_t>(ns);
    }
#endif
    return ok_status();
}

// Writes [offset, offset + length) of the file to `destination` in file order.
// `done` only grows after the bytes it covers have been verified and copied.
inline Status Archive::read_range(std::uint32_t file_id, std::uint64_t offset, std::size_t length,
                                  std::uint8_t* destination, Sink sink, std::size_t& done) {
    Status st;
    done=0;
    if (file_id>=files_.size()) return fail(ErrorCode::bounds,"file ID is out of range");
    const auto& file=files_[file_id];
    if (offset>file.size || length>file.size-offset) return fail(ErrorCode::bounds,"requested file range is outside logical file");
    const std::uint64_t chunk_size=1ull<<file.chunk_log2;
    ReadBuffer stored;
    // Reuse decoded storage across chunks instead of allocating per block.
    detail::LogicalBytes logical;
    bool raw_single=false;  // set after a failed RAW run read
    RawPiece pieces[max_raw_run];  // built once per call, refilled per run
    while (done<length) {
        // RAW blocks have no other cancellation point.
        if (done) { st=check_cancelled(); if (!st) return st; }
        const std::uint64_t file_at=offset+done;
        const std::uint64_t chunk_index=file_at>>file.chunk_log2;
        const std::size_t in_chunk=static_cast<std::size_t>(file_at&(chunk_size-1));
        const auto block_id=block_refs_[file.first+static_cast<std::uint32_t>(chunk_index)];
        std::uint8_t* const target=destination+done;
        Block block;
        st=block_record(block_id,block); if (!st) return st;
        if (block.codec==0) {
            // RAW: read and verify only the 16 KiB slices covering the request.
            // Bytes reach the caller only after their slices have verified.
            if (in_chunk>=block.logical) return fail(ErrorCode::format,"file offset maps outside RAW block");
            const auto amount=std::min<std::size_t>(length-done,block.logical-in_chunk);
            if (options_.verify_read_blocks) {
                // A run of RAW blocks stored back to back is fetched with one
                // source read (at most raw_run_bytes), then each block is
                // verified slice by slice and committed in file order. A run
                // only holds whole 16 KiB slices of the request, so it is read
                // straight into the output; unaligned edges go through scratch.
                std::size_t count=0, run_bytes=0, covered=done;
                auto add=[&](const Block& b,std::size_t at) {
                    auto& piece=pieces[count++];
                    piece.block=b; piece.in_chunk=at;
                    piece.amount=std::min<std::size_t>(length-covered,b.logical-at);
                    piece.begin=at/check_size*check_size;
                    piece.span=std::min<std::size_t>(b.logical,static_cast<std::size_t>(align_up(at+piece.amount,check_size)))-piece.begin;
                    run_bytes+=piece.span; covered+=piece.amount;
                };
                add(block,in_chunk);
                // Atomic single-block reads never write the caller buffer before
                // verification. Partial reads may: on failure they zero every
                // byte they wrote past the verified prefix.
                const bool direct=sink!=Sink::caller && pieces[0].begin==pieces[0].in_chunk &&
                                  pieces[0].span==pieces[0].amount;
                while (direct && !raw_single && covered<length && count<max_raw_run) {
                    const auto& last=pieces[count-1];
                    if (last.in_chunk+last.amount!=last.block.logical) break;
                    const auto next_index=file.first+static_cast<std::uint32_t>((offset+covered)>>file.chunk_log2);
                    Block next;
                    if (!block_record(block_refs_[next_index],next) || next.codec!=0 ||
                        next.offset!=last.block.offset+last.block.logical) break;
                    const auto next_amount=std::min<std::size_t>(length-covered,next.logical);
                    // A trailing partial slice is read on its own, through scratch.
                    if (next_amount!=next.logical && next_amount%check_size) break;
                    if (run_bytes+next_amount>raw_run_bytes) break;
                    add(next,0);
                }
                std::uint8_t* bytes=target;
                if (!direct) { stored.reserve(run_bytes); bytes=stored.bytes.get(); }
                const auto scrub=[&] { if (direct && sink==Sink::partial) std::memset(destination+done,0,covered-done); };
                if (!read(block.offset+pieces[0].begin,bytes,run_bytes)) {
                    scrub();
                    // Retry block by block so a partial read keeps an exact prefix.
                    if (count>1) { raw_single=true; continue; }
                    return fail(ErrorCode::io,"failed to read RAW block slice");
                }
                if (block_cache_) {
                    auto& counter=block_cache_->counter();
                    BlockCache::add(counter.raw_reads); BlockCache::add(counter.raw_bytes,run_bytes);
                }
                for (std::size_t i=0;i<count;++i) {
                    if (i) { st=check_cancelled(); if (!st) { scrub(); return st; } }
                    const auto& piece=pieces[i];
                    std::uint8_t* const out=destination+done;
                    std::size_t verified=0;
                    st=verify_slices(piece.block,bytes,piece.begin/check_size,piece.span,sink==Sink::partial ? &verified : nullptr);
                    if (!st) {
                        // Partial reads keep the verified slices in front of the bad one.
                        if (sink==Sink::partial && piece.begin+verified>piece.in_chunk) {
                            const auto good=std::min(piece.amount,piece.begin+verified-piece.in_chunk);
                            if (!direct) std::memcpy(out,bytes+(piece.in_chunk-piece.begin),good);
                            done+=good;
                        }
                        scrub();
                        return st;
                    }
                    if (!direct) std::memcpy(out,bytes+(piece.in_chunk-piece.begin),piece.amount);
                    done+=piece.amount;
                    bytes+=piece.span;
                }
                continue;
            } else {
                // Scratch keeps caller output intact if the source fails midway.
                std::uint8_t* bytes=target;
                if (sink!=Sink::staging) { stored.reserve(amount); bytes=stored.bytes.get(); }
                if (!read(block.offset+in_chunk,bytes,amount)) return fail(ErrorCode::io,"failed to read RAW block slice");
                if (sink!=Sink::staging) std::memcpy(target,bytes,amount);
                done+=amount;
                continue;
            }
        }
        // Zstandard: decode the whole frame; decoded blocks may be cached.
        if (options_.verify_read_blocks && block_cache_) {
            std::size_t amount = 0;
            st=read_cached_slice(block_id,in_chunk,length-done,target,amount,stored);
            if (!st) return st;
            done+=amount;
            continue;
        }
        st=load_block(block_id,options_.verify_read_blocks,logical,&block,&stored); if (!st) return st;
        st=check_cancelled(); if (!st) return st;
        if (in_chunk>=logical.size()) return fail(ErrorCode::format,"file offset maps outside decoded block");
        const auto amount=std::min(length-done,logical.size()-in_chunk);
        std::memcpy(target,logical.data()+in_chunk,amount);
        done+=amount;
    }
    return ok_status();
}

inline Status Archive::validate_media() {
    if (!media_size_) {
        if (!media_use_count_.empty()) return fail(ErrorCode::format,"Catalog references absent media");
        return ok_status();
    }
    std::uint64_t cursor=media_payload_offset_;
    for (std::uint32_t i=0;i<media_record_count_;++i) {
        std::array<std::uint8_t,64> row{};
        const std::uint64_t row_at=media_offset_+32+std::uint64_t(i)*64;
        if (!read(row_at,row.data(),row.size())) return fail(ErrorCode::io,"failed to read media descriptor");
        const auto type=u16(row.data()); const auto codec=row[2]; const auto flags=row[3];
        const auto stored=u64(row.data()+4), logical=u64(row.data()+12), offset=u64(row.data()+20);
        const auto aligned=align_up(cursor,16);
        if (aligned>media_size_) return fail(ErrorCode::bounds,"Media payload alignment exceeds region");
        if (aligned>cursor) { Status pad=zero_range(media_offset_+cursor,aligned-cursor); if (!pad) return pad; }
        if (type<1 || type>5 || codec || flags || !stored || stored>64ull*1024*1024 || logical!=stored || offset!=aligned || offset%16 || !all_zero(row.data()+60,4))
            return fail(ErrorCode::format,"invalid media descriptor");
        if (!range_ok(offset,stored,media_size_) || offset<media_payload_offset_) return fail(ErrorCode::bounds,"media payload range is invalid");
        const auto mask=media_expected_mask_[i];
        if (!media_use_count_[i] || !(mask&(1u<<type))) return fail(ErrorCode::format,"media record is unreferenced or has the wrong metadata type");
        if (type>2 && media_use_count_[i]!=1) return fail(ErrorCode::format,"non-icon media record must have exactly one owner");
        cursor=offset+stored;
    }
    if (cursor!=media_size_) return fail(ErrorCode::format,"Media region has trailing or missing bytes");
    return ok_status();
}

inline Result<std::vector<std::uint8_t>> Archive::read_media_impl(std::uint32_t index) {
    if (index>=media_record_count_ || !media_size_) return Result<std::vector<std::uint8_t>>::failure(ErrorCode::bounds,"media index is out of range");
    std::array<std::uint8_t,64> row{};
    if (!read(media_offset_+32+std::uint64_t(index)*64,row.data(),row.size())) return Result<std::vector<std::uint8_t>>::failure(ErrorCode::io,"failed to read media descriptor");
    const auto type=u16(row.data()); const auto codec=row[2]; const auto flags=row[3];
    const auto stored=u64(row.data()+4), logical=u64(row.data()+12), offset=u64(row.data()+20);
    if (type<1 || type>5 || codec || flags || !stored || stored>64ull*1024*1024 || stored!=logical || offset%16 ||
        offset<media_payload_offset_ || !range_ok(offset,stored,media_size_) || !all_zero(row.data()+60,4))
        return Result<std::vector<std::uint8_t>>::failure(ErrorCode::format,"invalid media descriptor");
    if (index>=media_expected_mask_.size() || !media_use_count_[index] ||
        !(media_expected_mask_[index]&(1u<<type)) || (type>2 && media_use_count_[index]!=1))
        return Result<std::vector<std::uint8_t>>::failure(ErrorCode::format,"Media owner/type mismatch");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stored));
    if (!read(media_offset_+offset,bytes.data(),bytes.size())) return Result<std::vector<std::uint8_t>>::failure(ErrorCode::io,"failed to read media payload");
    const auto digest=Sha256::digest(bytes.data(),bytes.size());
    if (!std::equal(digest.begin(),digest.end(),row.begin()+28)) return Result<std::vector<std::uint8_t>>::failure(ErrorCode::integrity,"media SHA-256 mismatch");
    return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

inline Status Archive::block_record(std::uint32_t id, Block& block) const {
    if (id>=block_count_) return fail(ErrorCode::bounds,"block ID is out of range");
    return decode_block_record(index_bytes_.data()+std::size_t(id)*24,block);
}

inline Status Archive::decode_block_record(const std::uint8_t* row, Block& block) const {
    block.offset=u64(row); block.stored=u32(row+8); block.logical=u32(row+12); block.codec=row[16]; block.first_check=u32(row+20);
    if (block.codec>1 || row[17] || u16(row+18) || !block.logical || block.logical>1024*1024 ||
        !block.stored || block.stored>1024*1024+65536 || block.offset%16 || !range_ok(block.offset,block.stored,data_offset_+data_size_) || block.offset<data_offset_)
        return fail(ErrorCode::format,"invalid hot block descriptor");
    if (block.codec==0 && block.stored!=block.logical) return fail(ErrorCode::format,"RAW block sizes differ");
    return ok_status();
}

// `bytes` holds `size` logical bytes starting at slice `first_slice`; the run
// ends on a slice boundary or at the block end.
inline Status Archive::verify_slices(const Block& block, const std::uint8_t* bytes,
                                     std::size_t first_slice, std::size_t size, std::size_t* verified) const {
#if defined(NX_READER_DIAGNOSTICS)
    const auto hash_start=options_.collect_read_timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#endif
    const std::uint8_t* table=index_bytes_.data()+static_cast<std::size_t>(block_count_*24ull)+
                              (std::size_t(block.first_check)+first_slice)*4;
    std::size_t at=0, k=0;
    if (const auto x8=detail::crc32c_x8_function()) {
        // Slices are independent: 8 chains in one loop run about 6x faster.
        std::uint32_t got[8];
        for (; size-at>=8*check_size; at+=8*check_size, k+=8) {
            x8(bytes+at,check_size,got);
            for (std::size_t j=0; j<8; ++j)
                if (got[j]!=u32(table+(k+j)*4)) {
                    if (verified) *verified=at+j*check_size;
                    return fail(ErrorCode::integrity,"block slice CRC-32C mismatch");
                }
        }
    }
    for (; at<size; at+=check_size, ++k) {
        const auto n=std::min(check_size,size-at);
        if (crc32c(bytes+at,n)!=u32(table+k*4)) {
            if (verified) *verified=at;
            return fail(ErrorCode::integrity,"block slice CRC-32C mismatch");
        }
    }
#if defined(NX_READER_DIAGNOSTICS)
    if (options_.collect_read_timing && block_cache_) {
        const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-hash_start).count();
        std::lock_guard<std::shared_mutex> lock(block_cache_->mutex); block_cache_->stats.hash_ns+=static_cast<std::uint64_t>(ns);
    }
#endif
    return ok_status();
}

inline Status Archive::read_cached_slice(std::uint32_t id, std::size_t offset, std::size_t wanted,
                                                std::uint8_t* output, std::size_t& copied, ReadBuffer& stored) const {
    auto status=check_cancelled(); if (!status) return status;
    auto& cache=*block_cache_;
    // Copies outside the cache lock from a pinned block.
    const auto copy=[&](const BlockBytes& pinned) -> Status {
        struct Unpin { BlockData& data; ~Unpin() { data.pins.fetch_sub(1,std::memory_order_release); } } unpin{*pinned};
        const auto& bytes=pinned->bytes;
        if (offset>=bytes.size()) return fail(ErrorCode::format,"file offset maps outside cached block");
        copied=std::min(wanted,bytes.size()-offset);
#if defined(NX_READER_DIAGNOSTICS)
        const auto copy_start=options_.collect_read_timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#endif
        std::memcpy(output,bytes.data()+offset,copied);
#if defined(NX_READER_DIAGNOSTICS)
        if (options_.collect_read_timing) {
            const auto ns=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-copy_start).count());
            std::lock_guard<std::shared_mutex> lock(cache.mutex); cache.stats.copy_ns+=ns;
        }
#endif
        return ok_status();
    };
    // Under the shared lock: find, mark referenced and pin. Hits never write
    // the map or the list, so they do not wait for each other.
    const auto pin=[&](bool coalesced) -> BlockBytes {
        auto& counter=cache.counter();
        BlockBytes block;
        {
            std::shared_lock<std::shared_mutex> lock(cache.mutex);
            const auto hit=cache.entries.find(id);
            if (hit!=cache.entries.end()) {
                block=hit->second.bytes;
                block->pins.fetch_add(1,std::memory_order_relaxed);
                // Skip the store when already set: keeps the line shared.
                if (!block->referenced.load(std::memory_order_relaxed)) block->referenced.store(true,std::memory_order_relaxed);
            }
        }
        if (!block) { if (!coalesced) BlockCache::add(counter.misses); return {}; }
        BlockCache::add(counter.hits); if (coalesced) BlockCache::add(counter.coalesced);
        return block;
    };
    if (auto pinned=pin(false)) return copy(pinned);
    std::unique_lock<std::mutex> loading(cache.loads[id%cache.loads.size()],std::defer_lock);
    if (!loading.try_lock()) {
        BlockCache::add(cache.counter().load_waits);
        loading.lock();
    }
    status=check_cancelled(); if (!status) return status;
    if (auto pinned=pin(true)) { loading.unlock(); return copy(pinned); }
    detail::LogicalBytes logical;
    BlockBytes reuse;
    {
        std::lock_guard<std::shared_mutex> lock(cache.mutex);
        reuse=std::move(cache.spare);
        if (reuse) logical.swap(reuse->bytes);
    }
    status=load_block(id,true,logical,nullptr,&stored); if (!status) return status;
    status=check_cancelled(); if (!status) return status;
    if (offset>=logical.size()) return fail(ErrorCode::format,"file offset maps outside decoded block");
    if (options_.max_cached_blocks && logical.size()<=options_.max_cached_block_bytes) {
        BlockBytes block;
        if (reuse) { reuse->bytes.swap(logical); block=std::move(reuse); }
        else { block=std::make_shared<BlockData>(); block->bytes=std::move(logical); }
        block->pins.store(1,std::memory_order_relaxed);  // this reader's copy below
        block->referenced.store(false,std::memory_order_relaxed);
        {
            std::lock_guard<std::shared_mutex> lock(cache.mutex);
            // CLOCK: a referenced block gets one more pass; after a full lap
            // every bit is clear, so this ends within 2 * entries steps.
            while (!cache.entries.empty() && (block->bytes.size()>options_.max_cached_block_bytes-cache.bytes ||
                                             cache.entries.size()>=options_.max_cached_blocks)) {
                const auto victim=cache.lru.front();
                auto& candidate=*cache.entries.find(victim)->second.bytes;
                if (candidate.referenced.load(std::memory_order_relaxed)) {
                    candidate.referenced.store(false,std::memory_order_relaxed);
                    cache.lru.splice(cache.lru.end(),cache.lru,cache.lru.begin());
                    continue;
                }
                auto node=cache.entries.extract(victim);
                auto& evicted=node.mapped().bytes;
                cache.bytes-=evicted->bytes.size();
                // Recycle the buffer only when no reader still holds it. New
                // pins need this lock, so use_count() cannot grow meanwhile.
                if (evicted.use_count()==1 && evicted->pins.load(std::memory_order_acquire)==0 &&
                    (!cache.spare || evicted->bytes.capacity()>cache.spare->bytes.capacity())) cache.spare=std::move(evicted);
                else evicted.reset();
                cache.spare_node=std::move(node);
                cache.spare_position.splice(cache.spare_position.end(),cache.lru,cache.lru.begin());
                if (cache.spare_position.size()>1) cache.spare_position.pop_front();
                ++cache.stats.evictions;
            }
            if (!cache.spare_position.empty()) cache.lru.splice(cache.lru.end(),cache.spare_position,cache.spare_position.begin());
            else cache.lru.push_back(id);
            cache.lru.back()=id;
            try {
                if (cache.spare_node) {
                    auto node=std::move(cache.spare_node);
                    node.key()=id; node.mapped()=CachedBlock{block};
                    cache.entries.insert(std::move(node));
                } else cache.entries.emplace(id,CachedBlock{block});
            } catch (...) { cache.lru.pop_back(); throw; }
            cache.bytes+=block->bytes.size();
            cache.stats.peak_bytes=std::max(cache.stats.peak_bytes,cache.bytes);
        }
        loading.unlock();
        return copy(block);
    }
    copied=std::min(wanted,logical.size()-offset);
    std::memcpy(output,logical.data()+offset,copied);
    return ok_status();
}

inline Status Archive::load_block(std::uint32_t id, bool verify, detail::LogicalBytes& logical,
                                  const Block* known_record, ReadBuffer* stored_scratch) const {
    auto cancelled=check_cancelled(); if (!cancelled) return cancelled;
    Block local_block;
    const Block* selected=known_record;
    if (!selected) {
        Status st=block_record(id,local_block); if (!st) return st;
        selected=&local_block;
    }
    const Block& block=*selected;
    logical.resize(block.logical);
#if defined(NX_READER_DIAGNOSTICS)
    auto io_start=std::chrono::steady_clock::time_point{};
    const auto start_io=[&] { if (options_.collect_read_timing) io_start=std::chrono::steady_clock::now(); };
    auto record_io=[&] { if (options_.collect_read_timing && block_cache_) {
        const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-io_start).count();
        std::lock_guard<std::shared_mutex> lock(block_cache_->mutex); block_cache_->stats.io_ns+=static_cast<std::uint64_t>(ns);
    }};
#else
    const auto start_io=[] {};
    const auto record_io=[] {};
#endif
    if (block.codec==0) {
        // RAW blocks need no intermediate compressed-data buffer or copy.
        if (block_cache_) BlockCache::add(block_cache_->counter().raw_reads);
        start_io();
        if (!read(block.offset,logical.data(),logical.size()))
            return fail(ErrorCode::io,"failed to read RAW block payload");
        record_io();
    } else {
        ReadBuffer local_stored;
        auto& stored=stored_scratch ? *stored_scratch : local_stored;
        stored.reserve(block.stored);
        start_io();
        if (!read(block.offset,stored.bytes.get(),block.stored))
            return fail(ErrorCode::io,"failed to read compressed block payload");
        record_io();
        if (!options_.zstd) return fail(ErrorCode::decompression,"Zstandard block requires a caller decoder");
        std::size_t written=0, consumed=0;
        const auto started=block_cache_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const bool decoded=options_.zstd(options_.zstd_context,stored.bytes.get(),block.stored,logical.data(),logical.size(),&written,&consumed);
        if (block_cache_) {
            const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
            auto& counter=block_cache_->counter();
            BlockCache::add(counter.decompressions); BlockCache::add(counter.decompression_ns,static_cast<std::uint64_t>(ns));
        }
        if (!decoded || written!=logical.size() || consumed!=block.stored)
            return fail(ErrorCode::decompression,"Zstandard callback failed or did not consume one exact frame");
    }
    if (block_cache_) BlockCache::add(block_cache_->counter().stored_bytes_read,block.stored);
    auto after_io=check_cancelled(); if (!after_io) return after_io;
    if (verify) {
        Status st=verify_slices(block,logical.data(),0,logical.size()); if (!st) return st;
    }
    return ok_status();
}

} // namespace nx

#endif // NX_READER_HPP_INCLUDED
