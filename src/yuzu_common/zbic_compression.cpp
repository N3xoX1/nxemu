// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Adapted from Eden PR #4482 (PavelBARABANOV and Eden contributors).
// https://git.eden-emu.dev/eden-emu/eden/pulls/4482

#include "yuzu_common/zbic_compression.h"

// The C++ amalgamation must use the sanitizer runtime's C declarations.
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
#endif
#endif

#define ZSTD_ZBIC_SUPPORT 1
#define ZSTDLIB_VISIBLE static
#define ZSTDLIB_HIDDEN static
#define ZSTDERRORLIB_VISIBLE static
#define ZSTDERRORLIB_HIDDEN static
#define ZDICTLIB_VISIBLE static
#define ZDICTLIB_HIDDEN static
#define g_debuglevel g_ZSTD_zbic_debuglevel
#undef ZSTD_MULTITHREAD

#if defined(__ANDROID__)
#undef _GNU_SOURCE
#endif

#include "zstd.h"
#define g_ZSTD_threading_useless_symbol g_ZSTD_zbic_threading_useless_symbol
#include "zstd.c"
#undef g_ZSTD_threading_useless_symbol
#undef g_debuglevel

namespace Common::Compression {

bool IsZBIC(std::span<const u8> src) {
    if (src.size() < sizeof(u32)) {
        return false;
    }
    return src[0] == 'Z' && src[1] == 'B' && src[2] == 'I' && src[3] == 'C';
}

bool DecompressDataZBIC(std::span<u8> dst, std::span<const u8> src) {
    if (!IsZBIC(src)) {
        return false;
    }

    const size_t res = ZSTD_decompress(dst.data(), dst.size(), src.data(), src.size());
    return !ZSTD_isError(res) && res == dst.size();
}

} // namespace Common::Compression
