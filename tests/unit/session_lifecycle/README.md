# Session lifecycle regressions

Run with `ctest -R test_session_lifecycle` (see `tests/README.md`). It compiles the production
host-thread state helper and production Host1x syncpoint manager with AddressSanitizer
and UndefinedBehaviorSanitizer. A watchdog bounds deadlocks.

The tests cover reuse of a host thread across kernel sessions (including reuse of
the kernel address), replacement by an explicitly registered service/guest thread,
reset to the session's fallback thread, already requested shutdown, blocked fences,
normal completion, 300 completion/cancellation cases, and cancellation from a
callback invoked while the syncpoint mutex is locked.

Two reports motivated these fixes:

- 2026-10-22 00:21:23, PID 43971: a hang during `ShutdownMainProcess` /
  `CloseServices`. NVDRV's `IocCtrlEventWait` fallback waited indefinitely in
  `SyncpointManager::WaitHost` while the UI joined the emulation thread. The fallback
  now receives the session shutdown token and returns Timeout on cancellation,
  without reporting an unfinished fence as completed or resuming stopped CPUs.
- 2026-10-22 10:32:52, PID 92558: EXC_BAD_ACCESS at 0x20a0 in the scheduler lock,
  through `KThread::BeginWait`, while creating services for the next game. Static
  thread-local fallback/current KThread pointers previously retained a reference
  to the previous KernelCore. They now use a unique kernel session generation,
  recreate the fallback for each session, and honor explicit thread registration.
  This is consistent with the report; the intermittent crash still requires
  validation through real game transitions.

Validation on 2026-10-22: sanitizer tests passed; full Release build and strict deep
application signature verification passed. Neither report identifies MoltenVK as
the crashing component. This does not establish a game-specific Yoshi defect.

Manual validation: in the same application process, load a game, stop it, load
Yoshi or the title involved in the second report, then repeat several times.

## Follow-up Metal validation

The same session (PID 99170) successfully changed through approximately 20 titles
before a SIGBUS in Apple's tiny allocator while Metal compute pipelines were
released after Sonic X Shadow Generations. Its cause remains unconfirmed.

A diagnostic run using `MTL_DEBUG_LAYER=1`, `MallocScribble=1`, `MallocNanoZone=0`
and periodic heap checks stopped on two distinct validation errors when loading
Yoshi's Crafted World:

1. PID 3516: a declared 65536-byte fragment constant buffer at offset 116992 in a
   131072-byte allocation. MoltenVK's canonical SPIR-V uniform declarations now
   use the collected static access size per descriptor. Dynamic accesses and
   global-memory helper accesses retain the full 65536-byte declaration.
   `test_spirv_constant_buffers` verifies the actual production SPIR-V,
   including rounding and conservative full-size cases.
2. PID 5312: a GPU buffer copy with source end 69380 beyond a 65536-byte buffer.
   `DownloadMemory` iterates all intersecting buffers, but the synchronous
   `DownloadBufferMemory` path previously passed the entire CPU read range to
   each buffer's dirty tracker. It now intersects the read with that buffer
   before consuming dirty pages, as the asynchronous path already does. This
   also preserves the adjacent buffer's dirty pages for its own download.

These validation errors are actionable defects, but neither alone proves the
cause of the earlier allocator crash. Validate startup and repeated stop/reload
with Metal validation, then repeat without diagnostic overhead.

The next validation run (PID 6127) passed those two initial checks and stopped on a
third issue: an RGBA8Uint sampled texture bound to a float texture declaration.
This texture-type issue is still unresolved. Metal validation intentionally aborts
on it; do not describe that abort as proof of the earlier allocator failure.
A subsequent run keeps heap checks but omits `MTL_DEBUG_LAYER` to validate normal
startup and stop/reload separately. NCE is now the default for builds with
NXEMU_HAS_NCE, exposing the existing native backend choice in the Debug page;
unsupported address spaces/ISA/host x18/fastmem conditions still use Dynarmic.

Manual result (2026-10-22): the user confirmed Yoshi startup, correct rendering,
stop and reopen as working with the fixes and NCE enabled. The run retained heap
checks and disabled the aborting Metal validation layer. The final application
was rebuilt and signature-verified; the video module ABI version is now 0x0127
because its host-fence wait takes a shutdown token and returns completion status.
