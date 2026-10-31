// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstring>

namespace Core::NCE::StoreThunk {
enum class Trap : unsigned { Prepare = 0x7b00, Plain, Spilled, Completed };
constexpr bool IsTrap(std::uint32_t i) {
    return (i & 0xffe0001fu) == 0xd4200000u &&
           ((i >> 5) & 0xfffcu) == unsigned(Trap::Prepare);
}
constexpr bool Cache(std::uint32_t i) {
    return (i & ~31u) == 0xd50b7e20u || (i & ~31u) == 0xd50b7a20u ||
           (i & ~31u) == 0xd50b7b20u;
}
constexpr bool Pair(std::uint32_t i) { return (i & 0xffc00000u) == 0xad000000u; }
constexpr bool Indexed(std::uint32_t i) { return (i & 0x3fe00c00u) == 0x3c200800u && (i >> 30) >= 2 && ((i >> 13) & 2); }
constexpr bool Floating(std::uint32_t i) { return Pair(i) || Indexed(i) || ((i & 0x3fc00000u) == 0x3d000000u && (i >> 30) >= 2); }
constexpr unsigned Base(std::uint32_t i) { return Cache(i) ? i & 31 : (i >> 5) & 31; }
constexpr bool Eligible(std::uint32_t i) {
    return Base(i) != 31 && (Cache(i) || Pair(i) || Indexed(i) || Floating(i) || (i & 0x3fc00000u) == 0x39000000u);
}
constexpr unsigned Scratch(std::uint32_t i) {
    unsigned r = 0;
    while (r == Base(i) || (!Floating(i) && !Cache(i) && r == (i & 31)) ||
           (Indexed(i) && r == ((i >> 16) & 31))) ++r;
    return r;
}
constexpr unsigned Bytes(std::uint32_t i) { return Cache(i) ? 1 : Pair(i) ? 32 : 1u << (i >> 30); }
constexpr std::int64_t Offset(std::uint32_t i) {
    if (Cache(i) || Indexed(i)) return 0;
    if (Pair(i)) {const int n = (i >> 15) & 127;return std::int64_t(n < 64 ? n : n - 128) * 16;}
    return ((i >> 10) & 4095) * Bytes(i);
}
template<class Registers> std::uint64_t Address(std::uint32_t i, Registers& r) {
    auto result = r[Base(i)] + std::uint64_t(Offset(i));
    if (Indexed(i)) {
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
template<class Memory, class Registers>
void Complete(Memory& memory, Registers& r, std::uint64_t& sp, std::uint64_t& pc,
              std::uint64_t original_pc, std::uint32_t i, Trap trap, const void* vectors) {
    if (trap == Trap::Spilled || trap == Trap::Completed) {r[Scratch(i)] = memory.Read64(sp);sp += 16;}
    pc = original_pc + 4;
    if (trap == Trap::Completed) return;
    const auto address = Address(i, r);
    const unsigned bytes = Bytes(i);
    const auto last = address + bytes - 1;
    if (last < address || !memory.NceAccessAllowed(address, true) || !memory.NceAccessAllowed(last, true)) return;
    if (trap == Trap::Prepare && address < (1ULL << 39) &&
        (address & 0xfff) + bytes <= 0x1000 && memory.NceAccessAllowed(address, false) &&
        memory.NcePrepareCanonicalWrite(address)) {pc = original_pc;return;}
    memory.NceTrackWrites(address);
    if ((address >> 12) != (last >> 12)) memory.NceTrackWrites(last);
    if (Cache(i)) {
        auto* pointer = memory.GetPointerSilent(address);
        if (!pointer) return;
#if defined(__aarch64__)
        if ((i & ~31u) == 0xd50b7e20u) asm volatile("dc civac, %0" : : "r"(pointer) : "memory");
        else if ((i & ~31u) == 0xd50b7a20u) asm volatile("dc cvac, %0" : : "r"(pointer) : "memory");
        else asm volatile("dc cvau, %0" : : "r"(pointer) : "memory");
#endif
        return;
    }
    unsigned char payload[32]{};
    const unsigned rt = i & 31;
    if (Floating(i)) {
        std::memcpy(payload, static_cast<const unsigned char*>(vectors) + 16 * rt, Pair(i) ? 16 : bytes);
        if (Pair(i)) std::memcpy(payload + 16, static_cast<const unsigned char*>(vectors) + 16 * ((i >> 10) & 31), 16);
    } else {const std::uint64_t value = rt == 31 ? 0 : r[rt];std::memcpy(payload, &value, bytes);}
    for (unsigned b = 0; b < bytes; ++b) memory.Write8(address + b, payload[b]);
}
} // namespace Core::NCE::StoreThunk
