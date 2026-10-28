// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime_fixture.h"
#include "patch/runtime_code_patch.h"
#include "yuzu_common/native_code_metadata.h"
#include <csignal>
#include <mach/mach.h>
#include <mach/mach_vm.h>

using namespace NceTest;
namespace {
Common::NativeCodeMetadata metadata;
std::atomic<uint64_t> signal_reads{};
std::atomic<bool> invalid_read{};
void MetadataSignal(int) {
    const auto value = metadata.FindRestart(0x1234);
    if (value != 0 && value != 0x1200) invalid_read.store(true);
    signal_reads.fetch_add(1);
}
void MetadataConcurrency() {
    struct sigaction action{}, previous{};
    action.sa_handler = MetadataSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGWINCH, &action, &previous);
    std::atomic<bool> done{};
    std::atomic<uint64_t> reads{};
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back([&] {
        while (!done.load()) {
            const auto value = metadata.FindHandler(0x5678);
            if (value != 0 && value != 0x5600) invalid_read.store(true);
            ++reads;
        }
    });
    const NativeCodeEntry handlers[] = {{0x5678, 0x5600}};
    const NativeCodeEntry restarts[] = {{0x1234, 0x1200}};
    for (int i = 0; i < 10000; ++i) {
        metadata.Install(1, handlers, restarts);
        if (!(i % 16)) for (auto& worker : workers) pthread_kill(worker.native_handle(), SIGWINCH);
        metadata.Remove(1);
    }
    done.store(true);
    for (auto& worker : workers) worker.join();
    sigaction(SIGWINCH, &previous, nullptr);
    Check(!invalid_read.load() && signal_reads.load() != 0, "concurrent signal-safe metadata lookup");
    Check(metadata.FindHandler(0x5678) == 0 && metadata.FindRestart(0x1234) == 0,
          "no stale entries after unload");
    std::printf("PASS: 10000 metadata load/unload cycles, %llu reads, %llu signal reads\n",
        static_cast<unsigned long long>(reads.load()), static_cast<unsigned long long>(signal_reads.load()));
}

void RuntimeModule(bool before, unsigned version) {
    Process process;
    System system;
    Thread thread;
    thread.process = &process;
    const uint64_t text_base = Base + 0x1000;
    const uint64_t patch_base = before ? Base + 0x40000 : Base + 0x200000;
    // For the "before" case, move text rather than changing any NRO-relative offsets.
    const uint64_t actual_text = before ? Base + 0x100000 : text_base;
    std::vector<uint8_t> original(0x1000);
    // mrs x21,tpidr_el0; mrs x22,tpidrro_el0; mrs x23,cntfrq_el0;
    // mov x24,#1234+version; msr tpidr_el0,x24; mrs x25,tpidr_el0; svc #42.
    const uint32_t instructions[] = {0xD53BD055, 0xD53BD076, 0xD53BE017,
        0xD2800018u | ((1234u + version) << 5), 0xD51BD058, 0xD53BD059, 0xD4000841};
    std::memcpy(original.data() + 0x80, instructions, sizeof(instructions));
    RuntimeCodePatch patch(original.data(), original.size());
    Check(!patch.Relocate(actual_text, actual_text + (1ULL << 28)), "out-of-range branch refused");
    Check(patch.Relocate(actual_text, patch_base), "runtime relocation after refused placement");
    std::memcpy(reinterpret_cast<void*>(actual_text), original.data(), original.size());
    std::memcpy(reinterpret_cast<void*>(patch_base), patch.GetCode(), patch.GetCodeSize());
    uint32_t count;
    const auto* changes = patch.GetTextChanges(&count);
    Check(count != 0, "sparse text journal");
    for (uint32_t i = 0; i < count; ++i)
        *reinterpret_cast<uint32_t*>(actual_text + changes[i].offset) = changes[i].replacement;
    uint32_t handler_count, restart_count;
    const auto* handlers = patch.GetPostHandlers(&handler_count);
    const auto* restarts = patch.GetRestartPcs(&restart_count);
    Check(handler_count && restart_count, "runtime metadata exported");
    for (uint32_t i = 0; i < handler_count; ++i) process.handlers.emplace(handlers[i].pc, handlers[i].target);
    for (uint32_t i = 0; i < restart_count; ++i) process.restarts.emplace(restarts[i].pc, restarts[i].target);
    const auto host_text = actual_text & ~uint64_t(0x3fff);
    Check(mprotect(reinterpret_cast<void*>(host_text), 0x4000, PROT_READ | PROT_EXEC) == 0, "RX text");
    Check(mprotect(reinterpret_cast<void*>(patch_base), patch.GetCodeSize(), PROT_READ | PROT_EXEC) == 0, "RX island");
    sys_icache_invalidate(reinterpret_cast<void*>(actual_text), original.size());
    sys_icache_invalidate(reinterpret_cast<void*>(patch_base), patch.GetCodeSize());
    Core::ArmNce cpu(system, false, process, 0);
    cpu.Initialize();
    thread.context.pc = actual_text + 0x80;
    thread.context.sp = Base + 0xf00000;
    thread.context.tpidr = 0x87654321;
    cpu.SetContext(thread.context);
    cpu.SetTpidrroEl0(0x12345678);
    cpu.LockThread(&thread);
    Check(cpu.RunThread(&thread) == CpuHaltReason::SupervisorCall, "runtime NRO reaches SVC");
    cpu.UnlockThread(&thread);
    cpu.GetContext(thread.context);
    Check(cpu.GetSvcNumber() == 0x42 && thread.context.r[21] == 0x87654321 &&
          thread.context.r[22] == 0x12345678 && thread.context.r[23] == 19200000 &&
          thread.context.r[25] == 1234 + version && thread.context.tpidr == 1234 + version,
          "guest TLS, counter and SVC semantics");
    Check(mprotect(reinterpret_cast<void*>(host_text), 0x4000, PROT_READ | PROT_WRITE) == 0, "unload text");
    Check(mprotect(reinterpret_cast<void*>(patch_base), patch.GetCodeSize(), PROT_READ | PROT_WRITE) == 0, "unload island");
    for (uint32_t i = 0; i < count; ++i)
        *reinterpret_cast<uint32_t*>(actual_text + changes[i].offset) = changes[i].original;
    Check(!std::memcmp(reinterpret_cast<void*>(actual_text), original.data(), original.size()),
          "unload restores exact original text including header");
    std::printf("PASS: real NCE runtime module, island %s text, version %u, exact text restoration\n",
                before ? "before" : "after", version);
}

void InvalidateUnmappedCode() {
    System system;
    Process process;
    Core::ArmNce cpu(system, false, process, 0);
    constexpr size_t Page = 0x4000;
    auto* memory = static_cast<uint8_t*>(mmap(nullptr, Page * 3, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0));
    Check(memory != MAP_FAILED, "cache test allocation");
    std::memset(memory, 0, Page * 3);
    Check(mprotect(memory + Page, Page, PROT_NONE) == 0, "cache test inaccessible page");
    cpu.InvalidateCacheRange(reinterpret_cast<uint64_t>(memory), Page * 3);
    Check(munmap(memory + Page, Page) == 0, "cache test hole");
    cpu.InvalidateCacheRange(reinterpret_cast<uint64_t>(memory), Page * 3);
    Check(munmap(memory, Page) == 0, "cache test first page release");
    Check(munmap(memory + Page * 2, Page) == 0, "cache test last page release");
    cpu.InvalidateCacheRange(reinterpret_cast<uint64_t>(memory), Page * 3);
    cpu.InvalidateCacheRange(0, 0);
    std::puts("PASS: native cache invalidation across protected, unmapped and released code");
}

void NativeAtomics() {
    using namespace oaknut::util;
    constexpr uint64_t TableSize = 1ULL << 30;
    mach_vm_address_t table_address = 0;
    Check(mach_vm_map(mach_task_self(), &table_address, TableSize, TableSize - 1,
        VM_FLAGS_ANYWHERE, MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE,
        VM_PROT_ALL, VM_INHERIT_NONE) == KERN_SUCCESS, "aligned native atomic table");
    auto* table = reinterpret_cast<uint64_t*>(table_address);
    void* data{};
    Check(posix_memalign(&data, 0x4000, 0x4000) == 0, "canonical atomic backing");
    std::memset(data, 0, 0x4000);
    const uint64_t aliases[] = {Base + 0x400000, Base + 0x500000};
    for (const auto alias : aliases) {
        table[alias >> 12] = (reinterpret_cast<uint64_t>(data) - alias) | 3;
        Check(mprotect(reinterpret_cast<void*>(alias), 0x4000, PROT_NONE) == 0,
              "atomic guest alias is genuinely inaccessible");
    }
    const uint64_t text_base = Base + 0x300000, island = Base + 0x200000;
    mach_port_t code_entry{};
    memory_object_size_t code_size = 0x4000;
    Check(mach_make_memory_entry_64(mach_task_self(), &code_size, 0,
        MAP_MEM_NAMED_CREATE | VM_PROT_ALL, &code_entry, MACH_PORT_NULL) == KERN_SUCCESS, "code memory entry");
    mach_vm_address_t writer{};
    Check(mach_vm_map(mach_task_self(), &writer, code_size, 0, VM_FLAGS_ANYWHERE,
        code_entry, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_ALL, VM_INHERIT_NONE) == KERN_SUCCESS, "code write alias");
    mach_vm_address_t executable = text_base;
    Check(mach_vm_map(mach_task_self(), &executable, code_size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
        code_entry, 0, FALSE, VM_PROT_READ | VM_PROT_EXECUTE, VM_PROT_ALL, VM_INHERIT_NONE) == KERN_SUCCESS, "code execution alias");
    for (int mode = 0; mode < 3; ++mode) {
        const bool aliased_destination = mode == 1;
        const bool tracked_write = mode == 2;
        for (const auto alias : aliases) {
            table[alias >> 12] = (reinterpret_cast<uint64_t>(data) - alias) | (tracked_write ? 1 : 3);
            Check(mprotect(reinterpret_cast<void*>(alias), 0x4000, PROT_NONE) == 0, "reset atomic protection");
        }
        *static_cast<uint64_t*>(data) = 0;
        std::vector<uint32_t> words(0x80 / sizeof(uint32_t));
        oaknut::VectorCodeGenerator c(words);
        oaknut::Label loop;
        c.l(loop);
        c.MOV(X11, X20);
        if (aliased_destination) {
            c.LDAXR(X11, X11);
            c.ADD(X12, X11, 1);
        } else {
            c.LDAXR(X8, X11);
            c.ADD(X12, X8, 1);
        }
        c.STLXR(W13, X12, X20);
        c.CBNZ(W13, loop);
        c.SUBS(X19, X19, 1);
        c.B(oaknut::Cond::NE, loop);
        c.SVC(0x42);
        words.resize(0x1000 / sizeof(uint32_t));
        RuntimeCodePatch patch(reinterpret_cast<const uint8_t*>(words.data()), 0x1000, table_address);
        Check(patch.Relocate(text_base, island), "atomic runtime relocation");
        std::memcpy(reinterpret_cast<void*>(writer), words.data(), 0x1000);
        std::memcpy(reinterpret_cast<void*>(island), patch.GetCode(), patch.GetCodeSize());
        uint32_t count;
        const auto* changes = patch.GetTextChanges(&count);
        for (uint32_t i = 0; i < count; ++i)
            *reinterpret_cast<uint32_t*>(writer + changes[i].offset) = changes[i].replacement;
        Check(mprotect(reinterpret_cast<void*>(text_base), 0x4000, PROT_READ | PROT_EXEC) == 0, "atomic RX text");
        Check(mprotect(reinterpret_cast<void*>(island), patch.GetCodeSize(), PROT_READ | PROT_EXEC) == 0, "atomic RX island");
        sys_icache_invalidate(reinterpret_cast<void*>(text_base), 0x1000);
        sys_icache_invalidate(reinterpret_cast<void*>(island), patch.GetCodeSize());
        Process process;
        System system;
        process.memory.redirect_begin = text_base;
        process.memory.redirect_size = 0x4000;
        process.memory.redirect = reinterpret_cast<uint8_t*>(writer);
        process.memory.fault_mode = Memory::FaultMode::Emulate;
        if (tracked_write) {
            process.memory.fault_mode = Memory::FaultMode::OpenOnWrite;
            process.memory.native_atomic_table = table;
        }
        uint32_t handler_count, restart_count;
        const auto* handlers = patch.GetPostHandlers(&handler_count);
        const auto* restarts = patch.GetRestartPcs(&restart_count);
        uint32_t atomic_count;
        const auto* atomics = patch.GetAtomicHandlers(&atomic_count);
        for (uint32_t i = 0; i < atomic_count; ++i) process.atomics.emplace(atomics[i].pc, atomics[i].target);
        for (uint32_t i = 0; i < handler_count; ++i) process.handlers.emplace(handlers[i].pc, handlers[i].target);
        for (uint32_t i = 0; i < restart_count; ++i) process.restarts.emplace(restarts[i].pc, restarts[i].target);
        std::vector<std::thread> workers;
        for (size_t i = 0; i < 4; ++i) workers.emplace_back([&, i] {
            Thread thread;
            thread.process = &process;
            Core::ArmNce cpu(system, false, process, i);
            cpu.Initialize();
            thread.context.pc = text_base + 0x80;
            thread.context.sp = Base + 0xf00000 - i * 0x10000;
            thread.context.r[0] = 0x11223344;
            thread.context.r[19] = 10000;
            thread.context.r[20] = aliases[i % 2];
            cpu.SetContext(thread.context);
            cpu.LockThread(&thread);
            Check(cpu.RunThread(&thread) == CpuHaltReason::SupervisorCall, "atomic loop reaches SVC");
            cpu.UnlockThread(&thread);
            cpu.GetContext(thread.context);
            Check(thread.context.r[0] == 0x11223344 && thread.context.r[19] == 0 &&
                  thread.context.r[20] == aliases[i % 2], "atomic scratch registers preserved");
        });
        for (auto& worker : workers) worker.join();
        Check(*static_cast<uint64_t*>(data) == 40000, "exclusive updates across two inaccessible aliases are atomic");
        Check(process.atomics_enabled.load(), "both halves of LL/SC specialized together");
        Check(process.memory.faults.load() < 100, "specialized atomics do not keep faulting");
        for (uint32_t i = 0; i < count; ++i)
            *reinterpret_cast<uint32_t*>(writer + changes[i].offset) = changes[i].original;
        Check(!std::memcmp(reinterpret_cast<void*>(writer), words.data(), 0x1000),
              "lazy atomic specialization is fully restored at unload");
        if (tracked_write) Check(process.memory.faults.load() >= 2, "both aliases honor write tracking before native atomics");
        Check(mprotect(reinterpret_cast<void*>(text_base), 0x4000, PROT_READ | PROT_WRITE) == 0, "atomic unload text");
        Check(mprotect(reinterpret_cast<void*>(island), patch.GetCodeSize(), PROT_READ | PROT_WRITE) == 0, "atomic unload island");
        std::printf("PASS: native LL/SC, 4 cores, 2 inaccessible aliases, 40000 increments, destination %s base, tracking=%d\n",
                    aliased_destination ? "equals" : "differs from", tracked_write);
    }
    for (const auto alias : aliases)
        Check(mprotect(reinterpret_cast<void*>(alias), 0x4000, PROT_READ | PROT_WRITE) == 0, "release atomic alias");
    mach_vm_deallocate(mach_task_self(), writer, code_size);
    mach_port_deallocate(mach_task_self(), code_entry);
    std::free(data);
    mach_vm_deallocate(mach_task_self(), table_address, TableSize);
}

// A lock on the guest stack, just above sp, updated by native atomics. The store handler's
// spill runs between the exclusive load and store; in the lock's cache line it would cancel
// the reservation on every attempt (Absolum's main thread spun there forever).
void StackLockAtomics() {
    using namespace oaknut::util;
    constexpr uint64_t TableSize = 1ULL << 30;
    mach_vm_address_t table_address = 0;
    Check(mach_vm_map(mach_task_self(), &table_address, TableSize, TableSize - 1,
        VM_FLAGS_ANYWHERE, MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE,
        VM_PROT_ALL, VM_INHERIT_NONE) == KERN_SUCCESS, "aligned native atomic table");
    auto* table = reinterpret_cast<uint64_t*>(table_address);
    // The stack is an accessible alias of canonical memory, as guest RAM is.
    mach_port_t stack_entry{};
    memory_object_size_t stack_size = 0x4000;
    Check(mach_make_memory_entry_64(mach_task_self(), &stack_size, 0,
        MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE, &stack_entry, MACH_PORT_NULL) == KERN_SUCCESS, "stack memory entry");
    mach_vm_address_t canonical{};
    Check(mach_vm_map(mach_task_self(), &canonical, stack_size, 0, VM_FLAGS_ANYWHERE, stack_entry, 0,
        FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE) == KERN_SUCCESS, "canonical stack");
    mach_vm_address_t stack = Base + 0x600000;
    Check(mach_vm_map(mach_task_self(), &stack, stack_size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, stack_entry, 0,
        FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE) == KERN_SUCCESS, "guest stack alias");
    for (uint64_t page = stack; page < stack + stack_size; page += 0x1000)
        table[page >> 12] = (canonical + (page - stack) - page) | 3;
    // The layout Absolum hung on: lock at sp + 0x20, the old spill at sp - 0x10.
    const uint64_t sp = stack + 0xdd0, lock = sp + 0x20;
    *reinterpret_cast<uint64_t*>(lock) = 0;

    const uint64_t text_base = Base + 0x700000, island = Base + 0x780000;
    std::vector<uint32_t> words(0x80 / sizeof(uint32_t));
    oaknut::VectorCodeGenerator c(words);
    oaknut::Label loop;
    c.l(loop);
    c.LDAXR(X8, X20);
    c.ADD(X12, X8, 1);
    c.STLXR(W13, X12, X20);
    c.CBNZ(W13, loop);
    c.SUBS(X19, X19, 1);
    c.B(oaknut::Cond::NE, loop);
    c.SVC(0x42);
    words.resize(0x1000 / sizeof(uint32_t));
    RuntimeCodePatch patch(reinterpret_cast<const uint8_t*>(words.data()), 0x1000, table_address);
    Check(patch.Relocate(text_base, island), "stack lock relocation");
    std::memcpy(reinterpret_cast<void*>(text_base), words.data(), 0x1000);
    std::memcpy(reinterpret_cast<void*>(island), patch.GetCode(), patch.GetCodeSize());
    uint32_t count;
    const auto* changes = patch.GetTextChanges(&count);
    for (uint32_t i = 0; i < count; ++i)
        *reinterpret_cast<uint32_t*>(text_base + changes[i].offset) = changes[i].replacement;
    Process process;
    System system;
    uint32_t n;
    const auto* atomics = patch.GetAtomicHandlers(&n);
    for (uint32_t i = 0; i < n; ++i) process.atomics.emplace(atomics[i].pc, atomics[i].target);
    const auto* handlers = patch.GetPostHandlers(&n);
    for (uint32_t i = 0; i < n; ++i) process.handlers.emplace(handlers[i].pc, handlers[i].target);
    const auto* restarts = patch.GetRestartPcs(&n);
    for (uint32_t i = 0; i < n; ++i) process.restarts.emplace(restarts[i].pc, restarts[i].target);
    // As after the first atomic fault anywhere in the process.
    process.EnableNceAtomicHandlers();
    Check(mprotect(reinterpret_cast<void*>(text_base), 0x4000, PROT_READ | PROT_EXEC) == 0, "stack lock RX text");
    Check(mprotect(reinterpret_cast<void*>(island), patch.GetCodeSize(), PROT_READ | PROT_EXEC) == 0, "stack lock RX island");
    sys_icache_invalidate(reinterpret_cast<void*>(text_base), 0x1000);
    sys_icache_invalidate(reinterpret_cast<void*>(island), patch.GetCodeSize());

    std::atomic<bool> done{};
    std::thread watchdog([&] {
        for (int i = 0; i < 100 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!done) {
            std::fprintf(stderr, "FAIL: lock on the stack: exclusive store never succeeds (count %llu)\n",
                         static_cast<unsigned long long>(*reinterpret_cast<uint64_t*>(lock)));
            std::_Exit(1);
        }
    });
    std::thread runner([&] {
        Thread thread;
        thread.process = &process;
        Core::ArmNce cpu(system, false, process, 0);
        cpu.Initialize();
        thread.context.pc = text_base + 0x80;
        thread.context.sp = sp;
        thread.context.r[19] = 10000;
        thread.context.r[20] = lock;
        cpu.SetContext(thread.context);
        cpu.LockThread(&thread);
        Check(cpu.RunThread(&thread) == CpuHaltReason::SupervisorCall, "stack lock loop reaches SVC");
        cpu.UnlockThread(&thread);
        cpu.GetContext(thread.context);
        Check(thread.context.sp == sp, "stack lock sp preserved");
    });
    runner.join();
    done = true;
    watchdog.join();
    Check(*reinterpret_cast<uint64_t*>(lock) == 10000, "stack lock count");
    std::puts("PASS: native LL/SC on a lock just above sp, 10000 increments");
    Check(mprotect(reinterpret_cast<void*>(text_base), 0x4000, PROT_READ | PROT_WRITE) == 0, "stack lock unload text");
    Check(mprotect(reinterpret_cast<void*>(island), patch.GetCodeSize(), PROT_READ | PROT_WRITE) == 0, "stack lock unload island");
    mach_vm_deallocate(mach_task_self(), canonical, stack_size);
    mach_port_deallocate(mach_task_self(), stack_entry);
    mach_vm_deallocate(mach_task_self(), table_address, TableSize);
}
}
int main(int argc, char**) {
    MetadataConcurrency();
    if (argc > 1) return 0;
    InvalidateUnmappedCode();
    Check(mmap(reinterpret_cast<void*>(Base), Size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0) == reinterpret_cast<void*>(Base), "guest mapping");
    RuntimeModule(false, 0);
    RuntimeModule(false, 1); // same addresses, different instructions
    RuntimeModule(true, 2);
    NativeAtomics();
    StackLockAtomics();
    Check(munmap(reinterpret_cast<void*>(Base), Size) == 0, "release guest mapping");
}
