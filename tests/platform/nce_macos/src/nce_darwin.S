/* Darwin (Mach-O) version of the NCE entry trampolines of src/nxemu-cpu/nce/arm_nce.s.
   Differences from Linux, all forced by the tests of this directory:
   - Mach-O sections and underscored symbols instead of ELF directives.
   - TPIDR_EL0 is never written: the Darwin kernel stores the CPU number there and
     overwrites it on every context switch. The thread parameters live in a TSD slot
     (see g_params_slot_offset) that the runtime sets before entering.
   - No tkill: the full-context entry raises SIGUSR2 on the calling thread with the
     __pthread_kill syscall, and the host state is saved here, not taken from the
     signal frame. */

#include "nce/arm_nce_asm_definitions.h"

#define SYS___pthread_kill 328

.text

/* uint64_t ncev_enter_trampoline(void* params, Core::GuestContext* ctx, uint64_t trampoline) */
    .globl _ncev_enter_trampoline
    .p2align 2
_ncev_enter_trampoline:
    mov     x3, sp

    /* Load guest sp. x5 is a scratch register. */
    ldr     x5, [x1, #(GuestContextSp)]
    mov     sp, x5

    add     x5, x1, #(GuestContextHostContext)

    /* Host sp; the host TPIDR_EL0 slot is unused on Darwin. */
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

    /* Tail call the post-SVC trampoline of the patch section. */
    br      x2


/* uint64_t ncev_enter_signal(mach_port_t thread, void* params, Core::GuestContext* ctx) */
    .globl _ncev_enter_signal
    .p2align 2
_ncev_enter_signal:
    mov     x3, sp
    add     x5, x2, #(GuestContextHostContext)
    stp     x3, xzr, [x5, #(HostContextSpTpidrEl0)]

    /* Exits resume at host_saved_regs[11] (x30), i.e. our return address. */
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

    /* __pthread_kill(thread, SIGUSR2): the signal is delivered when the syscall
       returns, and its handler replaces this context with the guest context. */
    mov     x1, #(ReturnToRunCodeByExceptionLevelChangeSignal)
    mov     x16, #(SYS___pthread_kill)
    svc     #0x80

    /* Block execution from flowing here. */
    brk     #1000


/* Test helper: calls fn(arg) with known values in every callee-saved register and
   returns the number of callee-saved registers whose value changed (0 = preserved).
   uint64_t ncev_call_check_callee_saved(uint64_t (*fn)(void*), void* arg) */
    .globl _ncev_call_check_callee_saved
    .p2align 2
_ncev_call_check_callee_saved:
    stp     x29, x30, [sp, #-0xa0]!
    mov     x29, sp
    stp     x19, x20, [sp, #0x10]
    stp     x21, x22, [sp, #0x20]
    stp     x23, x24, [sp, #0x30]
    stp     x25, x26, [sp, #0x40]
    stp     x27, x28, [sp, #0x50]
    stp     d8, d9,   [sp, #0x60]
    stp     d10, d11, [sp, #0x70]
    stp     d12, d13, [sp, #0x80]
    stp     d14, d15, [sp, #0x90]

    mov     x9, x0
    mov     x0, x1
    movz    x19, #0x119
    movz    x20, #0x120
    movz    x21, #0x121
    movz    x22, #0x122
    movz    x23, #0x123
    movz    x24, #0x124
    movz    x25, #0x125
    movz    x26, #0x126
    movz    x27, #0x127
    movz    x28, #0x128
    fmov    d8, x19
    fmov    d9, x20
    fmov    d10, x21
    fmov    d11, x22
    fmov    d12, x23
    fmov    d13, x24
    fmov    d14, x25
    fmov    d15, x26
    mov     x10, sp
    str     x10, [sp, #-16]!
    blr     x9
    ldr     x10, [sp], #16

    mov     x0, #0
    mov     x11, sp
    cmp     x10, x11
    cinc    x0, x0, ne
    cmp     x19, #0x119
    cinc    x0, x0, ne
    cmp     x20, #0x120
    cinc    x0, x0, ne
    cmp     x21, #0x121
    cinc    x0, x0, ne
    cmp     x22, #0x122
    cinc    x0, x0, ne
    cmp     x23, #0x123
    cinc    x0, x0, ne
    cmp     x24, #0x124
    cinc    x0, x0, ne
    cmp     x25, #0x125
    cinc    x0, x0, ne
    cmp     x26, #0x126
    cinc    x0, x0, ne
    cmp     x27, #0x127
    cinc    x0, x0, ne
    cmp     x28, #0x128
    cinc    x0, x0, ne
    fmov    x12, d8
    cmp     x12, #0x119
    cinc    x0, x0, ne
    fmov    x12, d9
    cmp     x12, #0x120
    cinc    x0, x0, ne
    fmov    x12, d10
    cmp     x12, #0x121
    cinc    x0, x0, ne
    fmov    x12, d11
    cmp     x12, #0x122
    cinc    x0, x0, ne
    fmov    x12, d12
    cmp     x12, #0x123
    cinc    x0, x0, ne
    fmov    x12, d13
    cmp     x12, #0x124
    cinc    x0, x0, ne
    fmov    x12, d14
    cmp     x12, #0x125
    cinc    x0, x0, ne
    fmov    x12, d15
    cmp     x12, #0x126
    cinc    x0, x0, ne

    ldp     d14, d15, [sp, #0x90]
    ldp     d12, d13, [sp, #0x80]
    ldp     d10, d11, [sp, #0x70]
    ldp     d8, d9,   [sp, #0x60]
    ldp     x27, x28, [sp, #0x50]
    ldp     x25, x26, [sp, #0x40]
    ldp     x23, x24, [sp, #0x30]
    ldp     x21, x22, [sp, #0x20]
    ldp     x19, x20, [sp, #0x10]
    ldp     x29, x30, [sp], #0xa0
    ret
