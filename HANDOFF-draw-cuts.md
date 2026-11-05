# Handoff: GPU thread draw cuts (TOTK performance)

Branch `perf/draw-cuts`, on top of `perf/gpu-thread-write-tracking` (b66c1fba19).
Worktree: `D:\Emulation\switch\NXDev\nxemu-pr2-gpu-cache`.
Delete this file before the branch is merged or a PR is opened.

## Goal

The aim is +30-40 % fps in Tears of the Kingdom (TOTK) on an RTX 5070 Ti. The baseline is about 104-105 fps without PGO.

The video card is not the limit. The CPU-side emulation is:

| Thread | Load at ~104 fps (9.5 ms per frame) |
|---|---|
| GPU thread (command decode + draw translation) | 82 % busy, about 8-9 ms of work per frame |
| Guest cores (JIT) | 50-66 % busy, about 6-7 ms per frame |
| Vulkan worker | 62 % idle |

Work on the GPU thread maps about 1 to 1 onto frame time. A causal test (+300 ns per draw, about 4,600 draws per frame) gave +1.46 ms per frame. Gains on the GPU thread alone cap near +25 %, at the point where the guest cores become the limit, around 125-130 fps.

## What this branch does (compiles, NOT yet run in game)

1. **HLE replacement table** (`engines/maxwell_3d.h`): `replace_table` is now a fixed array of 8 entries, with the same interface (emplace, clear, size, find, begin, end). Before, it was an `unordered_map` filled and cleared on every HLE draw, which cost about 1.5-2 % of the GPU thread.
2. **Vulkan worker wake** (`renderer_vulkan/vk_scheduler.*`):
   - `DispatchWork` notifies the worker only when it is sleeping (`worker_sleeping`, guarded by `queue_mutex`).
   - The worker notifies `event_cv` only when the queue becomes empty, which is what `WaitWorker` waits for.
   - Before, `notify_all` cost 2.3 % of the GPU thread.
   - **TEMP:** `NXEMU_WORKER_SPIN_US=<µs>` makes the worker spin before it sleeps, so the GPU thread does not pay a thread wake per chunk. The default is 0 (no spin). Measure 0, 100 and 300, then pick a value or drop the spin. Remove the env var before committing.
3. **Recursive mutex** (`yuzu_common/recursive_mutex.h`): `Common::RecursiveMutex`, an owner check plus a `std::shared_mutex` (an SRW lock on MSVC). It replaces the slow MSVC `std::recursive_mutex` for `buffer_cache.mutex` and `texture_cache.mutex`, which are locked on every draw (about 3-6 % of the GPU thread including the SRW calls). The `unique_lock` types in `vk_rasterizer.cpp` and `gl_rasterizer.cpp` were updated.
4. **Framebuffer lookup** (`texture_cache/texture_cache*.h`): `GetFramebuffer` keeps the last key and id, and skips the `RenderTargets` hash lookup (about 2.3 %). `RemoveFramebuffers` clears it when that framebuffer is erased.

## Next steps

1. **Measure and check rendering.**
   - Copy `modules\x64\video\Release\nxemu-video.dll` into the kit variant `G-tmp`, then run.
   - Compare against a baseline built from b66c1fba19 (about 104.5-105.6 fps, p5 about 102-104).
   - Look at the screenshot: Link must have normal colours, and the floor shadows on the stairs must be visible.
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
