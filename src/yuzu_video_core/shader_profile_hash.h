// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/sha256.h"
#include <array>
#include <string>
#include <string_view>

namespace VideoCommon::ShaderProfile {
inline std::array<unsigned char,SHA256::DIGEST_SIZE> ResourceDigest(std::string_view bytes) {
    std::array<unsigned char,SHA256::DIGEST_SIZE> digest{};
    SHA256 hash;hash.init();
    hash.update(reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size());
    hash.final(digest.data());
    return digest;
}
inline std::string ResourceSha256(std::string_view bytes) {
    const auto digest=ResourceDigest(bytes);
    constexpr char hex[]="0123456789abcdef";
    std::string out(64,'\0');
    for(size_t i=0;i<digest.size();++i) {
        out[2*i]=hex[digest[i]>>4];out[2*i+1]=hex[digest[i]&15];
    }
    return out;
}
} // namespace VideoCommon::ShaderProfile
