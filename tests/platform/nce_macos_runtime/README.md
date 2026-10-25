# macOS NCE runtime integration test

From a build configured with `-DNXEMU_BUILD_TESTS=ON` on macOS arm64:

```sh
ctest --test-dir build -R "nce_runtime|nce_native_mapping"
```

The executable links the production CPU module and uses its actual `ArmNce`, signal handlers,
assembly entry/exit routines, and `Patcher`. Only the kernel interfaces are test doubles.
The executable declares SDK 12.3, like the NCE application, so XNU preserves guest x18.

The deterministic regression tests force a signal between the generated `MRS TPIDRRO_EL0`
and TSD load, both in the SVC lock and a guest TLS read. Each switches the original host
thread to another guest and resumes the interrupted guest on a second host thread. They
check lock ownership, TLS values, and that no guest instructions were replayed. A BRK at
the interrupted instruction makes the signal location deterministic; the original
instruction is restored before resumption.

The stress test then performs 100,000 native runs on four host threads with eight migrating
guest contexts, first without timer interrupts and then with them. It checks SVC numbers,
registers, TLS, and lock ownership. A watchdog fails a stalled run after 15 seconds.
