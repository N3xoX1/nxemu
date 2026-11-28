// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstring>

namespace Core::NCE::ReadThunk
{
inline constexpr unsigned PlainTrap = 0x7d00;
inline constexpr unsigned SpilledTrap = 0x7e00;
inline constexpr unsigned CompletedTrap = 0x7f00;

// Integer and scalar FP loads: unsigned immediate (LDRB/LDRH/LDR W/X/S/D),
// unscaled signed immediate (LDURB/LDURH/LDUR W/X/S/D), register offset (LDR [Xn, Xm/Wm]),
// and signed-offset integer pairs (LDP W/X). SP addressing and other SIMD widths retain
// their existing route. Vector destinations include V31.
constexpr bool IntPair(std::uint32_t instruction)
{
    const unsigned rt = instruction & 31, rt2 = (instruction >> 10) & 31;
    return (instruction & 0x7fc00000u) == 0x29400000u && rt != 31 && rt2 != 31 && rt != rt2;
}
constexpr bool Unscaled(std::uint32_t instruction)
{
    return ((instruction & 0x3fe00c00u) == 0x38400000u && (instruction & 31) != 31) ||
           ((instruction & 0x3fe00c00u) == 0x3c400000u && (instruction >> 30) >= 2);
}
constexpr bool Indexed(std::uint32_t instruction)
{
    return ((instruction >> 13) & 2) != 0 &&
           (((instruction & 0x3fe00c00u) == 0x38600800u && (instruction & 31) != 31) ||
            ((instruction & 0x3fe00c00u) == 0x3c600800u && (instruction >> 30) >= 2));
}
constexpr bool Floating(std::uint32_t instruction)
{
    return (((instruction & 0x3fc00000u) == 0x3d400000u) ||
            ((instruction & 0x3fe00c00u) == 0x3c400000u) ||
            ((instruction & 0x3fe00c00u) == 0x3c600800u && ((instruction >> 13) & 2) != 0)) &&
           (instruction >> 30) >= 2;
}
constexpr bool Eligible(std::uint32_t instruction)
{
    return ((instruction >> 5) & 31) != 31 &&
           (((instruction & 0x3fc00000u) == 0x39400000u && (instruction & 31) != 31) ||
            Unscaled(instruction) || Indexed(instruction) || IntPair(instruction) ||
            Floating(instruction));
}
constexpr bool Basic(std::uint32_t instruction)
{
    return ((instruction & 0x3fc00000u) == 0x39400000u) && ((instruction >> 5) & 31) == 0 &&
           (instruction & 31) != 0 && (instruction & 31) != 31;
}
constexpr unsigned Bytes(std::uint32_t instruction)
{
    return IntPair(instruction) ? (8u << (instruction >> 31)) : (1u << (instruction >> 30));
}
constexpr std::int64_t Offset(std::uint32_t instruction)
{
    if (Indexed(instruction)) return 0;
    if (IntPair(instruction))
    {
        const int n = (instruction >> 15) & 127;
        return std::int64_t(n < 64 ? n : n - 128) * (4 << (instruction >> 31));
    }
    if (Unscaled(instruction))
    {
        const int n = (instruction >> 12) & 0x1ff;
        return std::int64_t(n < 256 ? n : n - 512);
    }
    return std::int64_t(((instruction >> 10) & 0xfffu) * Bytes(instruction));
}
constexpr unsigned Scratch(std::uint32_t instruction)
{
    const unsigned rt = instruction & 31, rn = (instruction >> 5) & 31;
    if (IntPair(instruction))
    {
        const unsigned rt2 = (instruction >> 10) & 31;
        unsigned r = 0;
        while (r == rn || r == rt || r == rt2) ++r;
        return r;
    }
    if (Indexed(instruction))
    {
        const unsigned rm = (instruction >> 16) & 31;
        unsigned r = 0;
        while (r == rn || r == rm || (!Floating(instruction) && r == rt)) ++r;
        return r;
    }
    return Floating(instruction) ? (rn == 0 ? 1 : 0) : rt != rn ? rt
                                                   : rn == 0    ? 1
                                                                : 0;
}
template <class Registers>
std::uint64_t Address(std::uint32_t instruction, Registers & regs)
{
    auto result = regs[(instruction >> 5) & 31] + std::uint64_t(Offset(instruction));
    if (Indexed(instruction))
    {
        const unsigned rm = (instruction >> 16) & 31, option = (instruction >> 13) & 7;
        std::uint64_t offset = rm == 31 ? 0 : regs[rm];
        if (option == 2) offset = std::uint32_t(offset);
        if (option == 6) offset = std::uint64_t(std::int64_t(std::int32_t(offset)));
        const unsigned shift = (instruction & (1u << 12)) ? (instruction >> 30) : 0;
        result += offset << shift;
    }
    return result;
}

constexpr bool IsTrap(std::uint32_t instruction)
{
    return instruction == (0xd4200000u | (PlainTrap << 5)) ||
           instruction == (0xd4200000u | (SpilledTrap << 5)) ||
           instruction == (0xd4200000u | (CompletedTrap << 5));
}

// Memory is the production IMemory interface; the template also permits independent
// page/byte oracles in validation without depending on Darwin signal-frame layout.
template <class Memory, class Registers>
void Complete(Memory & memory, Registers & regs, std::uint64_t & sp, std::uint64_t & pc,
              std::uint64_t original_pc, std::uint32_t instruction, bool spilled, bool completed = false, void * vector_data = nullptr)
{
    const unsigned rt = instruction & 31;
    if (spilled)
    {
        regs[Scratch(instruction)] = memory.Read64(sp);
        sp += 16;
    }
    pc = original_pc + 4;
    if (completed) return; // The load succeeded; only its synthetic restore was interrupted.
    const unsigned bytes = Bytes(instruction);
    const auto address = Address(instruction, regs);
    const auto last = address + bytes - 1;
    // Keep the existing backend's denied-access behavior: skip, preserving old Rt.
    if (last < address || !memory.NceAccessAllowed(address, false) ||
        !memory.NceAccessAllowed(last, false)) return;
    memory.NceTrackReads(address);
    if ((address >> 12) != (last >> 12)) memory.NceTrackReads(last);
    unsigned char buf[16]{};
    if constexpr (requires { memory.ReadBlock(address, buf, bytes); })
    {
        memory.ReadBlock(address, buf, bytes);
    }
    else
    {
        for (unsigned i = 0; i < bytes; ++i)
            buf[i] = memory.Read8(address + i);
    }
    if (IntPair(instruction))
    {
        const unsigned elem = 4u << (instruction >> 31);
        const unsigned rt2 = (instruction >> 10) & 31;
        std::uint64_t v1 = 0, v2 = 0;
        std::memcpy(&v1, buf, elem);
        std::memcpy(&v2, buf + elem, elem);
        regs[rt] = v1;
        regs[rt2] = v2;
    }
    else if (Floating(instruction))
    {
        std::uint64_t value = 0;
        std::memcpy(&value, buf, bytes);
        // Architectural LDR S/D zeroes the rest of the 128-bit destination.
        const std::uint64_t result[2]{value, 0};
        std::memcpy(static_cast<unsigned char *>(vector_data) + 16 * rt, result, sizeof(result));
    }
    else
    {
        std::uint64_t value = 0;
        std::memcpy(&value, buf, bytes);
        regs[rt] = value;
    }
}
} // namespace Core::NCE::ReadThunk
