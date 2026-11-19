// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/page_table.h"
#include "yuzu_common/scope_exit.h"

#ifdef _WIN32
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Common {

#ifdef _WIN32
namespace {
// The tables cover the whole guest address space (1 GiB each for 39 bits) but only the pages of
// mapped ranges are ever written. Without protection something outside the emulator code (it
// does not fault, so a kernel or driver path) reads every committed page at boot and makes all of
// them resident: ~3 GiB per game. Guard pages are skipped by it. The first real access of a page
// raises one guard exception, which clears the guard and is resumed here.
struct GuardRange {
    std::atomic<uintptr_t> begin{0};
    std::atomic<uintptr_t> end{0};
};
std::array<GuardRange, 32> guard_ranges;

LONG CALLBACK PageTableGuardHandler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* const record = info->ExceptionRecord;
    if (record->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION || record->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const uintptr_t address = record->ExceptionInformation[1];
    for (const GuardRange& range : guard_ranges) {
        if (address >= range.begin.load(std::memory_order_acquire) &&
            address < range.end.load(std::memory_order_acquire)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool PageTableGuardEnabled() {
    static const bool enabled = [] {
        const char* const flag = std::getenv("NXEMU_PAGE_TABLE_GUARD");
        return flag == nullptr || std::strcmp(flag, "0") != 0;
    }();
    return enabled;
}
} // namespace

static void GuardUntouchedPages(void* data, std::size_t size) {
    // Only large tables: small ones are fully used anyway
    if (data == nullptr || size < (std::size_t{64} << 20) || !PageTableGuardEnabled()) {
        return;
    }
    static const PVOID handler = AddVectoredExceptionHandler(1, PageTableGuardHandler);
    if (handler == nullptr) {
        return;
    }
    const uintptr_t begin = reinterpret_cast<uintptr_t>(data);
    for (const GuardRange& range : guard_ranges) {
        if (range.begin.load(std::memory_order_acquire) == begin) {
            return; // same table resized to the same size: already guarded
        }
    }
    for (GuardRange& range : guard_ranges) {
        uintptr_t expected = 0;
        if (range.end.load(std::memory_order_relaxed) == 0 &&
            range.begin.compare_exchange_strong(expected, begin, std::memory_order_acq_rel)) {
            range.end.store(begin + size, std::memory_order_release);
            DWORD old_protect;
            VirtualProtect(data, size, PAGE_READWRITE | PAGE_GUARD, &old_protect);
            return;
        }
    }
}

static void UnguardPages(const void* data) {
    const uintptr_t begin = reinterpret_cast<uintptr_t>(data);
    if (begin == 0) {
        return;
    }
    for (GuardRange& range : guard_ranges) {
        if (range.begin.load(std::memory_order_acquire) == begin) {
            range.end.store(0, std::memory_order_release);
            range.begin.store(0, std::memory_order_release);
        }
    }
}
#else
static void GuardUntouchedPages(void*, std::size_t) {}
static void UnguardPages(const void*) {}
#endif

PageTable::PageTable() = default;

PageTable::~PageTable() noexcept {
    UnguardPages(pointers.data());
    UnguardPages(backing_addr.data());
    UnguardPages(blocks.data());
    UnguardPages(write_track_storage.data());
}

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
    if (pointers.size() != num_page_table_entries) {
        // The tables are reallocated: forget the old ranges
        UnguardPages(pointers.data());
        UnguardPages(backing_addr.data());
        UnguardPages(blocks.data());
        UnguardPages(write_track_storage.data());
    }
    pointers.resize(num_page_table_entries);
    backing_addr.resize(num_page_table_entries);
    blocks.resize(num_page_table_entries);
    current_address_space_width_in_bits = address_space_width_in_bits;
    page_size = 1ULL << page_size_in_bits;
}

void PageTable::EnableWriteTracking() {
    if (write_track_map != nullptr) {
        return;
    }
    const std::size_t num_page_table_entries = pointers.size();
    // The padding keeps 8-byte scans inside the map
    const uint64_t l2_entries = ((num_page_table_entries >> 9) + 64) & ~uint64_t{63};
    const uint64_t l1_entries = ((num_page_table_entries >> 18) + 64) & ~uint64_t{63};
    write_track_storage.resize(num_page_table_entries + l2_entries + l1_entries + 64);
    write_track_map = write_track_storage.data();
    write_track_l2_offset = static_cast<u32>(num_page_table_entries);
    write_track_l1_offset = static_cast<u32>(num_page_table_entries + l2_entries);

    GuardUntouchedPages(pointers.data(), num_page_table_entries * sizeof(pointers[0]));
    GuardUntouchedPages(backing_addr.data(), num_page_table_entries * sizeof(u64));
    GuardUntouchedPages(blocks.data(), num_page_table_entries * sizeof(u64));
    GuardUntouchedPages(write_track_storage.data(), write_track_storage.size());
}

} // namespace Common
