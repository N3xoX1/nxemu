// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/dynamic_library.h"
#include "common/sha256.h"
#include <array>
#include <limits>
#include <string_view>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#endif

namespace VideoCommon::ShaderProfile {
namespace HashDetail {
#ifndef __APPLE__
class Provider {
public:
    Provider() {
#ifdef _WIN32
        library=DynamicLibraryOpen("bcrypt.dll",false);
        if (!library) return;
        const auto open=reinterpret_cast<decltype(&BCryptOpenAlgorithmProvider)>(
            DynamicLibraryGetProc(library,"BCryptOpenAlgorithmProvider"));
        close=reinterpret_cast<decltype(close)>(DynamicLibraryGetProc(library,"BCryptCloseAlgorithmProvider"));
        hash=reinterpret_cast<decltype(hash)>(DynamicLibraryGetProc(library,"BCryptHash"));
        if (!open || !close || !hash || open(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)
            algorithm=nullptr;
#else
        for (const char* name:{"libcrypto.so.3","libcrypto.so.1.1","libcrypto.so"}) {
            library=DynamicLibraryOpen(name,false);
            if (library) {
                hash=reinterpret_cast<decltype(hash)>(DynamicLibraryGetProc(library,"SHA256"));
                if (hash) break;
                DynamicLibraryClose(library);library=nullptr;
            }
        }
#endif
    }
    ~Provider() {
#ifdef _WIN32
        if (algorithm && close) close(algorithm,0);
#endif
        DynamicLibraryClose(library);
    }
    bool Digest(std::string_view bytes,unsigned char* out) const {
#ifdef _WIN32
        return algorithm && hash && bytes.size()<=(std::numeric_limits<ULONG>::max)() &&
            hash(algorithm,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                 static_cast<ULONG>(bytes.size()),out,SHA256::DIGEST_SIZE)>=0;
#else
        return hash && hash(reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size(),out);
#endif
    }
private:
    DynLibHandle library{};
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm{};
    decltype(&BCryptHash) hash{};
    decltype(&BCryptCloseAlgorithmProvider) close{};
#else
    unsigned char* (*hash)(const unsigned char*,size_t,unsigned char*){};
#endif
};
#endif
} // namespace HashDetail

inline std::array<unsigned char,SHA256::DIGEST_SIZE> ResourceDigest(std::string_view bytes) {
    std::array<unsigned char,SHA256::DIGEST_SIZE> digest{};
#ifdef __APPLE__
    const bool accelerated=bytes.size()<=(std::numeric_limits<CC_LONG>::max)() &&
        CC_SHA256(bytes.data(),static_cast<CC_LONG>(bytes.size()),digest.data());
#else
    static const HashDetail::Provider provider;
    const bool accelerated=provider.Digest(bytes,digest.data());
#endif
    if (!accelerated) {
        SHA256 fallback;fallback.init();
        fallback.update(reinterpret_cast<const unsigned char*>(bytes.data()),
                        static_cast<unsigned int>(bytes.size()));
        fallback.final(digest.data());
    }
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
