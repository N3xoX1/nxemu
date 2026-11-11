// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/sha256.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
std::string Finish(SHA256& hash) {
    std::array<unsigned char, SHA256::DIGEST_SIZE> digest{};
    hash.final(digest.data());
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : digest) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

void Check(std::string_view bytes, std::string_view expected) {
    if (sha256(std::string(bytes)) != expected)
        throw std::runtime_error("SHA-256 one-shot vector mismatch");
    for (const size_t stride : {size_t{1}, size_t{7}, size_t{64}, size_t{129}}) {
        SHA256 hash;
        hash.init();
        hash.update(nullptr, 0);
        for (size_t at = 0; at < bytes.size(); at += stride) {
            hash.update(reinterpret_cast<const unsigned char*>(bytes.data() + at),
                        std::min(stride, bytes.size() - at));
            hash.update(nullptr, 0);
        }
        if (Finish(hash) != expected)
            throw std::runtime_error("SHA-256 streaming vector mismatch");
    }
}
} // namespace

int main() {
    Check("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    Check("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // Binary messages around the padding and block boundaries. Reference
    // digests were generated independently with Python hashlib.sha256.
    constexpr std::array<std::pair<size_t, std::string_view>, 8> vectors{{
        {55, "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
        {56, "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
        {63, "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488"},
        {64, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
        {65, "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"},
        {127, "92ca0fa6651ee2f97b884b7246a562fa71250fedefe5ebf270d31c546bfea976"},
        {128, "471fb943aa23c511f6f72f8d1652d9c880cfa392ad80503120547703e56a2be5"},
        {129, "5099c6a56203f9687f7d33f4bfdf576d31dc91f6b695ecea38b2770c87631135"},
    }};
    for (const auto& [size, expected] : vectors) {
        std::string bytes(size, '\0');
        for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<char>(i);
        Check(bytes, expected);
    }
    // NX verification streams whole files, which can exceed 2^32 bits.
    // Keep memory bounded while crossing that boundary with a partial block.
    const std::string block(1024 * 1024, 'a');
    SHA256 large;
    large.init();
    for (unsigned i = 0; i < 512; ++i)
        large.update(reinterpret_cast<const unsigned char*>(block.data()), block.size());
    large.update(reinterpret_cast<const unsigned char*>(block.data()), 17);
    if (Finish(large) != "81ffd7307d7eebf5a424b8d079027b0430253c83879dd43219746e244cfc3125")
        throw std::runtime_error("SHA-256 length truncated past 512 MiB");
    std::cout << "PASS SHA-256 standard/binary/streaming vectors and 512 MiB boundary\n";
}
