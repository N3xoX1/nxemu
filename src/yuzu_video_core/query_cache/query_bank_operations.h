// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>

namespace VideoCommon {

// The callback may release the final reference to a bank, allowing another
// thread to reset/relink it immediately. Read the entire traversal state first.
template <typename Query, typename Pool, typename Func>
void ApplyQueryBankOp(const Query& query, Pool& pool, Func&& func) {
    size_t remaining = query.size_slots;
    if (remaining == 0) {
        return;
    }
    size_t bank_id = query.start_bank_id;
    const size_t bank_count = query.size_banks;
    size_t start = query.start_slot;
    for (size_t i = 0; i < bank_count; ++i) {
        auto& bank = pool.GetBank(bank_id);
        const size_t amount = std::min(bank.Size() - start, remaining);
        const size_t next_bank_id = bank.next_bank - 1;
        func(&bank, start, amount);
        bank_id = next_bank_id;
        start = 0;
        remaining -= amount;
    }
}

} // namespace VideoCommon
