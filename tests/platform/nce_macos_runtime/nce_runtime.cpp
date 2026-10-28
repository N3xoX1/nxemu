// SPDX-License-Identifier: GPL-2.0-or-later

#include "runtime_fixture.h"

using namespace NceTest;

namespace {

void InterruptedLookupMigration(System& system, Process& process, uint32_t reg) {
    uint64_t interrupted_pc{};
    for (const auto& [pc, restart] : process.restarts) {
        if (*reinterpret_cast<uint32_t*>(restart) == (0xD53BD060 | reg) && (!interrupted_pc || pc < interrupted_pc)) {
            interrupted_pc = pc;
        }
    }
    Check(interrupted_pc != 0, "restart metadata for TSD load");
    const auto original = *reinterpret_cast<uint32_t*>(interrupted_pc);
    const auto page = interrupted_pc & ~uint64_t(0x3fff);
    const auto replace = [&](uint32_t instruction) {
        Check(mprotect(reinterpret_cast<void*>(page), 0x4000, PROT_READ | PROT_WRITE) == 0, "patch fixture RW");
        *reinterpret_cast<uint32_t*>(interrupted_pc) = instruction;
        Check(mprotect(reinterpret_cast<void*>(page), 0x4000, PROT_READ | PROT_EXEC) == 0, "patch fixture RX");
        sys_icache_invalidate(reinterpret_cast<void*>(interrupted_pc), sizeof(instruction));
    };
    // Force the signal at the exact unsafe instruction, rather than depend on timer luck.
    // The production SIGTRAP exit saves the same context as an asynchronous interruption.
    replace(0xD4200000); // brk #0
    Thread migrating, other;
    migrating.process = other.process = &process;
    migrating.context.pc = other.context.pc = Base + 0x40;
    migrating.context.sp = Base + 0x300000;
    other.context.sp = Base + 0x310000;
    migrating.context.tpidr = 0x12345678;
    other.context.tpidr = 0x87654321;
    Core::ArmNce first(system, false, process, 0), second(system, false, process, 1);
    std::atomic<unsigned> stage{};
    std::thread source([&] {
        first.Initialize(); first.SetContext(migrating.context);
        first.SetTpidrroEl0(migrating.context.tpidr + 8);
        first.LockThread(&migrating);
        Check(first.RunThread(&migrating) == CpuHaltReason::PrefetchAbort, "forced interruption");
        first.UnlockThread(&migrating); first.GetContext(migrating.context);
        replace(original);
        // Keep the old host thread alive, now running another guest thread. A stale TSD base
        // would read this other thread's parameters when the first guest migrates.
        first.SetContext(other.context); first.LockThread(&other);
        first.SetTpidrroEl0(other.context.tpidr + 8);
        Check(first.RunThread(&other) == CpuHaltReason::SupervisorCall, "source switches guest");
        first.UnlockThread(&other);
        stage.store(1, std::memory_order_release);
        while (stage.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    });
    std::thread destination([&] {
        while (stage.load(std::memory_order_acquire) != 1) std::this_thread::yield();
        second.Initialize(); second.SetContext(migrating.context);
        second.SetTpidrroEl0(migrating.context.tpidr + 8);
        second.LockThread(&migrating);
        Check(second.RunThread(&migrating) == CpuHaltReason::SupervisorCall, "resume on another host thread");
        second.UnlockThread(&migrating);
        second.GetContext(migrating.context);
        Check(other.params.lock == 1, "migration must not lock the previous host's current guest");
        Check(migrating.params.lock == 1, "migrating guest lock released");
        Check(migrating.context.r[21] == migrating.context.tpidr && migrating.context.r[22] == migrating.context.tpidr + 8, "TLS lookup restarted on destination host");
        Check(migrating.context.r[19] == 1, "interruption did not replay guest instructions");
        stage.store(2, std::memory_order_release);
    });
    source.join(); destination.join();
    std::printf("PASS: forced interruption in %s TSD lookup, followed by host-thread migration\n", reg == 0 ? "SVC lock" : "guest TLS");
}

// Horizon does not check sp alignment, macOS does: stack accesses through an unaligned sp are emulated.
void UnalignedStackPointer(System& system, Process& process) {
    Thread t;
    t.process = &process;
    t.context.pc = Base + 0x80;
    t.context.sp = Base + 0x320000;
    t.context.r[19] = 0x1111111111111111;
    t.context.r[20] = 0x2222222222222222;
    t.context.v[0][0] = 0x3333333333333333;
    t.context.v[0][1] = 0x4444444444444444;
    t.context.tpidr = 0x12345678;
    Core::ArmNce cpu(system, false, process, 0);
    std::thread runner([&] {
        cpu.Initialize(); cpu.SetContext(t.context);
        cpu.SetTpidrroEl0(t.context.tpidr + 8);
        cpu.LockThread(&t);
        Check(cpu.RunThread(&t) == CpuHaltReason::SupervisorCall, "unaligned sp accesses reach the SVC");
        cpu.UnlockThread(&t);
        Check(cpu.GetSvcNumber() == 0x43, "unaligned sp SVC number");
        cpu.GetContext(t.context);
    });
    runner.join();
    Check(t.context.sp == Base + 0x320000, "sp restored");
    Check(t.context.r[21] == 0x1111111111111111 && t.context.r[22] == 0x2222222222222222, "stp and ldp through unaligned sp");
    Check(t.context.v[1][0] == 0x3333333333333333 && t.context.v[1][1] == 0x4444444444444444, "q register through unaligned sp");
    std::puts("PASS: integer and vector stack accesses through an unaligned sp");
}
}

int main(int argc, char**) {
    const bool interrupts = argc > 1;
    Check(mmap(reinterpret_cast<void*>(Base), Size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANON, -1, 0) == reinterpret_cast<void*>(Base), "guest mapping");
    System system;
    Process process;
    // add x19, x19, #1; mrs x21, TPIDR_EL0; mrs x22, TPIDRRO_EL0; svc #0x42; b loop.
    const uint32_t code[] = {0x91000673, 0xD53BD055, 0xD53BD076, 0xD4000841, 0x17FFFFFC};
    std::vector<uint8_t> image(1024 * 1024);
    std::memcpy(image.data() + 0x40, code, sizeof(code));
    // mov x9, #4; sub sp, sp, x9; stp x19, x20, [sp]; ldp x21, x22, [sp]; str q0, [sp, #16];
    // ldr q1, [sp, #16]; add sp, sp, x9; svc #0x43; b .
    const uint32_t unaligned_sp[] = {0xD2800089, 0xCB2963FF, 0xA90053F3, 0xA9405BF5, 0x3D8007E0,
                                     0x3DC007E1, 0x8B2963FF, 0xD4000861, 0x14000000};
    std::memcpy(image.data() + 0x80, unaligned_sp, sizeof(unaligned_sp));
    Core::NCE::Patcher patcher;
    Check(patcher.PatchText(image.data(), 0x4000, 0, 0x4000), "PatchText");
    uint64_t image_size = 0x4000;
    Check(patcher.RelocateAndCopy(Base, 0, 0x4000, image.data(), &image_size, &process.handlers, &process.restarts), "RelocateAndCopy");
    std::memcpy(reinterpret_cast<void*>(Base), image.data(), image_size);
    Check(mprotect(reinterpret_cast<void*>(Base), (image_size + 0x3fff) & ~uint64_t(0x3fff), PROT_READ | PROT_EXEC) == 0, "RX mapping");
    sys_icache_invalidate(reinterpret_cast<void*>(Base), image_size);
    InterruptedLookupMigration(system, process, 0);
    InterruptedLookupMigration(system, process, 21);
    UnalignedStackPointer(system, process);
    Thread guests[8];
    for (size_t i = 0; i < 8; ++i) {
        auto& t = guests[i]; t.process = &process;
        t.context.pc = Base + 0x40;
        t.context.sp = Base + 0x100000 + 0x10000 * (i + 1);
        t.stack_top = t.context.sp;
        t.context.r[20] = 0x12345678 + i;
        t.context.tpidr = Base + 0x800000 + i * 0x1000;
    }
    std::vector<std::unique_ptr<CoreState>> cores;
    for (size_t i = 0; i < 4; ++i) cores.push_back(std::make_unique<CoreState>(system, process, i));
    std::atomic<bool> done{};
    std::atomic<uint64_t> svc_count{}, irq_count{}, progress{};
    std::thread watchdog([&] {
        for (unsigned i = 0; i < 150 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!done) {
            std::fprintf(stderr, "FAIL: runtime stalled after %llu runs, %llu SVCs, %llu interrupts\n", progress.load(), svc_count.load(), irq_count.load());
            for (auto& c : cores) {
                auto t = c->current;
                std::fprintf(stderr, "core=%zu pc=%llx esr=%llx thread=%p lock=%u running=%u ctx=%p\n", c->cpu.m_core_index,
                    c->cpu.m_guest_ctx.pc, c->cpu.m_guest_ctx.esr_el1.load(), t,
                    t ? t->params.lock : 9, t ? t->params.is_running : 9, t ? t->params.native_context : nullptr);
            }
            std::_Exit(1);
        }
    });
    std::vector<std::thread> workers;
    for (size_t i = 0; i < cores.size(); ++i) workers.emplace_back([&, i] {
        auto& core = *cores[i]; core.cpu.Initialize();
        for (size_t n = 0; n < 25000; ++n) {
            auto& t = guests[(n + i) % 8];
            std::scoped_lock thread_lock(t.mutex);
            core.cpu.SetContext(t.context);
            core.cpu.SetTpidrroEl0(t.context.tpidr + 0x800);
            {
                std::scoped_lock lock(core.guard);
                core.current = &t; core.cpu.LockThread(&t);
            }
            const auto hr = core.cpu.RunThread(&t);
            core.cpu.UnlockThread(&t);
            { std::scoped_lock lock(core.guard); core.current = nullptr; }
            const bool svc = hr == CpuHaltReason::SupervisorCall || hr == CpuHaltReason::SupervisorCallBreakLoop;
            if (svc) {
                Check(core.cpu.GetSvcNumber() == 0x42, "SVC number"); ++svc_count; ++t.svc_count;
            } else Check(hr == CpuHaltReason::BreakLoop, "halt reason");
            core.cpu.GetContext(t.context);
            Check(t.context.r[20] == 0x12345678 + (&t - guests), "register preserved across migration");
            Check(t.context.tpidr == Base + 0x800000 + (&t - guests) * 0x1000, "TLS preserved across migration");
            Check(t.context.r[19] >= t.svc_count && t.context.r[19] <= t.svc_count + 1, "no duplicated or lost guest increments");
            if (svc) {
                Check(t.context.r[19] == t.svc_count && t.context.sp == t.stack_top, "complete SVC context");
                Check(t.context.r[21] == t.context.tpidr && t.context.r[22] == t.context.tpidr + 0x800, "guest TLS reads after migration");
            }
            Check(t.params.lock == 1 && !t.params.is_running && !t.params.native_context, "host owns no guest lock");
            ++progress;
        }
    });
    std::thread timer([&] {
        while (!done) {
            if (interrupts) for (auto& core : cores) {
                std::scoped_lock lock(core->guard);
                if (core->current) { core->cpu.SignalInterrupt(core->current); ++irq_count; }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
    });
    for (auto& worker : workers) worker.join();
    done = true; timer.join(); watchdog.join();
    std::printf("PASS: %llu runs, %llu SVCs, %llu interrupts; production runtime + patcher, 4 cores, 8 migrating threads\n", progress.load(), svc_count.load(), irq_count.load());
}
