/* SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Darwin (Mach-O) version of arm_nce.s. TPIDR_EL0 is never written: the Darwin kernel stores
   the CPU number there. The thread parameters are found through a TSD slot instead, and the
   signal handlers are written in C++ (arm_nce_darwin.cpp). */

#include "nce/arm_nce_asm_definitions.h"

#define SYS___pthread_kill 328

.text

/* static Dynarmic::HaltReason Core::ArmNce::ReturnToRunCodeByTrampoline(void* tpidr, Core::GuestContext* ctx, u64 trampoline_addr) */
    .globl __ZN4Core6ArmNce27ReturnToRunCodeByTrampolineEPvPNS_12GuestContextEy
    .p2align 2
__ZN4Core6ArmNce27ReturnToRunCodeByTrampolineEPvPNS_12GuestContextEy:
    /* Back up host sp to x3. */
    mov     x3, sp

    /* Load guest sp. x5 is used as a scratch register. */
    ldr     x5, [x1, #(GuestContextSp)]
    mov     sp, x5

    /* Offset GuestContext pointer to the host member. */
    add     x5, x1, #(GuestContextHostContext)

    /* Save original host sp. The host tpidr_el0 slot is unused on Darwin. */
    stp     x3, xzr, [x5, #(HostContextSpTpidrEl0)]

    /* Save all callee-saved host GPRs. */
    stp     x19, x20, [x5, #(HostContextRegs+0x0)]
    stp     x21, x22, [x5, #(HostContextRegs+0x10)]
    stp     x23, x24, [x5, #(HostContextRegs+0x20)]
    stp     x25, x26, [x5, #(HostContextRegs+0x30)]
    stp     x27, x28, [x5, #(HostContextRegs+0x40)]
    stp     x29, x30, [x5, #(HostContextRegs+0x50)]

    /* Save all callee-saved host FPRs. */
    stp     q8, q9,   [x5, #(HostContextVregs+0x0)]
    stp     q10, q11, [x5, #(HostContextVregs+0x20)]
    stp     q12, q13, [x5, #(HostContextVregs+0x40)]
    stp     q14, q15, [x5, #(HostContextVregs+0x60)]

    /* Save host fpcr and fpsr. The guest state loaded by the trampoline replaces them. */
    mrs     x6, fpcr
    mrs     x7, fpsr
    stp     w6, w7, [x5, #(HostContextFpcrFpsr)]

    /* Tail call the trampoline to restore guest state. */
    br      x2


/* static Dynarmic::HaltReason Core::ArmNce::ReturnToRunCodeByExceptionLevelChange(mach_port_t thread, Core::GuestContext* ctx) */
    .globl __ZN4Core6ArmNce37ReturnToRunCodeByExceptionLevelChangeEjPNS_12GuestContextE
    .p2align 2
__ZN4Core6ArmNce37ReturnToRunCodeByExceptionLevelChangeEjPNS_12GuestContextE:
    /* Save the host state here: the signal handler replaces the whole context with the
       guest one, and the exits resume at host_saved_regs[11] (x30), our return address. */
    mov     x3, sp
    add     x5, x1, #(GuestContextHostContext)
    stp     x3, xzr, [x5, #(HostContextSpTpidrEl0)]

    stp     x19, x20, [x5, #(HostContextRegs+0x0)]
    stp     x21, x22, [x5, #(HostContextRegs+0x10)]
    stp     x23, x24, [x5, #(HostContextRegs+0x20)]
    stp     x25, x26, [x5, #(HostContextRegs+0x30)]
    stp     x27, x28, [x5, #(HostContextRegs+0x40)]
    stp     x29, x30, [x5, #(HostContextRegs+0x50)]

    stp     q8, q9,   [x5, #(HostContextVregs+0x0)]
    stp     q10, q11, [x5, #(HostContextVregs+0x20)]
    stp     q12, q13, [x5, #(HostContextVregs+0x40)]
    stp     q14, q15, [x5, #(HostContextVregs+0x60)]

    mrs     x6, fpcr
    mrs     x7, fpsr
    stp     w6, w7, [x5, #(HostContextFpcrFpsr)]

    /* __pthread_kill(thread, SIGUSR2): the signal is delivered when the call returns. On
       entry, x0 = thread port, which is already in the right place. */
    mov     x1, #(ReturnToRunCodeByExceptionLevelChangeSignal)
    mov     x16, #(SYS___pthread_kill)
    svc     #0x80

    /* Block execution from flowing here. */
    brk     #1000


/* static void Core::ArmNce::LockThreadParameters(void* tpidr) */
    .globl __ZN4Core6ArmNce20LockThreadParametersEPv
    .p2align 2
__ZN4Core6ArmNce20LockThreadParametersEPv:
    /* Offset to lock member. */
    add     x0, x0, #(TpidrEl0Lock)

1:
    /* Clear the monitor. */
    clrex

2:
    /* Load-linked with acquire ordering. */
    ldaxr   w1, [x0]

    /* If the value was SpinLockLocked, clear monitor and retry. */
    cbz     w1, 1b

    /* Store-conditional SpinLockLocked with relaxed ordering. */
    stxr    w1, wzr, [x0]

    /* If we failed to store, retry. */
    cbnz    w1, 2b

    ret


/* static void Core::ArmNce::UnlockThreadParameters(void* tpidr) */
    .globl __ZN4Core6ArmNce22UnlockThreadParametersEPv
    .p2align 2
__ZN4Core6ArmNce22UnlockThreadParametersEPv:
    /* Offset to lock member. */
    add     x0, x0, #(TpidrEl0Lock)

    /* Load SpinLockUnlocked. */
    mov     w1, #(SpinLockUnlocked)

    /* Store value with release ordering. */
    stlr    w1, [x0]

    ret
