#ifndef NX_NATIVE_SHA256_HPP_INCLUDED
#define NX_NATIVE_SHA256_HPP_INCLUDED
#include "nx_reader.hpp"

// Optional block-hash callback. No retained TLS/provider handles: an emulator
// module may be unloaded after its sessions and outstanding calls are released.
// OpenSSL is a build-time optional dependency, never probed via unsafe DLL paths.
#if !defined(NX_DISABLE_NATIVE_SHA256) && defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#if defined(_MSC_VER)
#pragma comment(lib,"bcrypt.lib")
#endif
#elif !defined(NX_DISABLE_NATIVE_SHA256) && defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#elif !defined(NX_DISABLE_NATIVE_SHA256) && defined(NX_USE_OPENSSL)
#include <openssl/evp.h>
#endif

namespace nx {
inline const char* native_sha256_name() noexcept {
#if !defined(NX_DISABLE_NATIVE_SHA256) && defined(_WIN32)
    return "windows-cng";
#elif !defined(NX_DISABLE_NATIVE_SHA256) && defined(__APPLE__)
    return "apple-commoncrypto";
#elif !defined(NX_DISABLE_NATIVE_SHA256) && defined(NX_USE_OPENSSL)
    return "openssl-evp";
#else
    return "portable";
#endif
}
inline bool portable_block_sha256(void*,const std::uint8_t* data,std::size_t size,
                                  std::uint8_t output[32]) {
    if (!output || (size && !data) || size>1024*1024) return false;
    const auto hash=Sha256::digest(data,size);
    std::memcpy(output,hash.data(),hash.size());
    return true;
}
inline bool native_block_sha256(void*,const std::uint8_t* data,std::size_t size,
                                std::uint8_t output[32]) {
    if (!output || (size && !data) || size>1024*1024) return false;
    // A provider may partially write before reporting failure. Publish only a
    // completed native digest, or recompute the entire digest portably.
    std::array<std::uint8_t,32> digest{};
    bool done=false;
#if !defined(NX_DISABLE_NATIVE_SHA256) && defined(_WIN32)
    done=BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,
        const_cast<PUCHAR>(data),static_cast<ULONG>(size),digest.data(),32)>=0;
#elif !defined(NX_DISABLE_NATIVE_SHA256) && defined(__APPLE__)
    done=CC_SHA256(data,static_cast<CC_LONG>(size),digest.data())!=nullptr;
#elif !defined(NX_DISABLE_NATIVE_SHA256) && defined(NX_USE_OPENSSL)
    unsigned written=0;
    done=EVP_Digest(data,size,digest.data(),&written,EVP_sha256(),nullptr)==1 && written==32;
#endif
    if (!done) digest=Sha256::digest(data,size);
    std::memcpy(output,digest.data(),digest.size());
    return true;
}
} // namespace nx
#endif
