# PR 396 regression fixtures

Run from the repository root on Windows:

```powershell
& .\tests\pr396\run-tests.ps1
& .\tests\pr396\run-vulkan.ps1
```

The scripts currently use the installed VS 2026 Community v145 toolchain, Vulkan
SDK 1.4.357.0 and the bundled Codex Python runtime. Adjust their paths for another
machine. Initialize the repository's Boost submodule first. Generated sources,
executables, extraction hashes and results go into `build/pr396-corrections/tests`.

The CPU runner compares pinned master `c32270c39249ada403c518c4bb28e8f807f00e1a`,
original PR head `fa29fce321314eb99b49f083fd5601818450a6e3` and current working
sources. It extracts production function bodies without changing their logic;
only enclosing class names are replaced. Memory, channel setup and GPU runtime
are deterministic doubles. Range sets, descriptor payloads, command bitfields
and the corrected UsageTracker use production headers. Expected baseline failures
are printed; any failure of the corrected executable fails the runner.

Coverage includes DMA parsing after completed and partial macro/compute commands,
ordinary and sparse uploads preserving newer GPU data, asynchronous download
cancellation, descriptor capacity, scheduler hazards, sparse alias invalidation,
copy-back usage tracking, duplicate/overlapping/adjacent writable ranges and
unaligned writes smaller than 64 bytes. Byte/block oracles exercise 2,000 upload
cases, 1,500 sparse write cases and 5,000 UsageTracker cases with fixed seeds.

The Vulkan fixture runs actual compute/copy commands and checks producer/consumer
ordering, including negative controls reproducing both original regressions. It
uses the extracted scheduler predicates and CanReorderUpload body. Query hook
presence is checked in SyncValues; corrected copy counts and destination usage
are measured by the CPU fixture and passed to the Vulkan fixture. These fixtures
do not instantiate the complete emulator graphics pipeline or execute a game.

CPU timings report seven-series medians in optimized builds. GPU timestamps
report 21 samples after four warmups for 2 MiB copies with runtime-equivalent
barriers, using host-visible/coherent buffers. Results measure these operations,
not full rendering performance or game FPS. Run benchmarks without another
heavy workload for comparable timings.
