// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

void Require(bool ok) { if (!ok) std::abort(); }
namespace Xbyak {
enum { ERR_CODE_IS_TOO_BIG, ERR_CANT_ALLOC };
struct Error { int code; explicit Error(int value) : code(value) {} };
}
constexpr size_t MiB = 1024 * 1024;
constexpr size_t COMMIT_STEP = 4 * MiB;
using DWORD = unsigned long;
constexpr DWORD PAGE_READWRITE = 4, PAGE_EXECUTE_READWRITE = 64, MEM_COMMIT = 4096;
size_t calls{}, last_size{};
void* last_address{};
DWORD last_protection{};
bool fail{};
void* VirtualAlloc(void* address, size_t size, DWORD kind, DWORD protection) {
    Require(kind == MEM_COMMIT && size != 0);
    ++calls; last_address = address; last_size = size; last_protection = protection;
    return fail ? nullptr : address;
}
struct BlockOfCode {
    size_t size_{}, maxSize_{}, committed_size{};
    unsigned char* top_{};
    void EnsureMemoryCommitted(size_t codesize);
};
#ifndef _WIN32
#define _WIN32
#endif
#include "commit.inc"

template<class F> void Throws(F f, int code) {
    bool caught{};
    try { f(); } catch (Xbyak::Error e) { caught = e.code == code; }
    Require(caught);
}
int main() {
    std::vector<unsigned char> storage(64 * MiB);
    BlockOfCode cache{2 * MiB, storage.size(), 0, storage.data()};
    cache.EnsureMemoryCommitted(16 * MiB);
    Require(cache.committed_size == 20 * MiB && calls == 1 && last_size == 20 * MiB);
#ifdef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
    Require(last_protection == PAGE_READWRITE);
#else
    Require(last_protection == PAGE_EXECUTE_READWRITE);
#endif
    cache.size_ = 19 * MiB;
    cache.EnsureMemoryCommitted(MiB);
    Require(calls == 1); // No system call inside an already committed batch.
    cache.size_ = 20 * MiB;
    cache.EnsureMemoryCommitted(17 * MiB);
    Require(cache.committed_size == 40 * MiB && calls == 2);
    Require(last_address == storage.data() + 20 * MiB && last_size == 20 * MiB);
    cache.size_ = 40 * MiB;
    fail = true;
    Throws([&] { cache.EnsureMemoryCommitted(1); }, Xbyak::ERR_CANT_ALLOC);
    Require(cache.committed_size == 40 * MiB);
    fail = false;
    cache.EnsureMemoryCommitted(1);
    Require(cache.committed_size == 44 * MiB);
    cache.maxSize_ = 63 * MiB + 1;
    cache.size_ = cache.maxSize_ - 1;
    cache.EnsureMemoryCommitted(1);
    Require(cache.committed_size == cache.maxSize_); // Clamp the last batch.
    const auto before = calls;
    cache.size_ = cache.maxSize_;
    cache.EnsureMemoryCommitted(0);
    Throws([&] { cache.EnsureMemoryCommitted(1); }, Xbyak::ERR_CODE_IS_TOO_BIG);
    cache.size_++;
    Throws([&] { cache.EnsureMemoryCommitted(0); }, Xbyak::ERR_CODE_IS_TOO_BIG);
    cache.size_ = 1;
    Throws([&] { cache.EnsureMemoryCommitted(std::numeric_limits<size_t>::max()); },
           Xbyak::ERR_CODE_IS_TOO_BIG);
    Require(calls == before);
    // Already committed bounds near SIZE_MAX exercise arithmetic without pointer additions.
    cache.maxSize_ = cache.committed_size = std::numeric_limits<size_t>::max();
    cache.size_ = cache.maxSize_ - 1;
    cache.EnsureMemoryCommitted(1);
    Throws([&] { cache.EnsureMemoryCommitted(2); }, Xbyak::ERR_CODE_IS_TOO_BIG);
    cache.committed_size = 0;
    cache.EnsureMemoryCommitted(1);
    Require(cache.committed_size == std::numeric_limits<size_t>::max());
    Require(last_size == cache.committed_size); // Rounding cannot wrap near SIZE_MAX.
    std::puts("Windows commit bounds, batches, protections and allocation failure passed");
}
