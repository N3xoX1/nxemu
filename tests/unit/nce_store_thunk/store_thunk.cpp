// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>
#include "nxemu-cpu/nce/store_thunk.h"

namespace {
using namespace Core::NCE;

struct Memory {
    std::vector<std::uint64_t> prepared, tracked, written;
    bool NceAccessAllowed(std::uint64_t, bool) { return true; }
    bool NcePrepareCanonicalWrite(std::uint64_t address) { prepared.push_back(address); return true; }
    bool NceTrackWrites(std::uint64_t address) { tracked.push_back(address); return true; }
    void Write8(std::uint64_t address, std::uint8_t) { written.push_back(address); }
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
    return 0;
}
