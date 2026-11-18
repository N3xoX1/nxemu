// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/page_table.h"
#include "yuzu_common/scope_exit.h"

namespace Common {

PageTable::PageTable() = default;

PageTable::~PageTable() noexcept = default;

bool PageTable::BeginTraversal(TraversalEntry* out_entry, TraversalContext* out_context,
                               Common::ProcessAddress address) const {
    out_context->next_offset = address.GetValue();
    out_context->next_page = address / page_size;

    return this->ContinueTraversal(out_entry, out_context);
}

bool PageTable::ContinueTraversal(TraversalEntry* out_entry, TraversalContext* context) const {
    // Setup invalid defaults.
    out_entry->phys_addr = 0;
    out_entry->block_size = page_size;

    // Regardless of whether the page was mapped, advance on exit.
    SCOPE_EXIT {
        context->next_page += 1;
        context->next_offset += page_size;
    };

    // Validate that we can read the actual entry.
    const auto page = context->next_page;
    if (page >= backing_addr.size()) {
        return false;
    }

    // Validate that the entry is mapped.
    const auto phys_addr = backing_addr[page];
    if (phys_addr == 0) {
        return false;
    }

    // Populate the results.
    out_entry->phys_addr = phys_addr + context->next_offset;

    return true;
}

void PageTable::Resize(std::size_t address_space_width_in_bits, std::size_t page_size_in_bits) {
    const std::size_t num_page_table_entries{1ULL
                                             << (address_space_width_in_bits - page_size_in_bits)};
    pointers.resize(num_page_table_entries);
    backing_addr.resize(num_page_table_entries);
    blocks.resize(num_page_table_entries);
    current_address_space_width_in_bits = address_space_width_in_bits;
    page_size = 1ULL << page_size_in_bits;

    // Pages are only backed once written. The padding keeps 8-byte scans inside the map.
    const std::size_t l2_entries = ((num_page_table_entries >> 9) + 64) & ~std::size_t{63};
    const std::size_t l1_entries = ((num_page_table_entries >> 18) + 64) & ~std::size_t{63};
    write_track_storage.resize(num_page_table_entries + l2_entries + l1_entries + 64);
    write_track_map = write_track_storage.data();
    write_track_l2_offset = static_cast<u32>(num_page_table_entries);
    write_track_l1_offset = static_cast<u32>(num_page_table_entries + l2_entries);
}

} // namespace Common
