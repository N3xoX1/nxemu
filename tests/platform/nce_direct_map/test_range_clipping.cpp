// SPDX-License-Identifier: GPL-2.0-or-later
// Broad guest operations must update every overlapping NCE window, including
// host mappings and the canonical table, without visiting the unmapped holes.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include "yuzu_common/nce_direct_map.h"
#include "yuzu_common/logging/log.h"

namespace {
unsigned errors{}, failures{};
void Check(bool condition, const char* message) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    failures += !condition;
}
vm_prot_t Protection(u64 address) {
    mach_vm_address_t start = address;
    mach_vm_size_t size{};
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object{};
    const auto kr = mach_vm_region(mach_task_self(), &start, &size, VM_REGION_BASIC_INFO_64,
                                   reinterpret_cast<vm_region_info_t>(&info), &count, &object);
    if (object) mach_port_deallocate(mach_task_self(), object);
    if (kr != KERN_SUCCESS || start > address || address - start >= size) return -1;
    return info.protection;
}
}
namespace Common::Log {
LogLevel g_classLevel[(uint8_t)LogClass::Count]{};
void FmtLogMessageImpl(LogClass, LogLevel level, const char*, unsigned int, const char*,
                       const char* format, const fmt::format_args& args) {
    if (level >= LogLevel::Error) {
        ++errors;
        std::fprintf(stderr, "%s\n", fmt::vformat(format, args).c_str());
    }
}
}
void assert_fail_impl() { std::abort(); }
[[noreturn]] void unreachable_impl() { std::abort(); }

int main(int argc, char**) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    using Map = Common::NceDirectMap;
    constexpr u64 G = Map::GuestPageSize, H = Map::HostPageSize;
    constexpr u32 R = Map::PermRead, W = Map::PermWrite;
    memory_object_size_t backing_size = 0x20000;
    mach_port_t entry{};
    if (mach_make_memory_entry_64(mach_task_self(), &backing_size, 0,
            MAP_MEM_NAMED_CREATE | VM_PROT_ALL, &entry, MACH_PORT_NULL) != KERN_SUCCESS) return 1;
    mach_vm_address_t backing{};
    if (mach_vm_map(mach_task_self(), &backing, backing_size, 0, VM_FLAGS_ANYWHERE, entry, 0,
            FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_ALL, VM_INHERIT_NONE) != KERN_SUCCESS) return 1;
    {
        Map map(entry, reinterpret_cast<u8*>(backing));
        if (!map.Reserve(0x200000)) return 1;
        const auto low = map.Windows().at(0), high = map.Windows().at(1);
        const auto* table = static_cast<const std::atomic<u64>*>(map.AtomicTable());
        const auto rw = [&](u64 address) {
            return map.AccessAllowed(address, true) && map.TrackedAccessAllowed(address, true) &&
                   map.HostAccessAllowed(address, true) && (table[address / G].load() & W) &&
                   Protection(address) == (VM_PROT_READ | VM_PROT_WRITE);
        };
        const auto none = [&](u64 address) {
            return !map.AccessAllowed(address, false) && !map.HostAccessAllowed(address, false) &&
                   table[address / G].load() == 0 && Protection(address) == VM_PROT_NONE;
        };
        // Leading clipping must preserve the original relative backing offset.
        *reinterpret_cast<u32*>(backing + H) = 0x1badcafe;
        map.Map(low.start - G, H - G, H + G, R | W);
        const bool mapped = rw(low.start);
        Check(mapped, "leading-hole Map updates host permissions and canonical entries");
        if (mapped) Check(*reinterpret_cast<u32*>(low.start) == 0x1badcafe,
                          "clipped Map preserves its relative backing offset");
        // Start from a valid direct mapping independently of leading Map.
        map.Map(low.start, H, H, R | W);
        map.Unmap(low.start - G, H + G);
        Check(none(low.start), "leading-hole Unmap removes the real host mapping");
        map.Map(low.start, 0, H, R | W);
        map.SetAccessMask(low.start - G, H + G, R);
        Check(!map.HostAccessAllowed(low.start, true) && !map.TrackedAccessAllowed(low.start, true) &&
              map.AccessAllowed(low.start, true) && Protection(low.start) == VM_PROT_READ &&
              (table[low.start / G].load() & 3) == R,
              "leading-hole GPU mask updates host and canonical permissions");
        map.SetAccessMask(low.start - G, H + G, R | W);
        Check(rw(low.start), "leading-hole GPU mask can be lifted");
        map.Unmap(low.start, H);

        const u64 a = low.end - H, b = high.start;
        map.Map(a, 0, H, R | W);
        map.Map(b, H, H, R | W);
        const auto start = std::chrono::steady_clock::now();
        map.Protect(a, b + H - a, R);
        const auto us = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count();
        Check(Protection(a) == VM_PROT_READ && Protection(b) == VM_PROT_READ &&
              !map.HostAccessAllowed(b, true) && !map.AccessAllowed(b, true) &&
              (table[b / G].load() & 3) == R,
              "Protect spanning the reserved gap updates BOTH windows");
        std::printf("INFO: gap-spanning Protect %.3f us\n", us);
        map.Protect(a, b + H - a, R | W);
        Check(rw(a) && rw(b), "cross-window permissions can be restored");
        map.Unmap(a, b + H - a);
        Check(none(a) && none(b), "cross-window Unmap clears BOTH host mappings");
        map.Map(low.end - H, 0, H, R | W);
        map.Unmap(low.end - H, H + G);
        Check(none(low.end - H), "tail clipping does not touch the following hole");
        map.Protect(0, G, R);
        map.Unmap(low.end, G);
        map.SetAccessMask(high.end, G, 0);
        map.Map(0, 0, G, R | W);
        map.Unmap(0, 0);
        Check(errors == 0, "outside/empty ranges are harmless and generate no false errors");

        map.Map(b, H, H, R | W);
        // Skip this deliberately unbounded old path when reproducing the baseline.
        if (argc == 1) {
            const auto before_errors = errors;
            map.Unmap(b, std::numeric_limits<u64>::max() - b + 1);
            Check(errors == before_errors + 1 && rw(b),
                  "overflowing range is rejected before changing guest/host/atomic state");
        }
        map.Unmap(b, H);
    }
    mach_vm_deallocate(mach_task_self(), backing, backing_size);
    mach_port_deallocate(mach_task_self(), entry);
    std::printf("%u failures\n", failures);
    return failures ? 1 : 0;
}
