/* This file is part of the dynarmic project.
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <optional>
#include <tuple>

#include <mcl/assert.hpp>
#include <mcl/stdint.hpp>
#include <tsl/robin_map.h>

#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend::X64 {

/// The fastmem patch of each guest memory access of the compiled code, found by the rip of the
/// faulting instruction. There is one entry per access (over a million per core in big games), so
/// the code addresses are stored as 32-bit offsets from the start of the code buffer and the
/// table is kept well filled: lookups only happen on fastmem faults.
template<typename Info, typename Marker>
class FastmemPatchTable {
public:
    void SetCodeBase(const void* base) {
        code_base = reinterpret_cast<u64>(base);
        Reset();
    }

    void emplace(u64 rip, const Info& info) {
        Entry entry{
            .resume = Offset(info.resume_rip),
            .callback = Offset(info.callback),
            .location_low = static_cast<u32>(std::get<0>(info.marker).Value()),
            .location_high = static_cast<u32>(std::get<0>(info.marker).Value() >> 32),
            .inst = static_cast<u32>(std::get<1>(info.marker)),
            .recompile = info.recompile,
        };
        table.emplace(Offset(rip), entry);
    }

    std::optional<Info> find(u64 rip) const {
        if (rip < code_base || rip - code_base > UINT32_MAX) {
            return std::nullopt;
        }
        const auto iter = table.find(static_cast<u32>(rip - code_base));
        if (iter == table.end()) {
            return std::nullopt;
        }
        const Entry& entry = iter->second;
        const u64 location = u64{entry.location_low} | (u64{entry.location_high} << 32);
        return Info{
            code_base + entry.resume,
            code_base + entry.callback,
            Marker{IR::LocationDescriptor{location}, entry.inst},
            entry.recompile,
        };
    }

    /// Releases the table with the code it described (clear() would keep the bucket array)
    void clear() {
        Reset();
    }

private:
    static constexpr float LoadFactor = 0.85f;

    struct Entry {
        u32 resume;
        u32 callback;
        u32 location_low;
        u32 location_high;
        u32 inst;
        bool recompile;
    };

    u32 Offset(u64 address) const {
        ASSERT_MSG(address >= code_base && address - code_base <= UINT32_MAX,
                   "fastmem patch address outside the code buffer");
        return static_cast<u32>(address - code_base);
    }

    void Reset() {
        table = {};
        table.max_load_factor(LoadFactor);
    }

    u64 code_base = 0;
    tsl::robin_map<u32, Entry> table;
};

}  // namespace Dynarmic::Backend::X64
