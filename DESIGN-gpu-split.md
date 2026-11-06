# Design: split the GPU thread into a decode stage and a draw stage (G1)

Status: steps 1, 3 and 4 done and committed (2026-11-06). **Net result on TOTK: neutral against the code before the split**, see "Step 4 result" at the end. Read `PLAN-ambitious.md` and `HANDOFF-draw-cuts.md` first. Delete before merging.

**Cleanup (2026-11-06):** the TEMP counters, timers and env switches named in this file were removed before the commit. They stay here as a record of how each result was measured. The kept defaults are the ones that were measured "on". `NXEMU_GPU_SPLIT_THREAD=0` is gone too: the decode thread always runs in async mode. `NXEMU_GPU_SPLIT_VERIFY` stays.

## Why

Measured on TOTK (2026-11-05, RTX 5070 Ti, Ryzen 7 9800X3D, about 105 fps). One GPU thread does everything:

| Share of the GPU thread | What |
|---|---|
| ~60 % | Draws (`RasterizerVulkan::PrepareDraw`) |
| ~15-18 % | Pure decode: `DmaPusher` (`ProcessCommands` alone 6.1 %), `CallMethod`, `PushMethodRun`, macro execution, shadow RAM, dirty registers |
| ~9-10 % | Non-draw rasterizer work: invalidations, `TickFrame`, constant buffer binds, compute dispatches, flushes, clears, queries |
| ~11 % | Waiting for commands (`PopWait`) |

Sources:
- the timers in `draw_timers.h`, run `split-timers-01`: macros 56 ms/s, DmaPusher + methods 231 ms/s, draws 590 ms/s, wait 104 ms/s;
- the sampling profile `prof-tools/gpu-split.txt` in the kit. Stacks that go through macro JIT code do not reach `RunThread`, so the inclusive shares of decode are low there.

The GPU thread is the limit: it waits only 11 %. The guest cores wait 25-46 % of the time.

**Expected gain:** the draw thread keeps about 70 % of today's load, so about +20 % fps minus the cost of the stream (estimate 2-4 %). That gives about +15-20 %.

More important, it is the base for tracks 2 and 3 (`PLAN-ambitious.md`):
- the decode stage already produces the per-draw state;
- a draw packet then is "the methods since the previous draw";
- prepare workers can work on packets later.

## Cut point

The code already separates the two kinds of work:
- **Plain register writes:** non-executable methods go to `method_sink` and only reach `ProcessShadowRam` + `ProcessDirtyRegisters`.
- **Methods with side effects** (`Maxwell3D::IsMethodExecutable`, `ProcessMethodCall`):
  - draw begin and end, inline index data;
  - `const_buffer.buffer` (inline constant buffer data, written into guest memory at once with `WriteBlockCached`);
  - `bind_groups` (constant buffer binds);
  - queries, `sync_info`, `render_enable`, `launch_dma` and `inline_data` (I2M);
  - barriers, `wait_for_idle`;
  - macro upload and bind.
- **Macros** (`>= MacroRegistersStart`):
  - generic macros only read registers and parameters, and emit method calls;
  - the 12 HLE macros (`macro_hle.cpp`) read parameters from GPU memory (`RefreshParameters`, sometimes written by the GPU), set and restore a few registers, and draw.

### Stage D: decode thread (new)

- `Scheduler::Push` → `DmaPusher` → `Puller`.
- The subchannel engines in "front" mode:
  - shadow RAM;
  - a private register file;
  - execution of generic macros (MacroJIT), which write into the private register file.
- **Output:** a stream of already-resolved method calls `(engine, method, argument)`, after shadow RAM and with macros expanded.
- HLE macros are not run here. Their call (macro id, parameters, parameter segments) goes into the stream as one event.

### Stage R: draw thread (the current GPU thread)

- Reads the stream in order.
- Register writes: `ProcessDirtyRegisters` on its own register file, the one `PrepareDraw` reads today.
- Executable methods and HLE macro events: run exactly as today (`ProcessMethodCall`, `HLEMacroImpl::Execute`).
- Everything the rasterizer reads (regs, dirty flags, `state.shader_stages`, `draw_manager` state) stays on stage R, unchanged. **The rasterizer does not change in step 1.**

## Hazards and how each one is handled

| Hazard | Handling |
|---|---|
| Inline constant buffer data written to guest memory while stage R is behind: a draw would see the next draw's constants | Stage D does not write memory. The data goes into the stream, and stage R calls `ProcessCBMultiData` in order. |
| I2M (`inline_data`), DMA engine copies, `ClearMemory` HLE | Same: payload in the stream, executed by stage R. |
| Macros that read a register written by an HLE macro | The HLE macros restore what they set. For the rest (`BindShader`, `SetRasterBoundingBox`, `TransformFeedbackSetup`, `MultiLayerClear`), give each a small "register effects" function that stage D runs, or mark the registers "R only" and check them in verify mode. |
| `Puller` semaphore acquire on a value that the GPU releases | Stage D waits until stage R has passed the matching release (a sequence number per stream position). |
| `wait_for_idle`, `invalidate_texture_data_cache`, syncpoint waits, fences seen by the guest | They go into the stream and stage R does them. The guest-visible effects (`signaled_fence`, syncpoints) are signalled by stage R, as today. |
| Push buffers that the GPU writes (rare) | Detect GPU-modified pages in the push buffer range, and drain stage R before fetching. |
| Channels (`BindChannel`), several engines | The stream carries channel and engine switches as events. Each engine keeps a front copy (D) and a back copy (R). |
| `RasterizerVulkan::FlushRegion` / `OnCacheInvalidation` commands from the guest (`gpu_thread.cpp`) | They stay on stage R, queued after the stream position at the time they arrived. |
| Guest reads of GPU registers (`GetRegisterValue`, the shadow state) | Answered from stage R; wait for R if needed. They are rare and already sync points. |

## Stream format

- A single-producer single-consumer ring of 32-bit words in blocks of, say, 64 KiB. Use a fixed pool, no allocation per draw.
- Word layout: header `(engine:4, kind:4, count:24)`, then the method/argument pairs or the payload.
- Runs of consecutive register writes are packed as `(method, count, values...)`. That is the common case: `PushMethodRun` already sees them.
- Stage D publishes a block when it is full, at each draw end, and at every sync point. This bounds the latency.
- **Size estimate:** TOTK writes about 50-150 registers per draw, about 1 KiB per draw, about 5 MB per frame. That is small for memory bandwidth.

## Verify mode (required before any performance work)

Setting `gpu_split_verify`: stage R also gets a full copy of the registers from stage D at each draw. It compares that copy with its own and logs the first mismatching register with the macro or method that wrote it.
- It catches missing effects in front mode (HLE macro register effects, shadow RAM replay).
- Run it on the benchmark set (TOTK, SMO, Civilization VII, one texture-heavy game) before any fps measure.

## Steps

1. **Refactor in place, one thread.** Split `Maxwell3D` method handling into `FrontCallMethod` (shadow RAM, private registers, macro expansion, emitting stream words) and `BackExecute` (dirty registers, side effects). Stage D writes the stream and stage R reads it right away on the same thread.
   - Expect a small loss.
   - Proves correctness with the verify mode.
2. **Same for the other engines:** Fermi2D, KeplerCompute, KeplerMemory, MaxwellDMA and `Puller`. Most of their methods are side effects and pass through as is.
3. **Second thread** for stage D, with the ring, sync points and drain. Measure on the benchmark set.
4. **Tune:** block size, spin before sleep on both sides (as for the Vulkan worker), thread placement (track 6).
5. **Then tracks 2 and 3:**
   - a draw packet is the stream slice between two draw ends;
   - prepare workers can read the register file state at a packet with a copy-on-write register snapshot from stage D.

## Not in scope for G1

- Changing the rasterizer, the buffer cache or the texture cache.
- Changing the guest-visible fence model (that is track 5).

## Before measuring

The TEMP timers in `draw_timers.h` cost about 2 % even when off: `DrawTimers::Enabled` is not inlined. Remove them, or compile them out, before any absolute fps comparison.

## Step 1 result (2026-11-05)

Done in place, one thread, not committed. Files: `engines/maxwell_3d.{h,cpp}`, `macro/macro.{h,cpp}`, `macro/macro_hle.cpp`, `macro/macro_interpreter.cpp`, `macro/macro_jit_x64.cpp`, `dma_pusher.{h,cpp}`.

**How it works:**
- `Maxwell3D::front_regs` is the decode side register file. The macro JIT and the interpreter read it. `regs` stays the execute side file, which the rasterizer and the HLE macros read.
- The front entry points (`CallMethod`, `CallMultiMethod`, `PushMethod`, `PushMethodRun`, `ConsumeSinkImpl`, `ProcessMacro`) do shadow RAM, write `front_regs`, and append to `Maxwell3D::stream`.
- Stream entries: register runs, executable calls, constant buffer data, inline data, HLE macro calls (`hle_calls`, with a copy of the parameters, segments, addresses and dirty flag) and the deferred draw at macro end.
- `DrainStream` replays the stream: `ProcessDirtyRegisters`, `ProcessMethodCall` and the HLE `Execute`.
- The front-only methods (shadow_ram_control, load_mme.*) run on the front. The back keeps `load_mme.start_address_ptr` equal.
- Each HLE macro has `FrontEffects`, which copies its register writes onto `front_regs`. HLE macros call `BackCallMethod` and `BackCallMultiMethod`, which skip shadow RAM.
- Where DmaPusher drains:
  - the end of `DispatchCalls`;
  - before puller methods and before executable methods of other engines;
  - before a SYNC_WAIT entry and before arming the next one;
  - before the macro or compute payload dirty check;
  - before each GP entry fetch, when the stream holds memory writes.
- Maxwell3D drains before `ClearCode` (queued HLE calls point to cached macros) and before a low-level macro refreshes dirty parameters. It also drains in an HLE call with dirty parameters whose front effects need them.

**Verify mode:** set `NXEMU_GPU_SPLIT_VERIFY=<file>`. After each drain it compares `front_regs` with `regs` and logs the first mismatch of each register.
- It costs a lot: about 45 fps in game (it scans 3584 registers per drain).
- TOTK run `split-verify-01`, 130 s with boot, menu and walk: **0 mismatches**. Rendering is correct on the screenshots.

**Fps (TOTK, interleaved, timers present but off, spin 300), mean and p5 from 75 s:**

| Run | Mean | p5 |
|---|---|---|
| base-01 | 110.1 | 107.6 |
| split-01 | 101.4 | 100.2 |
| split-02 | 101.7 | 99.9 |
| split-noverify-01 | 102.5 | 100.2 |

- base-02 is dropped: a key press was missed and it stayed on the "Load save data?" screen.
- **Step 1 costs about 8 %** (the stream write and replay on one thread).
- Correction: this table first used the mean from 62 s and showed 120-133 fps. From 62 to 70 s the game still loads at 200-270 fps, which inflates the mean by about 20 fps. Use the mean from 75 s.

**Notes for step 3:**
- Drains are frequent: puller methods, other engines, sync entries. Count them before choosing which ones to turn into real sync points.
- The macro payload dirty check drains the stream. On two threads, the execute side should check `IsMemoryDirty` on the HLE call segments itself instead.
- A low-level macro with dirty parameters drains, so it becomes a sync point.
- Front effects of `DrawArraysIndirect<true>` and `MultiDrawIndexedIndirectCount` guess the fallback path. The verify mode reports it if the guess is wrong.
- Possible next gain: skip front writes equal to `front_regs` (the back would skip them too). It needs the verify mode to stay clean first.
- Still to verify on the rest of the benchmark set (SMO, Civilization VII, a texture-heavy game).

## Step 3 result (2026-11-05)

Step 3 runs on a second thread. Step 2 (the other engines) was skipped: those engines run whole on the GPU thread. Not committed.

**Threads:**
- The new "GPUDecode" thread pops the command queue and runs `Scheduler::Push`, DmaPusher and the Maxwell3D front (shadow RAM, `front_regs`, macro JIT or interpreter, HLE `FrontEffects`).
- The "GPU" thread runs a `BackQueue` of work items under `execution_mutex`. The items are:
  - Maxwell3D stream blocks;
  - puller methods, except BindObject, which stays on the decode side because it changes the subchannels;
  - calls to the other engines, each with its sink, its dma segment and a copy of its payload;
  - BindChannel, FlushCommands and OnCommandListEnd, TickWork, FlushRegion and OnCacheInvalidation;
  - fence signals.
- `Maxwell3D::Publish` hands the stream to the GPU thread every 4096 words and at each point where the stream would drain. Blocks come from a pool.
- Sync primitives in `gpu_thread.h`:
  - `BackQueue::WaitIdle`;
  - `BackQueue::RunExclusive`: WaitIdle, then run a function under `execution_mutex`;
  - `ThreadManager::LockExecution`: lock the decode mutex, then WaitIdle, then lock `execution_mutex`. Callers from outside (CPU side flushes and invalidations) take it.
- Sync points on the decode side:
  - `ClearCode` (`SyncBack`);
  - a low-level macro whose parameters come from their own GP entry (`FrontRefreshParameters`);
  - an HLE macro with such parameters whose front effects need them;
  - a safe pushbuffer fetch of dirty memory (FlushRegion, then an unsafe read);
  - `SynchronizeMemoryOperations`.
- The HLE calls with payload parameters carry `recheck`: the GPU thread checks the dirty state and reads the parameters again before `Execute`.
- Toggle: `NXEMU_GPU_SPLIT_THREAD=0` turns the decode thread off. It is on by default when the GPU is async.

**Accepted hazards (to watch on other games):**
- No drain before a pushbuffer fetch when the stream holds constant buffer or I2M writes. A command list written by the GPU itself could be read stale.
- The decode side makes an early dirty check on macro payloads. The GPU thread checks again only for HLE calls (`recheck`).
- The CanSkip look-ahead on the next GP header is turned off with the decode thread.

**Verify mode:** each block carries a copy of `front_regs`, and the GPU thread compares it after the block runs. TOTK run `thread-verify-01`, 130 s: **0 mismatches**, correct rendering.

**Fps (TOTK, same DLL, interleaved, verify off, spin 300, mean and p5 from 75 s):**

| Run | Decode thread | Mean | p5 |
|---|---|---|---|
| one-01 | off | 100.0 | 97.0 |
| two-01 | on | 107.8 | 105.3 |
| one-02 | off | 101.4 | 99.0 |
| two-02 | on | 107.9 | 105.8 |

- **+7 %** against the split on one thread. Both rounds agree. All four 120 s screenshots show the same scene.
- Correction: this table first used the mean from 62 s (115 against 127, "+10 %"). See the step 1 note.

**Next (step 4):**
- Count the sync points (`BackQueue::SyncCount`) and the waits on each side.
- Spin before sleep in `PopWait` and `WaitIdle`, block size, thread placement.
- Run the rest of the benchmark set (SMO, Civilization VII, a texture-heavy game) in verify mode, then A/B.

## Step 4 result (2026-11-05)

**Counters** (TEMP, `NXEMU_GPU_SPLIT_STATS=<file>`, one line per second, in `gpu_thread.{h,cpp}`):
- In game there is **no sync point at all**. The 6140 sync points of a run are all during boot (LockExecution and ClearCode).
- With the step 3 queue the decode thread pushed **about 250,000 work items per second** (2,200 per frame).
  - About half were puller methods: each was one item and also cut the Maxwell3D stream into a small block (about 200 words).
  - Each item cost a lock, a `std::function`, a notify, and on the GPU thread a lock of `execution_mutex`, `ProcessPendingComposite` and `FinishOne`.

**Changes** (each has a TEMP env switch for A/B, default on):
- Batch pop (`NXEMU_SPLIT_BATCH`): the GPU thread takes all queued items at once and runs them under one `execution_mutex` lock.
- Lazy notify (`NXEMU_SPLIT_LAZY_NOTIFY`): Push wakes the GPU thread only when it sleeps.
- Puller coalescing (`NXEMU_SPLIT_PULLER_COALESCE`): the register-only puller methods (Nop, semaphore address and payload, syncpoint payload, WrcacheFlush) stay on the decode side. They go with the next puller method that acts, and they no longer cut the Maxwell3D stream. Only the puller triggers read these registers.
- Spin before sleep in `PopWait` and `WaitIdle` (`NXEMU_SPLIT_SPIN_US`, default 0).
- Block size (`NXEMU_SPLIT_BLOCK_WORDS`, default 4096).
- `WaitIdle` returns without the lock when the queue is already done (atomic mirrors of the counters).

**Effect on the queue:** 250,000 items per second become about 40,000. The GPU thread takes about 900 batches per second, with about 1,200 notifies.

**Fps (TOTK, interleaved, mean from 75 s):**

| Variant | Runs | Mean |
|---|---|---|
| step 3 queue (all switches off) | 104.4, 107.5, 104.1 | 105.3 |
| tuned | 107.6, 108.6, 107.4, 109.6 | 108.3 |
| tuned + spin 50 us | 108.3, 105.3 | 106.8 |

- Tuning: about +3 %. Spin: nothing.

**Against the code before the split** (pre-split DLL `nxemu-video.timers-base.dll`, interleaved):

| Run | Mean | p5 |
|---|---|---|
| base-01 | 108.3 | 105.4 |
| split-01 | 108.2 | 105.9 |
| base-02 | 111.7 | 109.8 |
| split-02 | 110.6 | 108.4 |

**G1 as it stands is neutral on TOTK.** The second thread and the tuning only win back what step 1 costs.

**Why:** in game the decode thread waits 648 ms per second (busy about 35 %). The GPU thread waits 261 ms per second (busy about 74 %). No GPU-side thread is saturated, so the frame is limited upstream:
- the guest CPU (JIT, services);
- or a per-frame wait of the guest on a GPU fence, which makes the GPU thread latency count without the GPU thread being full.

**Next:**
- Find what the guest waits on each frame: syncpoint waits in nvhost and the guest thread time per frame (CPU side profile).
- If the guest waits on GPU fences, the latency of the GPU thread work matters, and tracks 2 and 3 (prepare draws in parallel) become the lever.
- If the guest CPU is busy, the JIT is the lever (track J).
- The split costs about 8 % on one thread. If G1 stays, make step 1 cheaper; if not, drop it and keep the cheap parts (puller coalescing does not need the split).
- Before any commit: remove the TEMP counters and switches, and decide if G1 stays at all.

## Skip unchanged register writes (2026-11-06)

At about 127 fps the GPU thread is the limit (busy about 90 %). Replaying the stream on it costs about 6 % (`RunWords` 4.4 % self, `ProcessDirtyRegisters` 1.4 %). `ProcessDirtyRegisters` already ignores a write of the value the register holds.

**Change:** `FrontWrite` returns early when `front_regs` already holds the value. `PushMethodRun` goes through `FrontWrite` too, so a run only carries the changed values.
- This is safe because the execute side file equals `front_regs` at each stream position. The execute side `regs` is only written by `ProcessDirtyRegisters`, `ProcessCBData` (offset, mirrored by `FrontMethodCall`), `falcon[4]` (mirrored) and the HLE macros (mirrored by `FrontEffects`).
- If a front effect is ever wrong, a skipped write would leave the execute side stale. The verify mode reports it.
- TEMP switch: `NXEMU_SPLIT_SKIP_SAME=0` turns it off.

**Verify:** TOTK run `skip-verify-01`, 130 s: 0 mismatches, correct rendering.

**Fps (TOTK, same DLL, interleaved, mean from 75 s):**

| Run | Skip | Mean | p5 |
|---|---|---|---|
| skip-0-1 | off | 127.3 | 125.1 |
| skip-1-1 | on | 130.1 | 128.2 |
| skip-0-2 | off | 125.8 | 122.5 |
| skip-1-2 | on | 129.3 | 125.9 |

**+2.5 %**, both rounds agree.

## DrawTimers compiled out (2026-11-06)

`draw_timers.h` now compiles the timers out unless `NXEMU_DRAW_TIMERS_BUILD=1` is defined. On top of the skip DLL (`skip1` vs `notimers`, interleaved, mean from 75 s): 127.2 / 131.4, then 129.9 / 129.0. Mean 128.6 vs 130.2, about +1 %, within noise. The timers are TEMP and go away before any commit anyway.

## Recipe recorder on its own thread (2026-11-06): no gain

`renderer_vulkan/guest_pipeline_recorder_async.h` wraps the recorder. The GPU thread keeps the unchanged-draw test, gathers the changed register groups, the key prefix (when it changed) and `key.state` into a batch. A "GuestRecipes" thread keeps a copy of the tracked registers and runs `ObserveState` (hash, lookup, capture, journal). Prefix ids for the pipeline memo are assigned on the GPU thread (top bits `1 << 62`). TEMP switch `NXEMU_RECIPE_RECORD_SYNC=1` = old path.

- Journal check, empty journal, 130 s: async 45,485 bytes, sync 47,753 bytes, no `.failed` marker, correct rendering.
- Fps (TOTK, same DLL, interleaved): sync 133.9 / 131.7, async 132.5 / 126.1 (p5 116.8). **No gain, maybe a loss.**
- Likely reasons: the gather and the state copy stay on the GPU thread and are most of what the cheap part cost; the extra thread competes for cores with the guest cores, decode, GPU and the spinning Vulkan worker.
- Not kept: the kit stays on `notimers`. Drop the file (and restore `GuestPipelineRecorder` in `vk_pipeline_cache`) unless a later change makes it pay.

## Draw memoization: draws do not repeat (2026-11-06)

Measured with a TEMP counter in `PrepareDraw` (removed), TOTK in game, about 4,000 draws per frame:

- Hash of all Maxwell registers at draw time: 0 % found again in the previous frame. Even with the inline constant buffer registers left out: 0 %. Same as the draw just before: 9 %.
- Same draw identity (shader addresses, index buffer, vertex range) seen again: only 11 % have all registers the same. This is an upper bound, because the bound constant buffer addresses are not in the registers.
- Registers that change between two uses of the same draw: `const_buffer` address (55 %, a per-frame uniform ring), `render_enable` and `report_semaphore` addresses (45 %, queries), `const_color_rendering`, render target, vertex stream limits.

So there is nothing to replay: every draw has to bind its new uniform buffers. Partial memoization does not help either. `UpdateRenderTargets` already has a fast path when the targets are clean; what is left is `PrepareImageView` for each target, which keeps the image modification order.

## Decode thread lock wait: not on the critical path

The profile showed the GPUDecode thread blocked 12 % of its time on the texture cache lock in
`MustFlushRegion`. It comes from the early dirty check of macro payloads in `DmaPusher::Step`
(High GPU accuracy is the desktop default, so the texture cache lock is taken). A test made that
check non-blocking (try_lock, a busy lock counts as dirty, safe because the back side ORs its own
recheck in). TOTK, 3 rounds, mean from 75 s: wait 137.6, no wait 137.4. Neutral: the decode thread
runs ahead of the GPU thread, so its wait does not delay the frame. The change was removed.

## Moving the "pure" draw steps to the Vulkan worker: no gain to take

The steps that looked pure (dynamic state, image descriptor push, ConfigureDraw, draw record,
about 146 ns per draw) already only check dirty flags and hand data to the worker through
`scheduler.Record`. The Vulkan calls already run on the worker. Moving them would mean copying
the same register data to the worker, which is the work being done today. Not done.

## Guest syncpoint waits: read-cached pages were not protected on Windows (2026-11-06)

What the guest waits on:
- NV syncpoint waits went from about 2.6 ms/s to 40-90 ms/s: 17-40 real waits/s, about 2.3 ms each.
- Fence stats (TEMP `NXEMU_FENCE_STATS`): all of the ~550 syncpoints/s were delayed through
  `pending_delayed_fences`, so `IncrementHost` ran only after the host GPU finished the work.
- The cause was about 830 unprotected texture/query flushes per second.
  `Memory::ProtectsReadCachedRegions()` returned false on Windows; it came with #543, for macOS.
  So every read-cached region counted as an unprotected flush and forced a delayed fence.

Fix (`nxemu-os/core/memory.cpp`): on non-Apple hosts, return true when there is a fastmem arena
and reactive flushing is on. The Windows path of `RasterizerMarkRegionReadCached` already denies
access to read-cached pages in the arena, so a guest read faults and flushes on demand.

TOTK, 3 rounds, mean from 75 s (TEMP `NXEMU_NO_READ_PROTECT=1` gives the old behavior):

| Round | Old | Protected |
|---|---|---|
| 1 | 134.2 | 135.4 |
| 2 | 134.2 | 138.0 |
| 3 | 138.1 | 139.0 |

**About +1.5 %**, ahead in every round. Guest core idle drops from 21-22 % to 16-17 %. Delayed
syncpoints go to 0, and NV wait time to 11-20 ms/s at about 0.5 ms each. The trees at 125 s are
fine.

Open points before a commit:
- the Windows protection covers only pages of type `RasterizerCachedMemory`; check query result
  pages, which may not have that type;
- test other games.

Not kept: delaying only fences that need it, without chaining through `pending` (TEMP
`NXEMU_FENCE_CHAIN=1` is the old behavior). No change alone.

## HostTiming: shorter busy wait before timing events (2026-11-06)

HostTiming was 99 % busy. Before each event it sleeps on a high-resolution waitable timer, but
stops one host timer tick early (0.5 ms) and busy-waits (`MWAITX`) the rest. Events come often,
so it busy-waited almost all the time, on a logical CPU that shares a core with a guest core or a
GPU thread.

TEMP `NXEMU_TIMER_MARGIN_US` sets that margin (`core_timing.cpp`, `TempTimerMarginNs`).

TOTK, mean from 75 s:

| Round | 500 us (default) | 100 us | 20 us |
|---|---|---|---|
| tm 1 | 137.4 | 142.1 | 139.7 |
| tm 2 | 140.4 | 140.8 | 142.2 |
| tc 1 | 131.5 | 126.5 | |
| tc 2 | 126.3 | 131.3 | |
| tc 3 | 131.7 | 120.6 (a 15 s dip at 85 fps, likely outside load) | |

The tc rounds ran on a machine that was about 10 fps slower overall. The fps effect is within the
noise. The CPU effect is clear: HostTiming drops from 99 % to 13 % of a core with 100 us. Worth
keeping for power and for machines with fewer cores; check that timing events are not late
(audio, frame pacing) before making it the default.

### Check on Super Mario Odyssey (2026-11-06)

Old = no read protection, 1 ms timer margin. New = read protection, 100 us margin. Mean from 75 s,
2 rounds each:

| Config | Round 1 | Round 2 |
|---|---|---|
| Old | 325.6 | 303.7 |
| New | 324.0 | 324.2 |

No regression, and the picture is correct at 145 s. SMO runs at about 325 fps with 61 % idle guest
cores, so it is not CPU bound and the two changes do not show there. The old round 2 is an outlier
in the same scene.
