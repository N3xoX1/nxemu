# Ambitious performance plan (TOTK, target 140+ fps)

Read `HANDOFF-draw-cuts.md` first: it has the measurements, the tools, the kit and the user's rules. Delete both files before merging or opening a PR.

## Where we are

TOTK runs at about 109 fps (p5 107.6) without PGO, on branch `perf/draw-cuts` with the worker spin. Two walls limit it:

- **GPU thread:** it translates about 4,600 draws per frame, one after another, at about 1.7 µs each. Draws take 84 % of its busy time. Command decode and macros take about 15 %.
- **Guest cores (JIT):** about 6-7 ms of work per frame. They become the limit near 125-140 fps.

So **both** walls must move. Work in four tracks. Track 0 comes first, because it decides how far tracks 2 and 3 can go.

## Track 0: measure what can run in parallel (1-2 days)

**Goal:** split the 1.7 µs of each draw into two kinds of work:
- **ordered:** it must run in draw order on one thread, because it mutates caches, syncs memory or depends on earlier draws;
- **pure:** it only reads a snapshot and builds data.

**How:**
1. Add TEMP rdtsc scope timers, enabled by an env var, around each step of:
   - `RasterizerVulkan::PrepareDraw` (`renderer_vulkan/vk_rasterizer.cpp`);
   - `GraphicsPipeline::ConfigureImpl` and `ConfigureDraw` (`vk_graphics_pipeline.cpp`);
   - `BufferCache::BindHost*` and `UpdateGraphicsBuffers`;
   - `TextureCache::FillImageViews`, `UpdateRenderTargets` and `GetFramebuffer`;
   - `UpdateDynamicStates` and `RefreshVertexInput`.
2. Accumulate the timers per frame and print one line per second to the log. Never measure fps with this on.
3. Also count, per draw, how often each lookup **hits** (same result as a read-only lookup would give) and how often it **mutates** (creates an image or view, uploads data, invalidates).
4. Put each step in one of three classes:

| Class | Meaning | Examples |
|---|---|---|
| A, pure | Only converts register state into Vulkan data | dynamic states, vertex input, descriptor writes, push constants |
| B, read-only on a hit | A cache lookup that only reads when it hits | texture view by TIC handle, sampler by TSC, pipeline by key, buffer id by address |
| C, ordered | Must stay in draw order | memory sync and uploads, image creation, render target changes, flushes and queries |

**Decision:**
- If A + B is at least 40 % of draw time, tracks 2 and 3 are worth it: up to +30-40 % on the GPU thread.
- If A + B is under 25 %, do track 1 and the JIT track first.

## Track 1: sync buffers at harvest points, not on every draw (large, about 1 week)

**Problem:** every draw calls `SynchronizeBuffer` for every vertex, index, storage and texture buffer. Each call walks the memory tracker bitmaps. Together with `TouchBuffer`, the tracker walks and UBO streaming, the buffer cache is about 29 % of the GPU thread.

**Key fact:** guest CPU writes only become visible to the GPU at harvest points:
- `GPU::InvalidateGPUCache` calls `GatherGPUDirtyMemory`, which calls `Memory::HarvestCpuWrites` (G2 write map);
- inline GPU writes go through `MemoryManager::WriteBlock*`.

Between two such points, no buffer can become CPU-dirty. The per-draw sync is therefore almost always useless.

**Design:**
1. Keep a global `cpu_dirty_epoch`, bumped whenever pages become CPU-modified: harvest, `WriteMemory`, `CachedWriteMemory`, inline writes. For more precision, keep one epoch per 2 MiB region.
2. Each binding slot (vertex[i], storage[stage][i], index, texture buffer) keeps `(buffer_id, addr, size, epoch_seen)`. If they are unchanged, skip `SynchronizeBuffer`, `TouchBuffer` and the bitmap walks. Only re-emit the bind when the host binding changed.
3. Even better: at the harvest point, upload the dirty pages of all bound or recently used buffers in one batch. Per-draw binding then becomes a pure lookup, which is class B and can move to workers in track 3.
4. Keep `MarkWrittenBuffer` and the GPU-modified handling per draw. They are rare, and needed for correctness.

**Expected:** -10 to -20 % of GPU thread time, about +8-15 % fps.
**Risk:** a missed CPU write gives stale geometry. Test several scenes, not only the save, and take several screenshots each time.

## Track 2: a prepare stage for the pure work (A), on workers (large, about 1-2 weeks)

**Design:** add a stage between the GPU thread and the Vulkan worker.
- The GPU thread does only class C and B, then pushes a compact **draw packet**. The packet holds:
  - the pipeline pointer;
  - the resolved image view and sampler ids;
  - the buffer handles with their offsets and sizes;
  - a copy of the few registers the pure part needs (viewport, scissor, blend constants, stencil, vertex streams and attributes, primitive restart…).
- A pool of 2-3 **prepare workers** turns packets into ready-to-record data:
  - descriptor update templates filled;
  - dynamic state values computed;
  - vertex input state built.
  Each worker takes a block of N consecutive draws.
- The Vulkan worker records the prepared blocks **in draw order**, so no secondary command buffers are needed.
  - Render pass begin and end, barriers and query commands stay as ordered markers inside the packet stream.

**Main files:**
- `vk_graphics_pipeline.cpp`: split `ConfigureImpl` into resolve (ordered) and build (pure);
- `vk_rasterizer.cpp`: `PrepareDraw`, `UpdateDynamicStates`, `RefreshVertexInput`;
- `vk_scheduler.*`: chunk and worker model;
- `pipeline_helper.h`: `PushImageDescriptors`;
- the descriptor queues: `vk_update_descriptor`, `guest_descriptor_queue`.

**Watch out for:**
- state the pure part reads from `maxwell3d->regs` directly: everything must come from the packet;
- the scheduler's `state` tracking (current pipeline, render pass): keep it on the ordered side;
- the `dirty` flags: decide them on the ordered side and carry them in the packet.

## Track 3: read-only lookups on workers, fall back to the ordered thread on a miss (very large, 2-4 weeks)

**Idea:** 84 % of draws reuse the same pipeline and texture handles, and most buffer lookups hit. Class B work only reads when it hits, so prepare workers can do it ahead of the ordered thread. The ordered thread then mostly checks and commits.

**Design:**
- **Snapshot epochs.** Each cache publishes an immutable read view, valid while its epoch is unchanged:
  - texture cache: `GraphicsDescriptorEpoch` already exists, extend it to image and view lifetime;
  - buffer cache: a buffer map epoch;
  - pipeline cache: the pipeline map.
  Mutations (new image or view, deletion, rescale, buffer overlap merge) bump the epoch and publish a new view. Use RCU-style deferred free: retire objects only after workers pass the epoch.
- **Speculative resolve.** A worker resolves draw N's handles against the current read view and tags the result with the epoch it used.
- **Ordered commit.** For each draw in order, the ordered thread checks that:
  - the epoch is unchanged;
  - no class C event happened since the snapshot (memory sync, render target change, image or view creation).

  If both hold, it commits and does only the ordered side effects: LRU touch, usage marks, feedback-loop check, written-buffer marks. These can be batched. Otherwise it **falls back** and resolves the draw itself, in order, exactly like today.
- **Register snapshots.** Workers need the register and constant buffer state of draw N. Two options:
  - a register **delta journal**: the methods written since the previous draw, applied to a worker-side register copy;
  - small per-draw snapshots of only the registers the resolve reads, which is cheaper.
  Measure first.

**Goal:** the ordered thread costs about 0.5 µs per draw instead of 1.7. That is about +40-60 % on the GPU thread, until the guest wall.

**Risks:**
- subtle ordering bugs (a texture rendered by draw N-1 and sampled by draw N);
- hard to debug.

Build it behind a setting, with a "verify" mode that resolves both ways and compares the results.

## Track 4: the JIT (needed past ~130 fps)

Facts from the profile (`perf-experiments/profiles`, `tools/jitmap.py`):
- guest JIT time is flat: the top 10 blocks are 10 %, the top 1000 are 51 %;
- dispatcher and return stack buffer are about 4 %;
- kernel and services are under 2 %.
So the gains must be general code quality, not HLE of a few functions.

**Steps:**
1. **Cheaper G2 write tracking** (`dynarmic/backend/x64/emit_x64_memory.h`, `EmitWriteTrack`).
   - Today each store does: mov, shr, mov imm64, cmp, je, plus 2 scratch registers. That causes spills in hot blocks.
   - Option a: pin the write map base in a register, as R13 (fastmem) and R14 (page table) already are in `a64_emit_x64.cpp`, `gpr_order`.
   - Option b: put the map at a fixed offset from the fastmem base, so one register and a displacement are enough.
   - Option c: track at a coarser grain (64 KiB) and keep the page map only for regions the GPU caches.
   - Measure with `cpu_debug_mode` off and the perf map on.
2. **Measure codegen quality** on the 50 hottest blocks.
   - Dump the x64 next to the A64 (dynarmic has a disassembler).
   - Look for flag materialization (NZCV written and never read), redundant loads and stores of guest registers between IR blocks, and memory accesses going to the slow path.
3. **Larger blocks and cross-block register allocation:**
   - raise the block size limit;
   - link blocks with a direct jump and keep guest registers in host registers across the link when both blocks agree (a big dynarmic change);
   - try cheap wins first: `MiscIROpt` passes, dead flag elimination at block ends.
4. **Exclusive monitor and atomics:** check the samples in `ExclusiveRead` and `ExclusiveWrite` callbacks (`nxemu-cpu+0x…` in the profile) and move them to fastmem exclusive paths where they are safe.

**Expected:** -10 to -20 % guest time, which moves the guest wall to about 150 fps.

## Other big ideas, ranked

1. **PGO retrain after each track.** Measured +8 % before the code changed. Cheap, always do it last.
2. **Dedicated thread for command decode and macros** (the ~15 % front end). Natural once track 3 has register snapshots, nearly free then.
3. **UBO streaming in one copy per draw:** gather all small UBOs of a draw into one stream allocation and one `ReadBlockUnsafe` per contiguous guest range. About 2 %.
4. **Direct UBO from imported guest memory:** re-test with several screenshots. The earlier "bug" may have been lighting. It is correct with host-synced fences (`patches/direct_ubo_full.patch` in `perf/experiments-archive`), but the gain was small.

## Track 5: the GPU / guest sync model (all games)

**Problem:** guest-visible fences (semaphores, syncpoints) are signalled when the GPU thread reaches them, not when the host GPU has run the work. Two things follow:
- Anything that needs the real GPU result stalls the whole pipeline: CPU reads of GPU-written memory, queries, conditional rendering, `WaitForIdle`. The GPU thread waits for the host GPU, then the guest waits for the GPU thread.
- Optimizations that depend on correct ordering are blocked. Direct UBO reads from guest memory are one example: they broke because the game recycled its UBO ring too early.

**Direction:**
- Timeline semaphores mapped to guest syncpoints, signalled when the host GPU finishes.
- Queries and conditional rendering resolved on the GPU (`VK_EXT_conditional_rendering`) instead of read back.
- Async readback of GPU-written buffers, with predictive flush of ranges the CPU read in earlier frames.

**First step:** count, per game, the GPU thread waits on the host GPU and the guest waits on GPU fences. Use timers like track 0.

## Track 6: host thread scheduling (all games)

From `ARCHITECTURE-PERF.md` (kit folder) C1-C3, not done yet:
- **Thread placement:** the GPU thread, the main guest core and the Vulkan worker each get their own physical core.
- **CoreTiming:** no core spinning on `mwaitx`.
- **Kernel scheduler lock:** today one global lock, 8-13 % of guest core time. Move to per-core run queues.

These matter most in games with many guest threads. `Placement.ps1` in the kit is a start for placement.

## Track 7: texture cache and copies on the GPU (games heavy on render-to-texture)

- Swizzle and unswizzle, format conversion and overlap resolution run on the CPU or on the GPU thread in many paths.
- Moving them to compute shaders and async copies takes them off the critical thread.
- Measure first on texture-heavy games. TOTK is light here.

## Track 8: shader and pipeline compilation (stutter, all games)

- This does not change the mean fps, but it is the first thing users notice.
- Use `VK_EXT_graphics_pipeline_library`: fast-linked pipelines first, optimized ones built in the background.
- Keep the disk cache shared across driver updates.

## Benchmark set (to avoid tuning only for TOTK)

- Every architecture step is measured on at least 4 games with different profiles:
  - TOTK: many draws, heavy GPU thread.
  - Super Mario Odyssey: light, very high fps, front-end overhead.
  - One game with many guest threads or heavy CPU, such as Civilization VII.
  - One game heavy on render-to-texture or readback.
- The track 0 timers become a permanent, off-by-default profiling option, so the same breakdown works for any game.
- Only SMO and TOTK have been measured in the kit so far.

## Order of work and rules

Order:
1. Track 0 (measure).
2. Track 1 (buffer sync at harvest).
3. Track 4 step 1 (cheap write tracking).
4. Track 2 or 3, depending on track 0.
5. Track 4 steps 2-4.
6. PGO retrain.

Rules:
- Each step lands as its own commit on `perf/draw-cuts`, or on a new branch per track.
- Commit titles are short, simple English, with a short description and no Claude or Co-Authored-By line.
- Remove TEMP code before committing.
- The user opens PRs. Ask before pushing, unless the user said to push.
- Measuring: one run per test, look at p5 as well as the mean, take several screenshots, and never kill the other nxemu (from `NXFormat_SmartBlocks`).
