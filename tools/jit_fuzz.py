#!/usr/bin/env python3
"""Differential test of the A64 JIT in nxemu-cpu.dll (see src/nxemu-cpu/jit_fuzz.cpp).

Random A64 sequences run on two JITs that differ only by UserConfig::test_variant; a difference
in registers, NZCV, FPSR or memory is a failure. Variant bits: 1 spills XMM values instead of
moving them to a free callee-saved register, 2 drops the handed-out check (the old bug, for
checking the tester), 4 writes the IR of each block to jit-ir.txt in the current folder.

  jit_fuzz.py DLL run SEED COUNT VARIANT_A VARIANT_B FASTMEM [REPORT]
  jit_fuzz.py DLL bench SEED COUNT VARIANT_A VARIANT_B FASTMEM [REPORT]   also times execution
                                                   (JIT_FUZZ_REPS runs of each sequence, default 200)
  jit_fuzz.py DLL shrink SEED ITERATION VARIANT_A VARIANT_B FASTMEM [REPORT]
  jit_fuzz.py DLL replay SEED ITERATION VARIANT_A VARIANT_B FASTMEM KEEP [REPORT]

shrink turns instructions of one failing sequence into NOPs while it still fails; KEEP is the
0/1 string it prints (1 = instruction kept). The exit code is the number of failures."""
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
    return lib


def main():
    if len(sys.argv) < 8:
        print(__doc__)
        return 2
    lib, cmd = load(sys.argv[1]), sys.argv[2]
    seed, n, va, vb, fm = (int(x) for x in sys.argv[3:8])
    if cmd in ('run', 'bench'):
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
