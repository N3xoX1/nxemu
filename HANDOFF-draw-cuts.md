# Handoff: GPU thread draw cuts (TOTK performance)

Branch `perf/draw-cuts`, on top of `perf/gpu-thread-write-tracking` (b66c1fba19).
Worktree: `D:\Emulation\switch\NXDev\nxemu-pr2-gpu-cache`.
Delete this file before the branch is merged or a PR is opened.

**The plan going forward is in `PLAN-ambitious.md`** (measure what can run in parallel, sync buffers at harvest points, prepare workers, read-only lookups on workers with fallback, JIT). This file holds the context, the tools and the rules.

## Goal

The aim is +30-40 % fps in Tears of the Kingdom (TOTK) on an RTX 5070 Ti. The baseline is about 104-105 fps without PGO.

The video card is not the limit. The CPU-side emulation is:

| Thread | Load at ~104 fps (9.5 ms per frame) |
|---|---|
| GPU thread (command decode + draw translation) | 82 % busy, about 8-9 ms of work per frame |
| Guest cores (JIT) | 50-66 % busy, about 6-7 ms per frame |
| Vulkan worker | 62 % idle |

Work on the GPU thread maps about 1 to 1 onto frame time. A causal test (+300 ns per draw, about 4,600 draws per frame) gave +1.46 ms per frame. Gains on the GPU thread alone cap near +25 %, at the point where the guest cores become the limit, around 125-130 fps.

## Results (2026-11-05, TOTK, non-PGO, one run each, kit variant G-tmp)

| Run | Mean fps | p5 | Rendering |
|---|---|---|---|
| Base b66c1fba19 (`pipe-01`) | 105.1 | 103.0 | OK |
| Cuts, no worker spin (`cuts-01`) | 107.0 | 102.8 | Link looked orange (see the caution below) |
| Cuts, no worker spin (`cuts-02`) | 107.7 | 104.2 | OK on 3 screenshots |
| Cuts + `NXEMU_WORKER_SPIN_US=300` (`cuts-spin300`) | 109.3 | 107.6 | OK |

The cuts give about +2.5 %; with the 300 µs worker spin, about +4 % (p5 +4.5 %). The spin keeps the Vulkan worker core busy during play. Decide whether to keep it, maybe as a setting, and try 100 µs.

**Caution about screenshots:** an orange Link with darker stairs also appears in correct builds. It is most likely cloud and sun lighting, not a bug: `cuts-01` showed it and `cuts-02` (same DLL) did not. Both "broken" screenshots so far (`cuts-01` and `direct-ubo-01`) also had the window cut off on the right. Always take several screenshots (`-Shots '120,135,145'`) before calling rendering wrong. This means the direct-UBO and stream-reuse "bugs" below may not be real. Re-test them with several screenshots before ruling them out for good. The host-sync explanation is still a real ordering risk for direct UBOs.

## Session 2026-11-05 (afternoon): track 0 done, track 1 started (uncommitted)

**Track 0 timers** (TEMP, `src/yuzu_video_core/draw_timers.h`, env `NXEMU_DRAW_TIMERS=<csv>`, one line per second; analysis script: `python timers.py <csv> 100`, copy in the kit as needed). Laps are in `PrepareDraw` (`vk_rasterizer.cpp`) and `ConfigureImpl` (`vk_graphics_pipeline.cpp`), counters in `SynchronizeBuffer`.

Run `timers-01` (cuts + spin 300, no skip), 1.59 us inside each draw:

| Step | ns/draw | Share |
|---|---|---|
| stagebuf (UBO, storage, texture buffer binds) | 337 | 21 % |
| pre (FlushWork, FlushCaching, NotifySegment, pipeline lookup) | 287 | 18 % |
| geom (index + vertex binds) | 198 | 12 % |
| handles (storage binds + texture handle reads) | 137 | 9 % |
| rt (UpdateRenderTargets + CheckFeedbackLoop) | 126 | 8 % |
| views (reuse check, samplers, FillGraphicsImageViews) | 108 | 7 % |
| updbuf (UpdateGraphicsBuffers) | 101 | 6 % |
| dynstate 77, pushimg 56, cfgdraw 41, others < 30 | | |

- 8.3 `SynchronizeBuffer` calls per draw, only 0.87 % upload anything.
- Descriptor reuse hits 74.6 % of draws.
- A (pure) is about 13 %, B (lookups) about 20 %: A + B is about 33 %, between the plan's thresholds, so track 1 goes first.
- "pre" is now split into preflush / precaching / prequery / pre (pipeline lookup) in the timers; not measured yet.

**Track 1, first step (uncommitted):**
- `MemoryTrackerBase::CpuEpoch()` (`memory_tracker_base.h`): bumped in `MarkRegionAsCpuModified`, `CachedCpuWrite`, both `FlushCachedWrites`, and `CreateRegion` (new regions start CPU modified).
- `BufferCache::SynchronizeBuffer(buffer, addr, size)` (`buffer_cache.h`): a 256-entry direct-mapped table of (addr, size, buffer_generation, epoch). It skips the sync when all four match. `buffer_generation` is bumped in `CreateBuffer` and `DeleteBuffer`.
- The sparse overload (with a copy list) is not cached.
- Skip rate: 53 % of calls (4.4 per draw), run `skip-timers-01`. Rendering OK on 3 shots (135 s had the orange-Link lighting, 145 s fine).
- TEMP toggle `NXEMU_NO_SYNC_SKIP=1` turns the skip off, for A/B with the same DLL.
- fps without timers, one run: `skip-spin300-01` 106.4 (p5 102.2). Below the earlier 109.3, so an interleaved A/B (`ab-noskip-0N` / `ab-skip-0N`) is needed.

**A/B, global epoch** (same DLL, `NXEMU_NO_SYNC_SKIP` toggle): the clean pair gives skip 108.3 (p5 104.6) vs no skip 105.7 (p5 101.7), about +2.5 %. The other two runs were disturbed: my own builds ran during `ab-noskip-01`, and `ab-skip-02` had an unexplained drop. **Never build during a run.**

**Per-region epochs** (done, replaces the global epoch):
- `region_epochs[NUM_HIGH_PAGES]`, one per 4 MiB tracker region.
- `CpuEpoch(addr, size)` sums the epochs of the regions the range covers. They only grow, so the sum changes when any of them does.
- The table grew to 1024 entries.
- Run `region-timers-01`: skip 74.6 % (6.2 skipped, 2.1 synced per draw). Misses: `miss_key` 2.09 per draw, `miss_epoch` 0.02. Epoch bumps: 1.15 per draw.
- The "pre" split: precaching (`FlushCaching`) 81 ns, pipeline lookup 125 ns, FlushWork 9 ns, NotifySegment 8 ns.
- Then the table went to 4096 entries (series `t4k-*`).

**Boot hang:**
- `rg-skip-01` stayed on a black screen from boot (about 650 fps, no title screen).
- An older run without the skip did the same (`scout2-totk-B-pr2`).
- 4 short boots with the skip (`boot-rg-01..04`) were all normal.
- Likely a rare existing hang, but keep watching for it.

**Result: dropped (reverted, kept as `patches/sync_skip_epoch.patch` in the kit).**
- With 4096 entries the skip reached 95 %, but fps did not move.
  - `t4k-noskip-01/02`: 106.3 / 107.8 (p5 101.9 / 104.2).
  - `t4k-skip-01/02`: 106.7 / 102.8 (p5 91.8 / 81.2).
- Clean syncs were already cheap: `ForEachModifiedRange` returns at once when `cpu_work_words == 0`. The earlier +2.5 % was noise.
- Two of ten boots with per-region epochs hung on a black screen (`rg-skip-01`, `t4k-timers-01`), against one in about 100 older runs. That is suspicious, but I found no correctness hole.
- Do not retry this without a new reason.

**Open question:** in the timers, the GPU thread spends only about 59 % of its time inside draws. "Outside" is about 40 %. Timers on the GPU thread loop (`gpu_thread.cpp`: `wait` for `PopWait`, `present` for `ProcessPendingComposite`) now split that. If most of it is waiting, the guest (JIT) side is already the limit.

**Old next steps for track 1 (superseded):**
- Why only 53 %? Likely epoch bumps every few draws (inline writes, cached writes). Count bumps per draw by source.
- Option: per-region epochs (one per 4 MiB tracker region) instead of one global epoch.
- Apply the same idea to `IsRegionGpuModified` / `IsRegionCpuModified` on the UBO path (a GPU epoch).

## What this branch does

1. **HLE replacement table** (`engines/maxwell_3d.h`): `replace_table` is now a fixed array of 8 entries, with the same interface (emplace, clear, size, find, begin, end). Before, it was an `unordered_map` filled and cleared on every HLE draw, which cost about 1.5-2 % of the GPU thread.
2. **Vulkan worker wake** (`renderer_vulkan/vk_scheduler.*`):
   - `DispatchWork` notifies the worker only when it is sleeping (`worker_sleeping`, guarded by `queue_mutex`).
   - The worker notifies `event_cv` only when the queue becomes empty, which is what `WaitWorker` waits for.
   - Before, `notify_all` cost 2.3 % of the GPU thread.
   - **TEMP:** `NXEMU_WORKER_SPIN_US=<µs>` makes the worker spin before it sleeps, so the GPU thread does not pay a thread wake per chunk. The default is 0 (no spin). Measure 0, 100 and 300, then pick a value or drop the spin. Remove the env var before committing.
3. **Recursive mutex** (`yuzu_common/recursive_mutex.h`): `Common::RecursiveMutex`, an owner check plus a `std::shared_mutex` (an SRW lock on MSVC). It replaces the slow MSVC `std::recursive_mutex` for `buffer_cache.mutex` and `texture_cache.mutex`, which are locked on every draw (about 3-6 % of the GPU thread including the SRW calls). The `unique_lock` types in `vk_rasterizer.cpp` and `gl_rasterizer.cpp` were updated.
4. **Framebuffer lookup** (`texture_cache/texture_cache*.h`): `GetFramebuffer` keeps the last key and id, and skips the `RenderTargets` hash lookup (about 2.3 %). `RemoveFramebuffers` clears it when that framebuffer is erased.

## Next steps

1. **Decide on the worker spin** (see Results): 0, 100 or 300 µs.
2. **Next cut: skip buffer sync when nothing changed** (about 5 %).
   - `SynchronizeBuffer` runs on every draw for every vertex buffer, storage buffer, index buffer and texture buffer, even when nothing was written.
   - Idea: keep an epoch counter per 2 MiB region, bumped wherever pages become CPU-modified (`MarkRegionAsCpuModified` / harvest of the G2 write map).
   - Remember (addr, size, epoch) per binding slot, and skip the sync when they match.
   - Keep `MarkWrittenBuffer` and the GPU-modified handling as they are.
3. **Other cuts from the profile** (`prof-tools/gpu-now.txt`, % of GPU thread samples):

   | Cost | Share |
   |---|---|
   | `RefreshVertexInput` | 2.3 % |
   | `PushImageDescriptors` | 2.3 % |
   | UBO `ReadBlockUnsafe` copies | 2 % |
   | malloc/new on the draw path | ~2 % |
   | `InnerInvalidation` locks | ~1 % |
   | `ProcessDirtyRegisters` | 1.2 % |
   | `TouchBuffer` | 2.1 % |

4. **Guest side** (needed past ~130 fps):
   - JIT time is spread out: the top 10 blocks are 10 %, the top 1000 are 51 %, and the dispatcher plus RSB about 4 %. There is no function worth replacing with native code (HLE).
   - The only general cost seen is the G2 write tracking: 5 instructions and 2 scratch registers per store (`dynarmic/backend/x64/emit_x64_memory.h`, `EmitWriteTrack`).
   - Tool: `patches/jit_perfmap.patch` (Windows perf map, env `NXEMU_PERF_MAP_DIR`) plus `prof-tools/jitmap.py <map> <raw.csv>`.
5. When all cuts are done:
   - retrain PGO (a stale profile costs about 2 %);
   - remove TEMP code;
   - squash into one commit with a short, simple English title and description, and no Claude or Co-Authored-By line;
   - the user opens PRs; ask before pushing.

## Dead ends (do not retry)

- **Skipping `SynchronizeBuffer` on clean ranges** (track 1, epoch per tracker region): 95 % skipped, no fps gain, maybe boot hangs. See the session notes above and `patches/sync_skip_epoch.patch`.

- **Direct UBO reads from imported guest memory** (`VK_EXT_external_memory_host`, `patches/direct_ubo_full.patch`):
  - Without host sync it renders wrong: Link turns orange and shadows are missing.
  - Cause: guest-visible fences (semaphores, syncpoints) are signalled when the GPU thread reaches them, through stubbed fences, before the host GPU runs the draws. The game then recycles its UBO ring.
  - With every fence waiting for the host GPU (TEMP `NXEMU_HOST_SYNC_FENCES` in the patch) rendering is correct, but there is no gain: 104.7 fps, against 105.6 for host sync alone.
  - The same cause explains why reusing stream copies (`patches/ubo_stream_reuse.patch`) broke rendering.
- **UBO binding cache keyed on epoch:** 4 % hits, because 88 % of UBO binds are streamed (`patches/ubo_epoch_cache.patch`).
- **Forcing delayed fences inline, idle-core spin, spin at TickFrame:** no gain (`patches/critical-path-tests.patch`).
- **Splitting command decode from draw translation:** decode plus macros is only ~15 % of the GPU thread, and draws are 84 %. A large refactor for at most 10-15 %.

## How to build and measure

Build (non-PGO, from the worktree, about 2-5 minutes incremental):

```bash
cmake --build build/windows-x64 --config Release --target nxemu-video -- -m:4 -v:minimal -nologo -p:CL_MPCount=12
```

Measurement kit: `D:\Emulation\switch\NXDev\nxemu\build\PR2-AB-20261104`. Use one run per test and look at p5, not only the mean. Before each run, check that no other nxemu, cl or link is running: another session runs nxemu from `NXFormat_SmartBlocks...NXEmuComparison`, and it must never be killed.

```powershell
$k='D:\Emulation\switch\NXDev\nxemu\build\PR2-AB-20261104'
Copy-Item 'D:\Emulation\switch\NXDev\nxemu-pr2-gpu-cache\modules\x64\video\Release\nxemu-video.dll' "$k\G-tmp\modules\video\" -Force
& "$k\Run.ps1" -Variant G-tmp -Game 'D:\Emulation\switch\NXDev\NxRoom\decrypted\The Legend of Zelda Tears of the Kingdom [0100F2C0115B6000][v0][US].dnsp' -Label <label> -Seconds 150 -Unlock -Keys '34:67,44:67,54:67' -Shots '140'
Set-Location $k; python analyze.py <label> | Select-Object -Last 1   # columns: frames, mean fps, p50, p5, ...
```

Notes on the kit:
- The screenshot is at `runs\<label>-G-tmp\shot-140s.png`.
- The run loads the save on its own.
- Never measure with stats or overlays on.
- `G-tmp\modules\cpu\nxemu-cpu.dll.keep` is the normal CPU module.

Profiling: start the run in the background, wait about 96 s, then run:

```powershell
prof-tools\stackprof2.exe <pid> 10 "<kit>\G-tmp;<kit>\G-tmp\modules\video;<kit>\G-tmp\modules\cpu;<kit>\G-tmp\modules\operating_system"
```

Set `STACKPROF_RAW=<file.csv>` to dump the raw top-of-stack addresses.

Uncommitted, unrelated changes in this worktree (leave them out of commits):
- LTO/PGO cmake files under `src/cmake/modules/`;
- the perf CSV logging in `src/nxemu/ui/views/main_window.cpp`.

## Session 2026-11-05 (evening): G1 step 1

- Step 1 of `DESIGN-gpu-split.md` is done, uncommitted. Details and numbers are in its "Step 1 result" section.
- In short: verify mode gives 0 mismatches on TOTK, and fps is neutral (base 123.9, split 120.1 / 132.6).
- Test slot `G-tmp` now holds the split DLL. The previous DLL is kept as `G-tmp\modules\video\nxemu-video.timers-base.dll`.
- Scratchpad scripts: `split-verify.ps1`, `split-noverify.ps1`, `ab-split.ps1`.
- Run.ps1 key presses sometimes miss the "Load save data?" screen (run ab-base-02 stayed at about 688 fps there). Check the screenshots before trusting a run.

## Session 2026-11-05 (night): G1 step 3

- Step 3 of `DESIGN-gpu-split.md` is done, uncommitted: a second "GPUDecode" thread does the decode, and the GPU thread executes. Details are in its "Step 3 result" section.
- Verify mode gives 0 mismatches on TOTK. Same DLL A/B from 75 s: one thread 100.0 / 101.4, two threads 107.8 / 107.9 (+7 %). The first numbers I wrote here (115 against 127, "+10 %") used the mean from 62 s, which still holds loading at 200-270 fps.
- `NXEMU_GPU_SPLIT_THREAD=0` turns the decode thread off. The test slot `G-tmp` holds the step 3 DLL.
- Scratchpad scripts: `thread-verify.ps1` and `ab-thread.ps1` (same DLL, toggles the env variable).
- TEMP code (draw timers, `NXEMU_WORKER_SPIN_US`) was removed on 2026-11-06. The worker spin is now a fixed 300 µs.

## Session 2026-11-05 (afternoon): G1 step 4

- **Measure from 75 s, not 62 s.** From 62 to 70 s TOTK still loads at 200-270 fps and inflates the mean by about 20 fps. Earlier "after 62 s" numbers in this file and in `DESIGN-gpu-split.md` are too high. Check the 10 s windows before trusting a run.
- Step 4 is done, uncommitted. Details are in `DESIGN-gpu-split.md`, "Step 4 result".
- **G1 is neutral on TOTK against the pre-split code** (base 108.3 / 111.7, split 108.2 / 110.6). Step 1 costs about 8 %, the second thread and the tuning win it back.
- The GPU-side threads are not saturated: the decode thread is busy about 35 %, the GPU thread about 74 %. The limit is upstream (guest CPU, or the guest waiting on GPU fences each frame).
- The CMake tree `build/windows-x64` builds the DLL into `modules/x64/video/Release` (16.7 MB, with pdb). `nxemu.sln` at the root builds a different DLL into `modules/x64/video` (12.4 MB): do not mix them in an A/B.
- The `nxemu.sln` build needs `vulkan_block_linear_unswizzle_2d_buffer_comp_spv.h`, which `host_shaders.vcxproj` does not list. It was generated by hand with `src/script/generate_shader_spc_header.cmd`.
- Kit: `G-tmp\modules\video\nxemu-video.dll` = `nxemu-video.step4.dll` (current). `nxemu-video.step3.dll` and `nxemu-video.timers-base.dll` (pre-split) are kept.
- Scratchpad scripts: `split-stats.ps1`, `ab-tune.ps1`, `ab-tune2.ps1`, `ab-base-vs-split.ps1`.

## Session 2026-11-05 (afternoon): where the TOTK frame time goes

Profile in game (TOTK, about 108 fps, split DLL, window 85-105 s), to choose between JIT work and deeper GPU changes.

**CPU time per thread** (scratchpad `thread-cpu.ps1`, `GetThreadTimes` over 20 s):

| Thread | Busy |
|---|---|
| HostTiming | 99 % (waits with MWAITX, low cost for the SMT sibling) |
| CPUCore_0 / 1 / 2 | 85 / 84 / 77 % |
| VulkanWorker | 79 % |
| GPU | 75 % |
| GPUDecode | 37 % |

No useful thread is at 100 %. CPU: Ryzen 7 9800X3D, 8 cores / 16 threads.

**Guest waits on the GPU** (TEMP counters in `nxemu-os`):
- `nvhost_ctrl::IocCtrlEventWait` (`NXEMU_NV_WAIT_STATS=<file>`): about 218 calls per second, 99 % already signalled. Only about 3 real waits per second, 2.6 ms per second in total.
- `BufferQueueCore::WaitForDequeueCondition` (`NXEMU_BQ_WAIT_STATS=<file>`): about 200 waits per second, 17 ms per second in total (about 80 us each).

**Conclusion:**
- The guest almost never waits on the GPU, and the GPU side has about 20-25 % slack. GPU-side changes cannot raise the fps much on TOTK.
- The frame is limited by the guest CPU work on the CPUCore threads: JIT code plus HLE services run there.
- The idle 15-20 % of the cores is the game's own threads waiting on each other, not on the emulator.

**Next:**
- Split the CPUCore time into JIT code, SVC and HLE service handlers, and idle. That tells JIT codegen work from HLE service cost.
- Then go to track J (JIT).

**Guest core time split** (TEMP, `NXEMU_CORE_STATS=<file>` in `physical_core.cpp`, run `core-01`, 108.3 fps, 85-120 s, per second):

| Core | JIT | Kernel (SVC, HLE, scheduler) | Idle | JIT exits | SVCs |
|---|---|---|---|---|---|
| 0 | 784 ms | 48 ms | 168 ms | 39k | 38k |
| 1 | 772 ms | 51 ms | 177 ms | 40k | 38k |
| 2 | 718 ms | 47 ms | 235 ms | 37k | 36k |
| 3 | 0 | 2 ms | 999 ms | 0 | 0 |

- The time is a state model per host thread (JIT, kernel, idle), so a fiber switch inside an SVC is not counted twice.
- **JIT 72-78 %, kernel about 5 %, idle 17-24 %.** HLE services are not the cost (about 1.3 us per SVC).
- About 112k SVCs per second, about 1,040 per frame. Top per second:
  - 0x0B SleepThread: 63k (yields);
  - 0x34 WaitForAddress and 0x35 SignalToAddress: 20k each;
  - 0x1A ArbitrateLock and 0x1B ArbitrateUnlock: 3k each;
  - 0x21 SendSyncRequest: 950.
- 63k yields per second means the game's threads often spin-yield while waiting on each other. Part of the JIT time is guest spin loops, not useful work. The critical path is probably one guest thread (main or render) that the others wait on.

**Conclusion:** the lever on TOTK is the speed of JIT code on the critical guest thread (track J):
- code quality (block linking, fastmem, flags);
- PGO for the cpu module (+8 % measured before);
- cost of a JIT exit and re-entry (about 117k per second).

Measure first which part of the JIT time is in generated code and which is in the dynarmic runtime (dispatcher, memory callbacks, compiles), for example with a host sampling profiler.
