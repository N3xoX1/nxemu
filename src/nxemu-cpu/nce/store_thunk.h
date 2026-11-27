// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstring>

namespace Core::NCE::StoreThunk
{
enum class Trap : unsigned
{
    Prepare = 0x7b00,
    Plain,
    Spilled,
    Completed
};
constexpr bool IsTrap(std::uint32_t i)
{
    return (i & 0xffe0001fu) == 0xd4200000u &&
           ((i >> 5) & 0xfffcu) == unsigned(Trap::Prepare);
}
constexpr bool Cache(std::uint32_t i)
{
    const std::uint32_t op = i & ~31u;
    // Data cache maintenance only (never IC IVAU, which must target executable VA).
    return op == 0xd50b7e20u || // DC CIVAC
           op == 0xd50b7a20u || // DC CVAC
           op == 0xd50b7b20u || // DC CVAU
           op == 0xd50b7c20u || // DC CVAP
           op == 0xd50b7d20u;   // DC CVADP
}
inline void ExecuteCache(std::uint32_t i, void * pointer)
{
    if (!pointer) return;
#if defined(__aarch64__)
    const std::uint32_t op = i & ~31u;
    if (op == 0xd50b7e20u)
        asm volatile("dc civac, %0" : : "r"(pointer) : "memory");
    else if (op == 0xd50b7b20u)
        asm volatile("dc cvau, %0" : : "r"(pointer) : "memory");
    else
        asm volatile("dc cvac, %0" : : "r"(pointer) : "memory"); // CVAC/CVAP/CVADP
#else
    (void)i;
#endif
}
constexpr bool Pair(std::uint32_t i)
{
    return (i & 0xffc00000u) == 0xad000000u;
}
constexpr bool IntPair(std::uint32_t i)
{
    return (i & 0x7fc00000u) == 0x29000000u;
}
constexpr bool Indexed(std::uint32_t i)
{
    return (i & 0x3fe00c00u) == 0x3c200800u && (i >> 30) >= 2 && ((i >> 13) & 2);
}
constexpr bool Floating(std::uint32_t i)
{
    return Pair(i) || Indexed(i) || ((i & 0x3fc00000u) == 0x3d000000u && (i >> 30) >= 2);
}
constexpr unsigned Base(std::uint32_t i)
{
    return Cache(i) ? i & 31 : (i >> 5) & 31;
}
constexpr bool Eligible(std::uint32_t i)
{
    return Base(i) != 31 && (Cache(i) || Pair(i) || IntPair(i) || Indexed(i) || Floating(i) || (i & 0x3fc00000u) == 0x39000000u);
}
constexpr unsigned Scratch(std::uint32_t i)
{
    unsigned r = 0;
    while (r == Base(i) || (!Floating(i) && !Cache(i) && r == (i & 31)) ||
           (IntPair(i) && r == ((i >> 10) & 31)) ||
           (Indexed(i) && r == ((i >> 16) & 31)))
        ++r;
    return r;
}
constexpr unsigned Bytes(std::uint32_t i)
{
    return Cache(i) ? 1 : Pair(i) ? 32
                        : IntPair(i) ? (8u << (i >> 31))
                                     : 1u << (i >> 30);
}
constexpr std::int64_t Offset(std::uint32_t i)
{
    if (Cache(i) || Indexed(i)) return 0;
    if (Pair(i) || IntPair(i))
    {
        const int n = (i >> 15) & 127;
        const int scale = Pair(i) ? 16 : (4 << (i >> 31));
        return std::int64_t(n < 64 ? n : n - 128) * scale;
    }
    return ((i >> 10) & 4095) * Bytes(i);
}
template <class Registers>
std::uint64_t Address(std::uint32_t i, Registers & r)
{
    auto result = r[Base(i)] + std::uint64_t(Offset(i));
    if (Indexed(i))
    {
        const unsigned rm = (i >> 16) & 31, option = (i >> 13) & 7;
        std::uint64_t offset = rm == 31 ? 0 : r[rm];
        if (option == 2) offset = std::uint32_t(offset);
        if (option == 6) offset = std::uint64_t(std::int64_t(std::int32_t(offset)));
        const unsigned shift = (i & (1u << 12)) ? i >> 30 : 0;
        result += offset << shift;
    }
    return result;
}
// A private trap either obtains a write lease and retries, or executes the
// existing byte-wise memory route. A completed store must never be replayed.
template <class Memory, class Registers>
void Complete(Memory & memory, Registers & r, std::uint64_t & sp, std::uint64_t & pc,
              std::uint64_t original_pc, std::uint32_t i, Trap trap, const void * vectors)
{
    if (trap == Trap::Spilled || trap == Trap::Completed)
    {
        r[Scratch(i)] = memory.Read64(sp);
        sp += 16;
    }
    pc = original_pc + 4;
    if (trap == Trap::Completed) return;
    const auto address = Address(i, r);
    if (Cache(i))
    {
        // Cache maintenance (DC CIVAC/CVAC/CVAU) does not write guest memory.
        // Never call NcePrepareCanonicalWrite/NceTrackWrites, which would open
        // GPU-read-cached pages as ReadWrite without downloading them first.
        const bool write = (i & ~31u) == 0xd50b7e20u;
        if (memory.NceAccessAllowed(address, write))
        {
            ExecuteCache(i, memory.GetPointerSilent(address));
        }
        return;
    }
    const unsigned bytes = Bytes(i);
    const auto last = address + bytes - 1;
    if (last < address || !memory.NceAccessAllowed(address, true) || !memory.NceAccessAllowed(last, true)) return;
    const bool single_page = (address >> 12) == (last >> 12);
    const bool tried_prepare = trap == Trap::Prepare && address < (1ULL << 39) &&
                               (address & 0xfff) + bytes <= 0x1000 &&
                               memory.NceAccessAllowed(address, false);
    if (tried_prepare && memory.NcePrepareCanonicalWrite(address))
    {
        pc = original_pc;
        return;
    }
    if (!tried_prepare)
    {
        memory.NceTrackWrites(address);
    }
    if (!single_page) memory.NceTrackWrites(last);
    unsigned char payload[32]{};
    const unsigned rt = i & 31;
    if (Floating(i))
    {
        std::memcpy(payload, static_cast<const unsigned char *>(vectors) + 16 * rt, Pair(i) ? 16 : bytes);
        if (Pair(i)) std::memcpy(payload + 16, static_cast<const unsigned char *>(vectors) + 16 * ((i >> 10) & 31), 16);
    }
    else if (IntPair(i))
    {
        const unsigned elem = 4u << (i >> 31);
        const unsigned rt2 = (i >> 10) & 31;
        const std::uint64_t v1 = rt == 31 ? 0 : r[rt];
        const std::uint64_t v2 = rt2 == 31 ? 0 : r[rt2];
        std::memcpy(payload, &v1, elem);
        std::memcpy(payload + elem, &v2, elem);
    }
    else
    {
        const std::uint64_t value = rt == 31 ? 0 : r[rt];
        std::memcpy(payload, &value, bytes);
    }
    if constexpr (requires { memory.WriteBlock(address, payload, bytes); })
    {
        memory.WriteBlock(address, payload, bytes);
    }
    else
    {
        for (unsigned b = 0; b < bytes; ++b)
            memory.Write8(address + b, payload[b]);
    }
}
} // namespace Core::NCE::StoreThunk
