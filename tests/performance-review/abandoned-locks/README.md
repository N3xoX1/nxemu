The fixture extracts current `SignalImpl`/`WaitForAddress` and the `9fc41c5b`
`SignalImpl` reference. It compares 100,000 normal-case traces, then checks an
already terminated owner, reacquisition by multiple signalled threads, write
failure and the direct mutex wait. Kernel interfaces and scheduling are mocked;
this verifies function decisions, not the full scheduler or game behavior.

The free-lock path retains its existing atomic operation, with no handle lookup,
allocation, logging or new state check. A contended live lock adds one state
comparison. Neither normal live-owner locking nor cycle detection is replaced.
This remains a compatibility workaround for abandoned locks, not native-kernel
semantics. Run `run.ps1` from PowerShell with MSVC and the bundled Python present.
