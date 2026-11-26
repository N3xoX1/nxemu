// SPDX-FileCopyrightText: Copyright 2026 nxemu Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <stdexcept>
#include <utility>

#include "yuzu_common/multi_level_page_table.h"
#include "yuzu_common/virtual_buffer.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
void Check(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

void CheckMultiLevelPageTableLifetime() {
    Common::MultiLevelPageTable<u32> first{32, 8, 12};
    Common::MultiLevelPageTable<u32> second{32, 8, 12};
    first.ReserveRange(0, 0x10000);
    second.ReserveRange(0x1000000, 0x10000);
    first[0] = 0x12345678u;
    second[0x1000000 >> 12] = 0x87654321u;

    auto* const first_raw = first.data();
    first = std::move(second);
    Check(first[0x1000000 >> 12] == 0x87654321u, "move-assigned multi-level page table lost data");
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    Check(VirtualQuery(first_raw, &info, sizeof(info)) && info.State == MEM_FREE,
          "old multi-level page table survived move assignment");
#else
    (void)first_raw;
#endif

    Common::MultiLevelPageTable<u32> moved{std::move(first)};
    Check(moved[0x1000000 >> 12] == 0x87654321u, "move-constructed multi-level page table lost data");
}

#ifdef _WIN32
void CheckGuardLifetime() {
    constexpr size_t Size = 64 << 20;
    Common::VirtualBuffer<u8> destination(Size);
    Common::VirtualBuffer<u8> source(Size);
    auto* old = destination.data();
    Common::GuardUntouchedPages(old, Size);
    Common::GuardUntouchedPages(source.data(), Size);
    destination = std::move(source);
    MEMORY_BASIC_INFORMATION info{};
    Check(VirtualQuery(old, &info, sizeof(info)) && info.State == MEM_FREE,
          "old mapping survived move");
    destination[Size - 1] = 42;
    Check(destination[Size - 1] == 42, "moved guard page inaccessible");
}

void CheckLazyCommitLifetime() {
    for (const size_t size : {size_t{4} << 20, size_t{64} << 20}) {
        Common::VirtualBuffer<u8> buf;
        buf.resize_lazy(size);
        MEMORY_BASIC_INFORMATION info{};
        Check(VirtualQuery(buf.data(), &info, sizeof(info)) && info.State == MEM_RESERVE,
              "lazy buffer was committed upfront");
        // Reading an untouched page must fault-commit a zeroed chunk without committing the rest.
        Check(buf[0] == 0, "lazy read was not zero");
        buf[size - 1] = 73;
        Check(buf[size - 1] == 73, "lazy write failed");
        Check(VirtualQuery(buf.data(), &info, sizeof(info)) && info.State == MEM_COMMIT,
              "touched start chunk not committed");
        Check(VirtualQuery(buf.data() + (size / 2), &info, sizeof(info)) &&
                  info.State == MEM_RESERVE,
              "untouched middle chunk was committed");
        auto* raw = buf.data();
        Common::VirtualBuffer<u8> moved = std::move(buf);
        Check(moved[size - 1] == 73, "moved lazy buffer lost contents");
        moved.resize_lazy(0);
        Check(VirtualQuery(raw, &info, sizeof(info)) && info.State == MEM_FREE,
              "freed lazy buffer still mapped");
    }
}
#endif
} // namespace

int main() {
    CheckMultiLevelPageTableLifetime();
#ifdef _WIN32
    CheckGuardLifetime();
    CheckLazyCommitLifetime();
#endif
}
