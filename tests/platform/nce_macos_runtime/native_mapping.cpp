// Production NceDirectMap against real Mach mappings, with no game code.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <libkern/OSCacheControl.h>
#include "yuzu_common/nce_direct_map.h"
#include "yuzu_common/logging/log.h"

namespace Common::Log {
LogLevel g_classLevel[(uint8_t)LogClass::Count]{};
void FmtLogMessageImpl(LogClass, LogLevel level, const char*, unsigned int, const char*,
                       const char* format, const fmt::format_args&) {
    if (level >= LogLevel::Error) { std::fprintf(stderr, "%s\n", format); }
}
}
void assert_fail_impl() { std::abort(); }
[[noreturn]] void unreachable_impl() { std::abort(); }
void Check(bool condition) { if (!condition) std::abort(); }
vm_prot_t Protection(uint64_t address) {
    mach_vm_address_t start = address;
    mach_vm_size_t size{};
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object{};
    Check(mach_vm_region(mach_task_self(), &start, &size, VM_REGION_BASIC_INFO_64,
          reinterpret_cast<vm_region_info_t>(&info), &count, &object) == KERN_SUCCESS);
    if (object) mach_port_deallocate(mach_task_self(), object);
    Check(start <= address && address < start + size);
    return info.protection;
}
int main() {
    memory_object_size_t size = 0x20000;
    mach_port_t entry{};
    Check(mach_make_memory_entry_64(mach_task_self(), &size, 0,
          MAP_MEM_NAMED_CREATE | VM_PROT_ALL, &entry, MACH_PORT_NULL) == KERN_SUCCESS);
    mach_vm_address_t backing{};
    Check(mach_vm_map(mach_task_self(), &backing, size, 0, VM_FLAGS_ANYWHERE, entry, 0,
          FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_ALL, VM_INHERIT_NONE) == KERN_SUCCESS);
    {
        Common::NceDirectMap map(entry, reinterpret_cast<u8*>(backing));
        Check(reinterpret_cast<uint64_t>(map.AtomicTable()) == 0);
        Check(map.Reserve(0x200000));
        const auto start = map.Windows()[0].start;
        const auto table_address = reinterpret_cast<uint64_t>(map.AtomicTable());
        Check(table_address != 0 && (table_address & ((1ULL << 30) - 1)) == 0);
        const auto* atomic_table = reinterpret_cast<const std::atomic<uint64_t>*>(table_address);
        unsigned accessible = 0, inaccessible = 0;
        // All sixteen physical/virtual residues, not just an aligned source allocation.
        for (uint64_t physical = 0; physical < 0x4000; physical += 0x1000) {
            const uint32_t code[] = {0x52800540, 0xD65F03C0}; // mov w0,#42; ret
            std::memcpy(reinterpret_cast<void*>(backing + physical + 0x80), code, sizeof(code));
            for (uint64_t virtual_offset = 0; virtual_offset < 0x4000; virtual_offset += 0x1000) {
                const auto address = start + virtual_offset;
                map.Map(address, physical, 0x1000, 5);
                const auto translated = atomic_table[address >> 12].load();
                Check((translated & 3) == 1);
                Check((translated & ~0xfffULL) + address == backing + physical);
                map.SetAccessMask(address, 0x1000, 0);
                Check((atomic_table[address >> 12].load() & 3) == 0);
                map.SetAccessMask(address, 0x1000, 3);
                const bool executable = (Protection(address) & VM_PROT_EXECUTE) != 0;
                Check(executable == (physical == virtual_offset));
                if (executable) {
                    sys_icache_invalidate(reinterpret_cast<void*>(address + 0x80), sizeof(code));
                    Check(reinterpret_cast<int(*)()>(address + 0x80)() == 42);
                    ++accessible;
                } else {
                    ++inaccessible;
                }
                map.Unmap(address, 0x1000);
                Check(Protection(address) == VM_PROT_NONE);
                Check(atomic_table[address >> 12].load() == 0);
            }
        }
        // Equal residues are necessary, but a fragmented backing can still prevent execution.
        map.Map(start, 0, 0x1000, 5);
        map.Map(start + 0x1000, 0x5000, 0x1000, 5);
        Check(Protection(start) == VM_PROT_NONE);
        map.Unmap(start, 0x2000);
        // Reproduce an NRO whose text and BSS have incompatible physical residues.
        // Materializing the whole locked alias also keeps their shared host page coherent.
        const auto module = start + 0x1000;
        const auto bss = module + 0x5000;
        auto* file_source = reinterpret_cast<u8*>(backing + 0x1000);
        auto* bss_source = reinterpret_cast<u8*>(backing + 0x9000);
        auto* materialized = reinterpret_cast<u8*>(backing + 0x11000);
        const uint32_t code[] = {0x52800540, 0xD65F03C0};
        std::memset(reinterpret_cast<void*>(backing + 0x6000), 0xa5, 0x3000);
        for (unsigned cycle = 0; cycle < 1000; ++cycle) {
            std::memset(file_source, 0, 0x5000);
            std::memcpy(file_source + 0x80, code, sizeof(code));
            std::memset(bss_source, cycle & 0xff, 0x3000);
            map.Map(module, 0x1000, 0x3000, 5);
            map.Map(module + 0x3000, 0x4000, 0x2000, 3);
            map.Map(bss, 0x9000, 0x3000, 3);
            Check(Protection(bss + 0x100) == VM_PROT_NONE);
            map.Unmap(module, 0x8000);
            std::memcpy(materialized, file_source, 0x5000);
            std::memcpy(materialized + 0x5000, bss_source, 0x3000);
            map.Map(module, 0x11000, 0x3000, 5);
            map.Map(module + 0x3000, 0x14000, 0x5000, 3);
            sys_icache_invalidate(reinterpret_cast<void*>(module + 0x80), sizeof(code));
            Check(reinterpret_cast<int(*)()>(module + 0x80)() == 42);
            Check((Protection(bss) & VM_PROT_WRITE) != 0);
            Check(*reinterpret_cast<u8*>(bss + 0x100) == (cycle & 0xff));
            *reinterpret_cast<u8*>(bss + 0x100) ^= 0xff;
            Check(bss_source[0x100] == (cycle & 0xff)); // source remains locked/unchanged
            Check((atomic_table[bss >> 12].load() & ~0xfffULL) + bss ==
                  reinterpret_cast<uint64_t>(materialized + 0x5000));
            std::memcpy(file_source, materialized, 0x5000);
            std::memcpy(bss_source, materialized + 0x5000, 0x3000);
            Check(bss_source[0x100] == ((cycle & 0xff) ^ 0xff));
            map.Unmap(module, 0x8000);
            map.Map(module, 0x1000, 0x5000, 3);
            map.Map(bss, 0x9000, 0x3000, 3);
            Check((atomic_table[bss >> 12].load() & ~0xfffULL) + bss == backing + 0x9000);
            map.Unmap(module, 0x8000);
        }
        for (unsigned i = 0; i < 0x3000; ++i)
            Check(*reinterpret_cast<u8*>(backing + 0x6000 + i) == 0xa5);
        std::puts("PASS: 1000 NRO text/BSS materialize/write/copy-back/restore cycles, native execution and canonical pointers, neighboring pages preserved");
        std::printf("PASS: 16 residues (%u native executions, %u inaccessible mappings), "
                    "fragmented backing rejected, canonical atomic pointers/permissions and removal verified\n", accessible, inaccessible);
    }
    mach_vm_deallocate(mach_task_self(), backing, size);
    mach_port_deallocate(mach_task_self(), entry);
}
