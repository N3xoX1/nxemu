# macOS NCE validation suite

Checks that native code execution (NCE) can run on macOS Apple Silicon, using the real
`src/nxemu-cpu/nce/patcher.cpp` with a minimal Darwin runtime (`src/nce_runtime.*`,
`src/nce_darwin.S`).

```sh
ctest --test-dir build -R nce_macos_validate --output-on-failure
```

One step can also be run directly:
`build/tests/platform/nce_macos_validate memory|arm64|store-thunks|read-thunks|transitions`.

The executable declares SDK 12.3, as NxEmu does, so that XNU keeps x18. The build also
makes two helpers next to it: `nce_macos_validate_current_sdk`, the same program declaring
the installed SDK (control for A3.2), and `pagezero_probe`, a binary with a 16 KiB
`__PAGEZERO` (M2.3).

## Steps

| Step | Checks |
|---|---|
| 2. Memory | Host page size, guest windows below 512 GiB, Mach memory entries and aliases, faults and ESR, the exact 4 KiB to 16 KiB page plan |
| 3. ARM64 | x18 and TPIDR_EL0 across kernel entries, rewritten instructions compared with the original ones on random full states, exclusives, SVC, data in `.text` |
| R / W | Native read and write thunks: full ARM64 state, GPU page protection, interrupts at every instruction |
| 4. Transitions | Guest entry and exit, host registers, FPCR, NZCV, SVC loops, faults, Mach exception ports, interrupts |

## Results

Each check has a class:

- **blocking**: the failure has no workaround. The program exits with code 2.
- **change**: the failure shows something macOS does differently from Linux, which the NCE
  had to change. The line after the failure names the change; another check validates it.
- **info**: a measurement.

On an M4 Pro with macOS 26, the expected result is no blocking failure and these 12 change
failures: M2.2, M2.3, M2.5, M2.7, M2.11, M2.12, M2.15, M2.19, M2.24, A3.3, A3.5 and A3.11.
