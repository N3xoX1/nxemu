/*
* Updated to C++, zedwood.com 2012
* Based on Olivier Gay's version
* See Modified BSD License below:
*
* FIPS 180-2 SHA-224/256/384/512 implementation
* Issue date:  04/30/2005
* http://www.ouah.org/ogay/sha2/
*
* Copyright (C) 2005, 2007 Olivier Gay <olivier.gay@a3.epfl.ch>
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
* 1. Redistributions of source code must retain the above copyright
*    notice, this list of conditions and the following disclaimer.
* 2. Redistributions in binary form must reproduce the above copyright
*    notice, this list of conditions and the following disclaimer in the
*    documentation and/or other materials provided with the distribution.
* 3. Neither the name of the project nor the names of its contributors
*    may be used to endorse or promote products derived from this software
*    without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
* ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
* OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
* HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
* LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
* OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
* SUCH DAMAGE.
*/
#include "sha256.h"
#include <cstring>
#include <fstream>

// The CPU's own SHA-256 instructions (x86 SHA-NI, ARMv8 SHA2) when the host has
// them. They are plain instructions in this file: no system or crypto library.
#if defined(_M_X64) || defined(__x86_64__)
#define SHA256_X86
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#if defined(__GNUC__) || defined(__clang__)
#define SHA256_X86_TARGET __attribute__((target("sha,sse4.1,ssse3")))
#else
#define SHA256_X86_TARGET
#endif
#elif defined(__aarch64__) && (defined(__APPLE__) || defined(__linux__))
#if defined(__ARM_FEATURE_SHA2) || defined(__ARM_FEATURE_CRYPTO)
#define SHA256_ARM64
#define SHA256_ARM64_TARGET
#elif defined(__clang__) && __clang_major__ >= 18
// Older clang only declares the SHA2 intrinsics when the whole file is built for them.
#define SHA256_ARM64
#define SHA256_ARM64_TARGET __attribute__((target("sha2")))
#elif defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 10
#define SHA256_ARM64
#define SHA256_ARM64_TARGET __attribute__((target("+crypto")))
#endif
#ifdef SHA256_ARM64
#include <arm_neon.h>
#ifdef __linux__
#include <sys/auxv.h>
#ifndef HWCAP_SHA2
#define HWCAP_SHA2 (1 << 6)
#endif
#endif
#endif
#endif

#define SHA2_SHFR(x, n) (x >> n)
#define SHA2_ROTR(x, n) ((x >> n) | (x << ((sizeof(x) << 3) - n)))
#define SHA2_ROTL(x, n) ((x << n) | (x >> ((sizeof(x) << 3) - n)))
#define SHA2_CH(x, y, z) ((x & y) ^ (~x & z))
#define SHA2_MAJ(x, y, z) ((x & y) ^ (x & z) ^ (y & z))
#define SHA256_F1(x) (SHA2_ROTR(x, 2) ^ SHA2_ROTR(x, 13) ^ SHA2_ROTR(x, 22))
#define SHA256_F2(x) (SHA2_ROTR(x, 6) ^ SHA2_ROTR(x, 11) ^ SHA2_ROTR(x, 25))
#define SHA256_F3(x) (SHA2_ROTR(x, 7) ^ SHA2_ROTR(x, 18) ^ SHA2_SHFR(x, 3))
#define SHA256_F4(x) (SHA2_ROTR(x, 17) ^ SHA2_ROTR(x, 19) ^ SHA2_SHFR(x, 10))
#define SHA2_UNPACK32(x, str)                \
    {                                        \
        *((str) + 3) = (uint8_t)((x));       \
        *((str) + 2) = (uint8_t)((x) >> 8);  \
        *((str) + 1) = (uint8_t)((x) >> 16); \
        *((str) + 0) = (uint8_t)((x) >> 24); \
    }
#define SHA2_PACK32(str, x)                                                                                                                        \
    {                                                                                                                                              \
        *(x) = ((uint32_t) * ((str) + 3)) | ((uint32_t) * ((str) + 2) << 8) | ((uint32_t) * ((str) + 1) << 16) | ((uint32_t) * ((str) + 0) << 24); \
    }

const unsigned int SHA256::sha256_k[64] = //UL = uint32
    {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
     0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
     0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
     0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
     0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
     0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
     0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
     0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
     0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
     0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
     0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
     0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
     0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
     0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
     0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
     0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#if defined(SHA256_X86)
static bool HasShaInstructions()
{
#ifdef _MSC_VER
    int info[4];
    __cpuid(info, 0);
    if (info[0] < 7)
    {
        return false;
    }
    __cpuid(info, 1);
    const bool ssse3_sse41 = (info[2] & (1 << 9)) != 0 && (info[2] & (1 << 19)) != 0;
    __cpuidex(info, 7, 0);
    return ssse3_sse41 && (info[1] & (1 << 29)) != 0;
#else
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid_max(0, nullptr) < 7 || !__get_cpuid(1, &eax, &ebx, &ecx, &edx))
    {
        return false;
    }
    const bool ssse3_sse41 = (ecx & (1u << 9)) != 0 && (ecx & (1u << 19)) != 0;
    __cpuid_count(7, 0, eax, ebx, ecx, edx);
    return ssse3_sse41 && (ebx & (1u << 29)) != 0;
#endif
}

// State is kept as ABEF/CDGH, the order SHA256RNDS2 works on.
SHA256_X86_TARGET static void TransformSha(uint32_t * state, const uint32_t * k, const unsigned char * message, size_t block_nb)
{
    const __m128i mask = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
    __m128i tmp = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)&state[0]), 0xB1);
    __m128i state1 = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)&state[4]), 0x1B);
    __m128i state0 = _mm_alignr_epi8(tmp, state1, 8);
    state1 = _mm_blend_epi16(state1, tmp, 0xF0);
    // Four rounds per step. msg0-3 hold the last four schedule words, and a step
    // finishes the next words and starts those three steps ahead. Written out in
    // full: MSVC keeps a loop's message words in memory, about 30% slower.
#define SHA256_ROUNDS(i, words)                                                    \
    wk = _mm_add_epi32(words, _mm_loadu_si128((const __m128i *)&k[4 * (i)]));      \
    state1 = _mm_sha256rnds2_epu32(state1, state0, wk);                            \
    state0 = _mm_sha256rnds2_epu32(state0, state1, _mm_shuffle_epi32(wk, 0x0E))
#define SHA256_FINISH(next, words, previous) \
    next = _mm_sha256msg2_epu32(_mm_add_epi32(next, _mm_alignr_epi8(words, previous, 4)), words)
#define SHA256_START(previous, words) previous = _mm_sha256msg1_epu32(previous, words)
    for (size_t block = 0; block < block_nb; block++, message += 64)
    {
        const __m128i abef = state0, cdgh = state1;
        __m128i wk;
        __m128i msg0 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(message + 0)), mask);
        __m128i msg1 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(message + 16)), mask);
        __m128i msg2 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(message + 32)), mask);
        __m128i msg3 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(message + 48)), mask);
        SHA256_ROUNDS(0, msg0);
        SHA256_ROUNDS(1, msg1); SHA256_START(msg0, msg1);
        SHA256_ROUNDS(2, msg2); SHA256_START(msg1, msg2);
        SHA256_ROUNDS(3, msg3); SHA256_FINISH(msg0, msg3, msg2); SHA256_START(msg2, msg3);
        SHA256_ROUNDS(4, msg0); SHA256_FINISH(msg1, msg0, msg3); SHA256_START(msg3, msg0);
        SHA256_ROUNDS(5, msg1); SHA256_FINISH(msg2, msg1, msg0); SHA256_START(msg0, msg1);
        SHA256_ROUNDS(6, msg2); SHA256_FINISH(msg3, msg2, msg1); SHA256_START(msg1, msg2);
        SHA256_ROUNDS(7, msg3); SHA256_FINISH(msg0, msg3, msg2); SHA256_START(msg2, msg3);
        SHA256_ROUNDS(8, msg0); SHA256_FINISH(msg1, msg0, msg3); SHA256_START(msg3, msg0);
        SHA256_ROUNDS(9, msg1); SHA256_FINISH(msg2, msg1, msg0); SHA256_START(msg0, msg1);
        SHA256_ROUNDS(10, msg2); SHA256_FINISH(msg3, msg2, msg1); SHA256_START(msg1, msg2);
        SHA256_ROUNDS(11, msg3); SHA256_FINISH(msg0, msg3, msg2); SHA256_START(msg2, msg3);
        SHA256_ROUNDS(12, msg0); SHA256_FINISH(msg1, msg0, msg3); SHA256_START(msg3, msg0);
        SHA256_ROUNDS(13, msg1); SHA256_FINISH(msg2, msg1, msg0);
        SHA256_ROUNDS(14, msg2); SHA256_FINISH(msg3, msg2, msg1);
        SHA256_ROUNDS(15, msg3);
        state0 = _mm_add_epi32(state0, abef);
        state1 = _mm_add_epi32(state1, cdgh);
    }
#undef SHA256_ROUNDS
#undef SHA256_FINISH
#undef SHA256_START
    tmp = _mm_shuffle_epi32(state0, 0x1B);
    state1 = _mm_shuffle_epi32(state1, 0xB1);
    _mm_storeu_si128((__m128i *)&state[0], _mm_blend_epi16(tmp, state1, 0xF0));
    _mm_storeu_si128((__m128i *)&state[4], _mm_alignr_epi8(state1, tmp, 8));
}
#elif defined(SHA256_ARM64)
static bool HasShaInstructions()
{
#ifdef __APPLE__
    return true; // Every Apple ARM64 CPU has them.
#else
    return (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0;
#endif
}

SHA256_ARM64_TARGET static void TransformSha(uint32_t * state, const uint32_t * k, const unsigned char * message, size_t block_nb)
{
    uint32x4_t state0 = vld1q_u32(&state[0]);
    uint32x4_t state1 = vld1q_u32(&state[4]);
    for (size_t block = 0; block < block_nb; block++, message += 64)
    {
        const uint32x4_t abcd = state0, efgh = state1;
        uint32x4_t msg[4];
        for (int i = 0; i < 4; i++)
        {
            msg[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(message + 16 * i)));
        }
        for (int i = 0; i < 16; i++)
        {
            const uint32x4_t words = msg[i & 3];
            const uint32x4_t wk = vaddq_u32(words, vld1q_u32(&k[4 * i]));
            if (i < 12)
            {
                msg[i & 3] = vsha256su1q_u32(vsha256su0q_u32(words, msg[(i + 1) & 3]), msg[(i + 2) & 3], msg[(i + 3) & 3]);
            }
            const uint32x4_t previous = state0;
            state0 = vsha256hq_u32(state0, state1, wk);
            state1 = vsha256h2q_u32(state1, previous, wk);
        }
        state0 = vaddq_u32(state0, abcd);
        state1 = vaddq_u32(state1, efgh);
    }
    vst1q_u32(&state[0], state0);
    vst1q_u32(&state[4], state1);
}
#endif

void SHA256::transform(const unsigned char * message, size_t block_nb)
{
#if defined(SHA256_X86) || defined(SHA256_ARM64)
    static const bool sha_instructions = HasShaInstructions();
    if (sha_instructions)
    {
        TransformSha(m_h, sha256_k, message, block_nb);
        return;
    }
#endif
    uint32_t w[64];
    uint32_t wv[8];
    uint32_t t1, t2;
    const unsigned char * sub_block;
    size_t i;
    int j;
    for (i = 0; i < block_nb; i++)
    {
        sub_block = message + (i << 6);
        for (j = 0; j < 16; j++)
        {
            SHA2_PACK32(&sub_block[j << 2], &w[j]);
        }
        for (j = 16; j < 64; j++)
        {
            w[j] = SHA256_F4(w[j - 2]) + w[j - 7] + SHA256_F3(w[j - 15]) + w[j - 16];
        }
        for (j = 0; j < 8; j++)
        {
            wv[j] = m_h[j];
        }
        for (j = 0; j < 64; j++)
        {
            t1 = wv[7] + SHA256_F2(wv[4]) + SHA2_CH(wv[4], wv[5], wv[6]) + sha256_k[j] + w[j];
            t2 = SHA256_F1(wv[0]) + SHA2_MAJ(wv[0], wv[1], wv[2]);
            wv[7] = wv[6];
            wv[6] = wv[5];
            wv[5] = wv[4];
            wv[4] = wv[3] + t1;
            wv[3] = wv[2];
            wv[2] = wv[1];
            wv[1] = wv[0];
            wv[0] = t1 + t2;
        }
        for (j = 0; j < 8; j++)
        {
            m_h[j] += wv[j];
        }
    }
}

void SHA256::init()
{
    m_h[0] = 0x6a09e667;
    m_h[1] = 0xbb67ae85;
    m_h[2] = 0x3c6ef372;
    m_h[3] = 0xa54ff53a;
    m_h[4] = 0x510e527f;
    m_h[5] = 0x9b05688c;
    m_h[6] = 0x1f83d9ab;
    m_h[7] = 0x5be0cd19;
    m_len = 0;
    m_tot_len = 0;
}

void SHA256::update(const unsigned char * message, size_t len)
{
    size_t block_nb;
    size_t new_len, rem_len, tmp_len;
    const unsigned char * shifted_message;
    tmp_len = SHA224_256_BLOCK_SIZE - m_len;
    rem_len = len < tmp_len ? len : tmp_len;
    memcpy(&m_block[m_len], message, rem_len);
    if (m_len + len < SHA224_256_BLOCK_SIZE)
    {
        m_len += (unsigned int)len;
        return;
    }
    new_len = len - rem_len;
    block_nb = new_len / SHA224_256_BLOCK_SIZE;
    shifted_message = message + rem_len;
    transform(m_block, 1);
    transform(shifted_message, block_nb);
    rem_len = new_len % SHA224_256_BLOCK_SIZE;
    memcpy(m_block, &shifted_message[block_nb << 6], rem_len);
    m_len = (unsigned int)rem_len;
    m_tot_len += (uint64_t)(block_nb + 1) << 6;
}

void SHA256::final(unsigned char * digest)
{
    unsigned int block_nb;
    unsigned int pm_len;
    uint64_t len_b;
    int i;
    block_nb = (1 + ((SHA224_256_BLOCK_SIZE - 9) < (m_len % SHA224_256_BLOCK_SIZE)));
    len_b = (m_tot_len + m_len) << 3;
    pm_len = block_nb << 6;
    memset(m_block + m_len, 0, pm_len - m_len);
    m_block[m_len] = 0x80;
    SHA2_UNPACK32((uint32_t)(len_b >> 32), m_block + pm_len - 8);
    SHA2_UNPACK32((uint32_t)len_b, m_block + pm_len - 4);
    transform(m_block, block_nb);
    for (i = 0; i < 8; i++)
    {
        SHA2_UNPACK32(m_h[i], &digest[i << 2]);
    }
}

std::string sha256(std::string input)
{
    unsigned char digest[SHA256::DIGEST_SIZE];
    memset(digest, 0, SHA256::DIGEST_SIZE);

    SHA256 ctx = SHA256();
    ctx.init();
    ctx.update((unsigned char *)input.c_str(), (unsigned int)input.length());
    ctx.final(digest);

    char buf[2 * SHA256::DIGEST_SIZE + 1];
    buf[2 * SHA256::DIGEST_SIZE] = 0;
    for (int i = 0; i < SHA256::DIGEST_SIZE; i++)
        sprintf(buf + i * 2, "%02x", digest[i]);
    return std::string(buf);
}
