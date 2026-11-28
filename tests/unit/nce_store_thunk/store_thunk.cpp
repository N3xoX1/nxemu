// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>
#include "nxemu-cpu/nce/read_thunk.h"
#include "nxemu-cpu/nce/store_thunk.h"

namespace {
using namespace Core::NCE;

struct Memory {
    std::vector<std::uint64_t> prepared, tracked, written, read_tracked;
    std::unordered_map<std::uint64_t, std::uint8_t> bytes;
    bool NceAccessAllowed(std::uint64_t, bool) { return true; }
    bool NcePrepareCanonicalWrite(std::uint64_t address) { prepared.push_back(address); return true; }
    bool NceTrackWrites(std::uint64_t address) { tracked.push_back(address); return true; }
    bool NceTrackReads(std::uint64_t address) { read_tracked.push_back(address); return true; }
    void Write8(std::uint64_t address, std::uint8_t v) { written.push_back(address); bytes[address] = v; }
    std::uint8_t Read8(std::uint64_t address) { return bytes[address]; }
    std::uint64_t Read64(std::uint64_t) { return 0; }
    void* GetPointerSilent(std::uint64_t) { return nullptr; }
};

struct Registers {
    std::array<std::uint64_t, 32> x{};
    std::uint64_t& operator[](unsigned i) { return x[i]; }
};

// str x1, [x0]
constexpr std::uint32_t Str64 = 0xf9000001u;
constexpr std::uint64_t OriginalPc = 0x80001000;

void Run(Memory& memory, std::uint64_t address, std::uint64_t& pc) {
    Registers registers;
    registers[0] = address;
    std::uint64_t sp = 0x1000;
    std::array<std::uint8_t, 512> vectors{};
    StoreThunk::Complete(memory, registers, sp, pc, OriginalPc, Str64, StoreThunk::Trap::Prepare, vectors.data());
}
} // namespace

int main() {
    static_assert(StoreThunk::Eligible(Str64) && StoreThunk::Bytes(Str64) == 8);

    // An unaligned store inside one 4 KiB page obtains a write lease and retries natively.
    {
        Memory memory;
        std::uint64_t pc = 0;
        Run(memory, 0x10000003, pc);
        assert(pc == OriginalPc);
        assert(memory.prepared.size() == 1 && memory.written.empty());
    }
    // The last 8 bytes of a page still fit.
    {
        Memory memory;
        std::uint64_t pc = 0;
        Run(memory, 0x10000ff8, pc);
        assert(pc == OriginalPc && memory.prepared.size() == 1);
    }
    // A store crossing a page is emulated byte-wise and tracks both pages.
    {
        Memory memory;
        std::uint64_t pc = 0;
        Run(memory, 0x10000ffc, pc);
        assert(pc == OriginalPc + 4);
        assert(memory.prepared.empty() && memory.written.size() == 8);
        assert(memory.tracked.size() == 2 && memory.tracked[0] == 0x10000ffc && memory.tracked[1] == 0x10001003);
    }
    // Data cache maintenance (DC CIVAC/CVAC/CVAU/CVAP/CVADP) never modifies memory
    // and must not enter write tracking, which would expose undownloaded GPU-read-cached pages.
    for (const std::uint32_t insn :
         {0xd50b7e20u, 0xd50b7a20u, 0xd50b7b20u, 0xd50b7c20u, 0xd50b7d20u}) {
        static_assert(StoreThunk::Cache(0xd50b7e20u) && StoreThunk::Cache(0xd50b7a20u) &&
                      StoreThunk::Cache(0xd50b7b20u) && StoreThunk::Cache(0xd50b7c20u) &&
                      StoreThunk::Cache(0xd50b7d20u));
        for (const auto trap : {StoreThunk::Trap::Prepare, StoreThunk::Trap::Plain, StoreThunk::Trap::Spilled}) {
            Memory memory;
            Registers registers;
            registers[0] = 0x10000040;
            std::uint64_t sp = 0x1000, pc = 0;
            std::array<std::uint8_t, 512> vectors{};
            StoreThunk::Complete(memory, registers, sp, pc, OriginalPc, insn, trap, vectors.data());
            assert(pc == OriginalPc + 4);
            assert(memory.prepared.empty() && memory.tracked.empty() && memory.written.empty());
        }
    }
    // Integer STP X/W (signed offset, non-writeback)
    {
        // stp x9, x10, [x8] and stp x11, x12, [x8, #0x10] and stp w8, w10, [x0]
        constexpr std::uint32_t StpX = 0xa9002909u;
        constexpr std::uint32_t StpXOff = 0xa901310bu;
        constexpr std::uint32_t StpW = 0x29002808u;
        constexpr std::uint32_t StpPost = 0xa8812909u;
        static_assert(StoreThunk::Eligible(StpX) && StoreThunk::IntPair(StpX) &&
                      StoreThunk::Bytes(StpX) == 16 && StoreThunk::Offset(StpX) == 0);
        static_assert(StoreThunk::Eligible(StpXOff) && StoreThunk::Offset(StpXOff) == 16);
        static_assert(StoreThunk::Eligible(StpW) && StoreThunk::Bytes(StpW) == 8);
        static_assert(!StoreThunk::Eligible(StpPost));
        Memory memory;
        Registers registers;
        registers[8] = 0x10000ff8; // crosses page boundary -> emulated write
        registers[9] = 0x0123456789abcdefULL;
        registers[10] = 0xfedcba9876543210ULL;
        std::uint64_t sp = 0x1000, pc = 0;
        std::array<std::uint8_t, 512> vectors{};
        StoreThunk::Complete(memory, registers, sp, pc, OriginalPc, StpX, StoreThunk::Trap::Plain, vectors.data());
        assert(pc == OriginalPc + 4 && memory.written.size() == 16);
        std::uint64_t got0 = 0, got1 = 0;
        for (int b = 0; b < 8; ++b) {
            got0 |= std::uint64_t{memory.bytes[0x10000ff8 + b]} << (b * 8);
            got1 |= std::uint64_t{memory.bytes[0x10001000 + b]} << (b * 8);
        }
        assert(got0 == registers[9] && got1 == registers[10]);
    }
    // Extended ReadThunk: LDURH (negative offset), LDR D [Xn, Xm], LDP X (with rt2 == rn)
    {
        constexpr std::uint32_t LdurhNeg2 = 0x785fe1edu; // ldurh w13, [x15, #-2]
        constexpr std::uint32_t LdrDIdx = 0xfc686922u;   // ldr d2, [x9, x8]
        constexpr std::uint32_t LdpX = 0xa941ad6eu;      // ldp x14, x11, [x11, #0x18]
        static_assert(ReadThunk::Eligible(LdurhNeg2) && ReadThunk::Unscaled(LdurhNeg2) &&
                      ReadThunk::Bytes(LdurhNeg2) == 2 && ReadThunk::Offset(LdurhNeg2) == -2);
        static_assert(ReadThunk::Eligible(LdrDIdx) && ReadThunk::Indexed(LdrDIdx) &&
                      ReadThunk::Floating(LdrDIdx) && ReadThunk::Bytes(LdrDIdx) == 8);
        static_assert(ReadThunk::Eligible(LdpX) && ReadThunk::IntPair(LdpX) &&
                      ReadThunk::Bytes(LdpX) == 16 && ReadThunk::Offset(LdpX) == 0x18);
        static_assert(ReadThunk::Scratch(LdpX) != 11 && ReadThunk::Scratch(LdpX) != 14);
        Memory memory;
        memory.bytes[0x200000fe] = 0x34;
        memory.bytes[0x200000ff] = 0x12;
        Registers regs;
        regs[15] = 0x20000100;
        std::uint64_t sp = 0x1000, pc = 0;
        std::array<std::uint8_t, 512> vectors{};
        ReadThunk::Complete(memory, regs, sp, pc, OriginalPc, LdurhNeg2, false, false, vectors.data());
        assert(pc == OriginalPc + 4 && regs[13] == 0x1234);

        const std::uint64_t expected_d = 0x3ff0000000000000ULL;
        for (int b = 0; b < 8; ++b) memory.bytes[0x20000240 + b] = static_cast<std::uint8_t>(expected_d >> (b * 8));
        regs[9] = 0x20000200;
        regs[8] = 0x40;
        ReadThunk::Complete(memory, regs, sp, pc, OriginalPc, LdrDIdx, false, false, vectors.data());
        std::uint64_t got_d[2]{};
        std::memcpy(got_d, vectors.data() + 16 * 2, sizeof(got_d));
        assert(got_d[0] == expected_d && got_d[1] == 0);

        const std::uint64_t p0 = 0x1122334455667788ULL, p1 = 0x99aabbccddeeff00ULL;
        for (int b = 0; b < 8; ++b) {
            memory.bytes[0x20000318 + b] = static_cast<std::uint8_t>(p0 >> (b * 8));
            memory.bytes[0x20000320 + b] = static_cast<std::uint8_t>(p1 >> (b * 8));
        }
        regs[11] = 0x20000300;
        ReadThunk::Complete(memory, regs, sp, pc, OriginalPc, LdpX, false, false, vectors.data());
        assert(regs[14] == p0 && regs[11] == p1);
    }
    return 0;
}
