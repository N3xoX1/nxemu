// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstring>

namespace Core::NCE::ReadThunk {
inline constexpr unsigned PlainTrap = 0x7d00;
inline constexpr unsigned SpilledTrap = 0x7e00;
inline constexpr unsigned CompletedTrap = 0x7f00;

// Integer unsigned-immediate loads and scalar FP LDR S/D. SP addressing and
// other SIMD widths retain their existing route. Vector destinations include V31.
constexpr bool Floating(std::uint32_t instruction) {
    return (instruction & 0x3fc00000u) == 0x3d400000u && (instruction >> 30) >= 2;
}
constexpr bool Eligible(std::uint32_t instruction) {
    return ((instruction >> 5) & 31) != 31 &&
        (((instruction & 0x3fc00000u) == 0x39400000u && (instruction & 31) != 31) || Floating(instruction));
}
constexpr bool Basic(std::uint32_t instruction) {
    return Eligible(instruction) && !Floating(instruction) && ((instruction >> 5) & 31) == 0 && (instruction & 31) != 0;
}
constexpr unsigned Scratch(std::uint32_t instruction) {
    const unsigned rt = instruction & 31, rn = (instruction >> 5) & 31;
    return Floating(instruction) ? (rn == 0 ? 1 : 0) : rt != rn ? rt : rn == 0 ? 1 : 0;
}

constexpr bool IsTrap(std::uint32_t instruction) {
    return instruction == (0xd4200000u | (PlainTrap << 5)) ||
           instruction == (0xd4200000u | (SpilledTrap << 5)) ||
           instruction == (0xd4200000u | (CompletedTrap << 5));
}

// Memory is the production IMemory interface; the template also permits independent
// page/byte oracles in validation without depending on Darwin signal-frame layout.
template<class Memory, class Registers>
void Complete(Memory& memory, Registers& regs, std::uint64_t& sp, std::uint64_t& pc,
              std::uint64_t original_pc, std::uint32_t instruction, bool spilled, bool completed = false, void* vector_data = nullptr) {
    const unsigned rt = instruction & 31;
    if (spilled) {
        regs[Scratch(instruction)] = memory.Read64(sp);
        sp += 16;
    }
    pc = original_pc + 4;
    if (completed) return; // The load succeeded; only its synthetic restore was interrupted.
    const unsigned bytes = 1u << (instruction >> 30);
    const auto address = regs[(instruction >> 5) & 31] + (((instruction >> 10) & 0xfffu) * bytes);
    const auto last = address + bytes - 1;
    // Keep the existing backend's denied-access behavior: skip, preserving old Rt.
    if (last < address || !memory.NceAccessAllowed(address, false) ||
        !memory.NceAccessAllowed(last, false)) return;
    memory.NceTrackReads(address);
    if ((address >> 12) != (last >> 12)) memory.NceTrackReads(last);
    std::uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i)
        value |= std::uint64_t{memory.Read8(address + i)} << (i * 8);
    if (Floating(instruction)) {
        // Architectural LDR S/D zeroes the rest of the 128-bit destination.
        const std::uint64_t result[2]{value, 0};
        std::memcpy(static_cast<unsigned char*>(vector_data) + 16 * rt, result, sizeof(result));
    } else regs[rt] = value;
}
} // namespace Core::NCE::ReadThunk
