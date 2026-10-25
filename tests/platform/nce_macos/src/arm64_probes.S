/* Host-level ARM64 probes for the NCE macOS validation (step 3) and fault-safe
   memory accessors (step 2). Mach-O. */

.text

/* uint64_t ncev_x18_spin(uint64_t value, uint64_t iterations)
   Writes x18, spins without any system call, returns x18. */
    .globl _ncev_x18_spin
    .p2align 2
_ncev_x18_spin:
    mov     x18, x0
1:  subs    x1, x1, #1
    b.ne    1b
    mov     x0, x18
    mov     x18, xzr
    ret

/* uint64_t ncev_x18_syscall(uint64_t value): x18 across getpid(). */
    .globl _ncev_x18_syscall
    .p2align 2
_ncev_x18_syscall:
    mov     x18, x0
    mov     x16, #20            /* SYS_getpid */
    svc     #0x80
    mov     x0, x18
    mov     x18, xzr
    ret

/* uint64_t ncev_x18_signal(uint64_t value, mach_port_t self_thread)
   x18 across a SIGUSR1 delivered to this thread (handler installed by the caller). */
    .globl _ncev_x18_signal
    .p2align 2
_ncev_x18_signal:
    mov     x18, x0
    mov     x0, x1
    mov     x1, #30             /* SIGUSR1 */
    mov     x16, #328           /* SYS___pthread_kill */
    svc     #0x80
    mov     x0, x18
    mov     x18, xzr
    ret

/* uint64_t ncev_tpidr_spin(uint64_t value, uint64_t iterations)
   Writes TPIDR_EL0, spins, returns TPIDR_EL0. The original value is restored. */
    .globl _ncev_tpidr_spin
    .p2align 2
_ncev_tpidr_spin:
    mrs     x9, tpidr_el0
    msr     tpidr_el0, x0
1:  subs    x1, x1, #1
    b.ne    1b
    mrs     x0, tpidr_el0
    msr     tpidr_el0, x9
    ret

/* uint64_t ncev_read_tpidr(void) */
    .globl _ncev_read_tpidr
    .p2align 2
_ncev_read_tpidr:
    mrs     x0, tpidr_el0
    ret

/* uint64_t ncev_read_tsd_slot(uint32_t byte_offset): the sequence the Darwin patch
   code uses to find the thread parameters. */
    .globl _ncev_read_tsd_slot
    .p2align 2
_ncev_read_tsd_slot:
    mrs     x1, tpidrro_el0
    ldr     x0, [x1, x0]
    ret

/* uint64_t ncev_read_tpidrro(void) */
    .globl _ncev_read_tpidrro
    .p2align 2
_ncev_read_tpidrro:
    mrs     x0, tpidrro_el0
    ret

/* uint64_t ncev_raw_svc_N(uint64_t x16): unpatched guest "svc #imm" with the given x16.
   Returns x0 as left by the kernel. */
    .globl _ncev_raw_svc_0
    .p2align 2
_ncev_raw_svc_0:
    mov     x16, x0
    svc     #0x0
    ret

    .globl _ncev_raw_svc_26
    .p2align 2
_ncev_raw_svc_26:
    mov     x16, x0
    svc     #0x26
    ret

    .globl _ncev_raw_svc_7f
    .p2align 2
_ncev_raw_svc_7f:
    mov     x16, x0
    svc     #0x7f
    ret

/* Counters and identification registers readable at EL0. */
    .globl _ncev_read_cntfrq
    .p2align 2
_ncev_read_cntfrq:
    mrs     x0, cntfrq_el0
    ret

    .globl _ncev_read_cntvct
    .p2align 2
_ncev_read_cntvct:
    isb
    mrs     x0, cntvct_el0
    ret

    .globl _ncev_read_cntpct
    .p2align 2
_ncev_read_cntpct:
    isb
    mrs     x0, cntpct_el0
    ret

    .globl _ncev_read_dczid
    .p2align 2
_ncev_read_dczid:
    mrs     x0, dczid_el0
    ret

    .globl _ncev_read_ctr
    .p2align 2
_ncev_read_ctr:
    mrs     x0, ctr_el0
    ret

/* void ncev_excl_add(uint64_t* p, uint64_t n): n increments with LDXR/STXR (as
   compiled for the guest). */
    .globl _ncev_excl_add
    .p2align 2
_ncev_excl_add:
1:  ldxr    x2, [x0]
    add     x2, x2, #1
    stxr    w3, x2, [x0]
    cbnz    w3, 1b
    subs    x1, x1, #1
    b.ne    1b
    ret

/* Same loop after Patcher's AsOrdered rewrite: LDAXR/STLXR. */
    .globl _ncev_excl_add_ordered
    .p2align 2
_ncev_excl_add_ordered:
1:  ldaxr   x2, [x0]
    add     x2, x2, #1
    stlxr   w3, x2, [x0]
    cbnz    w3, 1b
    subs    x1, x1, #1
    b.ne    1b
    ret

/* Message passing litmus, writer: data = v; flag = v through an exclusive store.
   void ncev_mp_write(uint64_t* data, uint64_t* flag, uint64_t v, int ordered) */
    .globl _ncev_mp_write
    .p2align 2
_ncev_mp_write:
    str     x2, [x0]
    cbnz    w3, 2f
1:  ldxr    x4, [x1]
    stxr    w5, x2, [x1]
    cbnz    w5, 1b
    ret
2:  ldaxr   x4, [x1]
    stlxr   w5, x2, [x1]
    cbnz    w5, 2b
    ret

/* Reader: flag through an exclusive load, then data. Returns flag in x0, data in *out.
   uint64_t ncev_mp_read(uint64_t* data, uint64_t* flag, uint64_t* out, int ordered) */
    .globl _ncev_mp_read
    .p2align 2
_ncev_mp_read:
    cbnz    w3, 2f
    ldxr    x4, [x1]
    clrex
    ldr     x5, [x0]
    str     x5, [x2]
    mov     x0, x4
    ret
2:  ldaxr   x4, [x1]
    clrex
    ldr     x5, [x0]
    str     x5, [x2]
    mov     x0, x4
    ret

/* Fault-safe accessors. A fault on the *_insn instruction is fixed up by the probe
   signal handler: x0 = 1 and execution continues after the instruction. */

/* int ncev_probe_load(const void* addr, uint64_t* out) */
    .globl _ncev_probe_load
    .globl _ncev_probe_load_insn
    .p2align 2
_ncev_probe_load:
    mov     x2, x0
    mov     x0, #0
    mov     x3, #0
_ncev_probe_load_insn:
    ldr     x3, [x2]
    str     x3, [x1]
    ret

/* int ncev_probe_store(void* addr, uint64_t value) */
    .globl _ncev_probe_store
    .globl _ncev_probe_store_insn
    .p2align 2
_ncev_probe_store:
    mov     x2, x0
    mov     x0, #0
_ncev_probe_store_insn:
    str     x1, [x2]
    ret

/* int ncev_probe_ldxr_unaligned(void* addr): exclusive load at a misaligned address. */
    .globl _ncev_probe_ldxr
    .globl _ncev_probe_ldxr_insn
    .p2align 2
_ncev_probe_ldxr:
    mov     x2, x0
    mov     x0, #0
_ncev_probe_ldxr_insn:
    ldxr    x3, [x2]
    clrex
    ret

/* uint64_t ncev_probe_call(void* fn): calls fn; an instruction fetch fault at fn
   returns 0xFA17 (fixed up by the probe handler through the link register). */
    .globl _ncev_probe_call
    .p2align 2
_ncev_probe_call:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    blr     x0
    ldp     x29, x30, [sp], #16
    ret
