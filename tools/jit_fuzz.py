#!/usr/bin/env python3
"""Differential test of the A64 JIT in nxemu-cpu.dll (see src/nxemu-cpu/jit_fuzz.cpp).

Random A64 sequences run on two JITs that differ only by UserConfig::test_variant; a difference
in registers, NZCV, FPSR or memory is a failure. Variant bits: 1 spills values instead of
moving them to a free callee-saved register (the allocator before, as a baseline), 2 drops the handed-out check (the old bug, for
checking the tester), 4 writes the IR of each block to jit-ir.txt in the current folder, 8 turns off
the reuse of host flags (FCMP NZCV the old way, no CSEL or side exit on the flags of the
instruction before), 16 turns off zero extension tracking, get/set forwarding between register
widths, adc for FCMP NZCV and plain ordered loads (the code generation before them). Bit 32 puts
the cold code of each block right after it instead of in the far area of the code cache (the
layout before), 64 turns off side exits (each forward branch ends the block). Bit 128 patches
block links by rewriting the whole jump in place (the way before) instead of one aligned store.
Bit 256 turns off 64-bit immediates for ADD/SUB/CMP, plain stores for SetS/SetD, 32/64-bit
vector loads straight into XMM registers, lane inserts (VectorSetElement32/64) from an XMM
register and the zero extension of LeastSignificantWord, MostSignificantWord and loads (the code
generation before them).
Bit 512 makes the If and CheckBit exits at the end of a block reload NZCV and the check bit
instead of using the host flags, and counts vector register moves as changing the host flags (the
code generation before). Most forward branches are side exits, so add 64 to both variants to
test the block ends.
Bit 1024 stores NZCV at every flag-setting instruction (the code generation before) instead of
leaving it in the host flags for the side exits and the block end to store.
Bit 2048 stores every guest register write on the hot path (the code generation before) instead
of leaving a write that a later write of the same register replaces to the side exits before it.
Bit 8192 makes FRECPE call the FP code for every input (the code generation before) instead of
the inline estimate for normal inputs.
With FASTMEM = 1 each JIT also gets a write tracking map, and an arena page written without its
mark is a failure.

  jit_fuzz.py DLL run SEED COUNT VARIANT_A VARIANT_B FASTMEM [REPORT]
  jit_fuzz.py DLL bench SEED COUNT VARIANT_A VARIANT_B FASTMEM [REPORT]   also times execution
                                                   (JIT_FUZZ_REPS runs of each sequence, default 200)

JIT_FUZZ_FOCUS=FCMP_float,B_cond picks these instructions (decoder names) half of the time.
  jit_fuzz.py DLL shrink SEED ITERATION VARIANT_A VARIANT_B FASTMEM [REPORT]
  jit_fuzz.py DLL replay SEED ITERATION VARIANT_A VARIANT_B FASTMEM KEEP [REPORT]

shrink turns instructions of one failing sequence into NOPs while it still fails; KEEP is the
0/1 string it prints (1 = instruction kept).

  jit_fuzz.py DLL stress SEED ROUNDS CORES VARIANT CLEAR_PERMILLE [REPORT]

stress runs CORES threads on JITs that share one code cache, each round a random program of
nested calls and forward branches, while the main thread invalidates its functions, clears the
cache (CLEAR_PERMILLE of the time, 20 is a good value) and halts cores. Each core must end with
the registers of a JIT of its own run alone. Odd rounds count cycles. Bit 128 is not safe with
several cores (it rewrites jumps another core may be running). The exit code is the number of
failures."""
import ctypes, os, sys


def load(dll):
    dll = os.path.abspath(dll)
    os.add_dll_directory(os.path.dirname(dll))
    for up in (1, 2, 3):  # the package root, for the DLLs the module needs
        root = os.path.dirname(dll)
        for _ in range(up):
            root = os.path.dirname(root)
        if os.path.isdir(root):
            os.add_dll_directory(root)
    lib = ctypes.CDLL(dll)
    lib.nxemu_jit_fuzz.argtypes = [ctypes.c_uint64, ctypes.c_int, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_int, ctypes.c_char_p]
    lib.nxemu_jit_fuzz.restype = ctypes.c_int
    lib.nxemu_jit_fuzz_replay.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_char_p]
    lib.nxemu_jit_fuzz_bench.argtypes = [ctypes.c_int]
    lib.nxemu_jit_fuzz_focus.argtypes = [ctypes.c_char_p]
    lib.nxemu_jit_stress.argtypes = [ctypes.c_uint64, ctypes.c_int, ctypes.c_int, ctypes.c_uint32, ctypes.c_int, ctypes.c_char_p]
    lib.nxemu_jit_stress.restype = ctypes.c_int
    lib.nxemu_jit_fuzz_focus(os.environ.get('JIT_FUZZ_FOCUS', '').encode())
    return lib


def main():
    if len(sys.argv) < 8:
        print(__doc__)
        return 2
    lib, cmd = load(sys.argv[1]), sys.argv[2]
    seed, n, va, vb, fm = (int(x) for x in sys.argv[3:8])
    if cmd == 'stress':
        report = sys.argv[8] if len(sys.argv) > 8 else 'jit-stress.txt'
        failed = lib.nxemu_jit_stress(*(int(x) for x in sys.argv[3:8]), report.encode())
    elif cmd in ('run', 'bench'):
        report = sys.argv[8] if len(sys.argv) > 8 else 'jit-fuzz.txt'
        lib.nxemu_jit_fuzz_replay(-1, 0, b'')
        lib.nxemu_jit_fuzz_bench(int(os.environ.get('JIT_FUZZ_REPS', '200')) if cmd == 'bench' else 0)
        failed = lib.nxemu_jit_fuzz(seed, n, va, vb, fm, report.encode())
    elif cmd in ('shrink', 'replay'):
        keep = sys.argv[8] if cmd == 'replay' else '1' * 24
        report = sys.argv[9 if cmd == 'replay' else 8] if len(sys.argv) > (9 if cmd == 'replay' else 8) else 'jit-fuzz.txt'

        def fails(k):
            lib.nxemu_jit_fuzz_replay(n, 0, k.encode())
            return lib.nxemu_jit_fuzz(seed, n + 1, va, vb, fm, report.encode()) > 0

        if cmd == 'shrink':
            if not fails(keep):
                print('the sequence does not fail alone (it depends on earlier sequences)')
                return 0
            changed = True
            while changed:
                changed = False
                for i in range(len(keep)):
                    if keep[i] == '1' and fails(keep[:i] + '0' + keep[i + 1:]):
                        keep, changed = keep[:i] + '0' + keep[i + 1:], True
            print('keep', keep)
        failed = int(fails(keep))
    else:
        print(__doc__)
        return 2
    print(open(report).read().rstrip())
    return failed


if __name__ == '__main__':
    sys.exit(main())
