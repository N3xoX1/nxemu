# Design: make the guest CPU JIT much faster (track J)

Status: kept JIT changes committed (2026-11-06); the region JIT was dropped, see the block-body profile. Read `HANDOFF-draw-cuts.md` ("where the TOTK frame time goes") first. Delete before merging.

**Cleanup (2026-11-06):** the TEMP counters, timers and env switches named in this file were removed before the commit. They stay here as a record of how each result was measured. The kept defaults are the ones that were measured "on".

## Why the JIT

On TOTK (RTX 5070 Ti, Ryzen 7 9800X3D, about 108 fps), the frame is bound by guest CPU work, not by the GPU side:

- The guest almost never waits on the GPU (nvhost 2.6 ms/s, buffer queue 17 ms/s).
- The GPU-side host threads have slack (decode thread 35 %, GPU thread 74 %).
- Guest core time: JIT 72-78 %, kernel/HLE about 5 %, idle 17-24 %.

New host profile (stackprof, 15 s from 96 s, jit perf map):

| Guest thread | In JIT code | nxemu-cpu C++ (FP helpers) | nxemu-os | Waiting |
|---|---|---|---|---|
| Main (critical) | 75.0 % | 2.9 % | 3.9 % | 17.4 % |
| Worker 1 | 59.4 % | 1.7 % | 3.4 % | 36.5 % |
| Worker 2 | 55.7 % | 1.5 % | 1.8 % | 40.6 % |

- The dispatcher is 0.03 % of the JIT samples, so block linking, RSB and fast dispatch already work. **All the time is inside the generated code.**
- The profile is flat:
  - the top 10 blocks are 6 % of the JIT time;
  - the top 100 are 16 %;
  - the top 1000 are 38 %.
- There is no hot loop to hand-optimize. Only a generic gain in code quality moves the frame.
- The main thread is about 75 % JIT, so a JIT speedup of X gives roughly 0.75 X in fps.

## What the generated code looks like

Sample-weighted over all hot blocks (block stats from `a64_emit_x64.cpp`, TEMP):

| Per block | Value |
|---|---|
| Guest instructions | 14.0 |
| IR instructions | 64 |
| GPR get / set (JitState) | 3.6 / 3.6 |
| Vector get / set (JitState) | 2.5 / 2.7 |
| Guest reads / writes | 3.7 / 2.5 |
| Host code size | 1016 bytes (hot path about 530, the rest is cold deferred code) |
| Terminal | If 47 %, CheckBit 36 %, LinkBlock 9 %, RSB 5 %, FastDispatch 3 % |

Blocks are short: 50 % of the time is in blocks of 7 guest instructions or fewer. Every block loads its guest registers from `JitState` (r15) and stores them back at its end. 85 % of blocks end in a conditional branch, so flags and registers cross a block boundary every 7-14 instructions.

Hot-path disassembly of the 33 hottest blocks (`NXEMU_JIT_DUMP_PCS`, capstone): 387 guest instructions become 3278 host instructions, **8.5 host instructions per guest instruction**.

| Waste in the hot path | Share of host instructions | Who fixes it |
|---|---|---|
| JitState store later overwritten in the same block | 5.3 % | in-block get/set elimination |
| JitState load right after a store to the same slot | 4.3 % | in-block get/set elimination |
| JitState load at block start / final store at block end | 3.5 % + 6.7 % | regions with cross-block register allocation |
| Write tracking (`mov, shr, mov, cmp, je` per store) | 6.9 % | page-protection tracking, or coalesced checks |
| Stack spills with free registers left | about 5 % | register allocator |
| `insertps ..., 0xe` (zero upper lanes) and `mov r32, r32` zero-extend | 4.0 % + 3.3 % | IR peephole / FP emitters |
| Flags through memory (`lahf/sahf`, FCMP lookup table, NZCV store + reload) | about 5 % | keep NZCV in a host register / EFLAGS |
| NaN checks after vector FP ops | about 3.5 % | accuracy setting (Auto keeps them) |
| Halt check at linked exits | 2.0 % | needed (could merge with cycle check) |

**About 40-50 % of the host hot path is overhead that a better JIT would not emit.** Instruction count is not time. Stores that hit L1 are cheap, but the store-to-load round trips (`movaps [r15+x], xmm; movq xmm, [r15+x]`) and `sahf`/`lahf` sit on dependency chains and cost 4-5 cycles each.

Two examples:

- **memcpy loop at 0x84703500** (LDP/STP x4 + SUBS + B.HI, 10 guest instructions):
  - It becomes 68 host instructions.
  - Write tracking alone is 40 of them: 8 stores × 5, and all 8 hit the same page.
  - The 8 data registers go to JitState and come back every iteration.
  - SUBS is `sub, cmc, lahf, seto, store, reload, sahf, cmc`.
  - Native work is about 12 instructions.
- **FP block at 0x8008b5e0** (FCMP + FCSEL x3, vector FMUL/FADDP, 21 guest instructions):
  - Every FCMP goes through a 64-bit lookup table into `JitState.nzcv`, and every FCSEL reloads it and does `sahf`.
  - `SetQ v7` is followed by `GetD v7`, and the round trip goes through memory, because get/set elimination does not forward across widths.

Runtime facts:

- **One JIT per guest core, with a private code cache.** In 130 s, TOTK compiled 1.16 M blocks for 579 k distinct PCs, about 2× duplication across the 3 game cores. That costs compile CPU during loading, plus i-cache and ITLB footprint.
- `FPRecipEstimate`, `FPRSqrtEstimate` and `FPUnpackBase` are called out of the JIT into C++: 1-3 % of the main thread.

## What Ryujinx (ARMeilleure) does differently

Read in `ref/Ryubing-master/src/ARMeilleure`:

1. **Function regions.** `Decoder.Decode(..., MultipleBlocks)` follows branches inside a function:
   - up to 2500 instructions in high quality, 500 in low quality;
   - it stops at calls;
   - loops compile as one unit.
2. **Tiering.** A function is first compiled in low quality. `EmitRejitCheck` counts calls, and after 100 calls a background thread recompiles it in high quality (`Translate(highCq: true)`).
3. **Liveness at region edges.** `RegisterUsage.RunPass` computes which guest registers are live in and out. Only those are loaded at entry and stored at exit. Inside the region, guest registers become locals → SSA → linear scan allocation.
4. **Real optimizer.** SSA construction, constant folding, simplification, dominance, block placement, and a linear-scan register allocator over the whole region.
5. **Host-mapped memory.** No per-access check. Write tracking for the GPU cache uses host page protection and a signal handler (`MemoryEhMeilleure`), not code in every store.
6. **One shared translation cache** for all cores (`FunctionTable` / `AddressTable`), plus the PTC disk cache.

ARMeilleure as a whole is not faster than dynarmic: its FP and SIMD emitters are not better, and Ryujinx is not faster than yuzu on TOTK. The ideas are worth taking; the code is not worth porting.

## Options

### Tier A: inside dynarmic, low risk, a few weeks, estimated +8-15 % JIT

- **A1. Write tracking.**
  - Short term: coalesce the checks for stores off the same base register in one block. Check the first and last address once, and skip the rest when they share a page.
  - Long term: track writes with host page protection (write-protect a tracked page, mark it in the fault handler, unprotect it). This removes all 6.9 %, and the memcpy loop drops from 68 to about 30 host instructions.
  - The video side only needs "page written since last check", which is exactly what the protection fault gives.
- **A2. Better in-block get/set elimination.**
  - Forward `SetQ` → `GetD`/`GetS` (any narrower width), and drop stores that a later set overwrites.
  - Forward `SetNZCV` to later NZCV readers in the same block, so FCSEL/CSEL/B.cond do not go through `JitState.nzcv` + `sahf`.
  - About 10 % of host instructions, all in `ir/opt/a64_get_set_elimination_pass.cpp`.
- **A3. Register allocator.** Find why values spill to `[rsp+0x30]` while r8-r12 are free. It is likely immediate operands and `ScratchGpr` choices.
- **A4. FP emitters.**
  - Remove the redundant `insertps 0xe` / zero-extend pairs. The IR does `GetQ` + `VectorGetElement32` where a scalar load would do.
  - Inline FRECPE/FRSQRTE: use a table, or the `rcpps`/`rsqrtps` path when reduced FP error is allowed.

Each step can be measured alone with the same TOTK protocol (means from 75 s). A1 and A2 are the first ones to try.

### Tier B: region JIT inside dynarmic, structural, 2-4 months, estimated +20-40 % JIT

- **B1. Multi-block regions.**
  - Translate a function-like region: follow conditional branches and loops, stop at calls and indirect jumps, cap at about 500 guest instructions.
  - Keep guest registers and NZCV in host registers across the blocks of the region.
  - Spill only the live-out registers at exits. This is the liveness pass from ARMeilleure, applied to dynarmic IR.
  - This removes the 10 % of block-boundary loads and stores, and most of the flags traffic. Loops like memcpy run 2-4× faster.
  - Needs: a multi-block IR (today `IR::Block` is one basic block with one terminal), a global register allocator, and exits that write back the right registers.
- **B2. Tiering.** Compile one block first, as today. Count entries, and after N entries recompile the region in the background, then patch the links. The profile is flat (top 1000 blocks = 38 %), so the threshold must be low and the background compiler fast.
- **B3. One code cache shared by the cores.** This halves compiles, i-cache and ITLB footprint. It needs a thread-safe cache and patching, and a different invalidation model.

### Tier C: replace dynarmic

Not recommended. A new JIT from scratch, or a port of ARMeilleure, would take 6+ months before it matched dynarmic correctness on all games. Tier B gets the main benefit (regions + global register allocation) and keeps dynarmic's emitters and its test suite.

## Plan

1. A1 short term (coalesced write-tracking checks) and A2 (width-aware get/set and NZCV forwarding). Measure each on TOTK, then check that the block stats and the dump show the waste is gone.
2. A3 and A4.
3. Decide on B1 using the tier A numbers. If A gives about 10 %, B1 is the project that moves the ceiling.

The TEMP tools stay until the end of the track:
- `NXEMU_PERF_MAP_DIR`: perf map plus `blocks-<pid>.txt`;
- `NXEMU_JIT_DUMP_PCS`: IR and host code of chosen PCs;
- `prof-tools/stackprof2.exe` with `STACKPROF_RAW`.

Remove them before any commit.

## Tier A result (2026-11-05)

Built in the worktree, uncommitted. TOTK, means from 75 s, same kit, `nxemu-cpu.jitbase.dll` vs `nxemu-cpu.jitc.dll`:

| Run | Base | Tier A |
|---|---|---|
| Pair 1 | 105.7 | 113.4 |
| Pair 2 | 104.0 | 110.3 |

**About +6.5 % fps.** Screenshots match the base: same trees, same scene.

What is in it:
- **A1 write tracking.**
  - The map sits below 2 GiB, so the check uses a 32-bit displacement: 4 instructions and 1 scratch register instead of 5 and 2.
  - Writes off the same base value, less than a page apart and with nothing between them that may leave the block, are grouped. Only the last write of a group of 3 or more marks the map, for the lowest and the highest address.
  - The fallback path of that last write comes back to the marks.
  - Code: `PlanWriteTracking` in `a64_emit_x64.cpp`, `EmitWriteTrack` in `emit_x64_memory.h`.
- **A2 get/set elimination** (`a64_get_set_elimination_pass.cpp`): forwards X→W, W→X after a set, Q→D, and D→Q after a set, instead of reading JitState.
- **FRINTA inline** for 32 and 64 bits (`EmitFPRound`), instead of the C++ fallback.
- **Register allocator** (`reg_alloc.cpp`, `a64_emit_x64.cpp`). This is the biggest part of the gain: spills in the hot blocks went from 6.4 % to 0.3 % of host instructions.
  - A GPR value moved out of the way goes to a free callee-saved register before the stack.
  - The A64 GPR order lists callee-saved registers first, and RAX, RCX and RDX last.
  - XMM order: callee-saved first.

**Open bug.** Doing the same move for XMM values (into a free xmm6-15) removes many trees in TOTK. That is a real miscompile, not streaming. It is reproducible, and it stays when the moved value gets its upper lanes cleared like a spill reload does. The cause is not found. It is disabled, so XMM values still spill to the stack. Find it before trusting callee-saved XMM in tier B.

TEMP env switches, to remove before a commit:
- `NXEMU_JIT_NO_FRINTA`
- `NXEMU_JIT_NO_MOVEAWAY`
- `NXEMU_JIT_NO_REGORDER`
- `NXEMU_JIT_NO_XMMORDER`

## Tier B plan, step by step

The hot blocks almost never loop on themselves (6 of the 40 hottest end in a back edge). They are short forward if/else blocks, so the right shape is **traces with side exits**, not loop regions:

1. **B1 traces.** At a conditional terminal, keep translating the likely path in the same IR block.
   - The other path becomes a side exit: a new IR op that leaves to its target when the condition holds.
   - Get/set elimination treats a side exit as a read of all registers.
   - This removes the reload at the next block start, the link/halt overhead, and gives the allocator a longer scope.
   - Likely path: backward branch taken, forward branch not taken. Cap at about 64 guest instructions.
2. **B2 sink stores into exit stubs.** Registers set before a side exit are stored in the cold exit stub instead of on the hot path. Only the trace end stores them on the hot path.
3. **B3 tiering.** Count side-exit hits. When one exit is hot, recompile the trace with that path inline.
4. **B4 shared code cache** across cores: one compile per PC instead of about two.

## Tier B result (2026-11-05)

TOTK, means from 75 s. Runs stuck on a menu are left out.

| Step | Build | Result |
|---|---|---|
| B1 traces | `jittf` | Kept. About 124 fps against about 114-117 for tier A, roughly +5 % in steady state. |
| B2 stores moved into exits | `jitts` | Neutral: 123.4 against 124.3. Off by default, `NXEMU_JIT_SINK` turns it on. |
| Follow BL and far B | `jitcall` | -4.5 %: 118.7 against 124.2. Off by default, `NXEMU_JIT_FAR` turns it on. |

B1 traces:
- Forward B.cond, CBZ/CBNZ and TBZ/TBNZ become `A64SideExit`, and translation goes on along the fall-through path.
- Short forward B (up to 64 instructions) is followed in the same block.
- Traces are capped at 64 guest instructions.

B2:
- A GPR store replaced later in the block is done only in the cold exit stub.
- It works (once the stub no longer clobbers rax), but removes too few stores to show.

Follow BL / far B:
- Blocks get several code ranges (`Block::AddCodeRange`), each registered for invalidation.
- It is slower, probably because callee code is copied into every caller and the 64-instruction cap fills with callee code.

Total so far: base about 105 fps, now about 124 fps, about +18 %.

## Step 4 result (non-JIT overhead)

Measured on TOTK with per-SVC kernel time, per guest thread wait time and wake-to-run delay:
- Kernel time in SVCs is about 5 % of guest core time. Making SVCs free would give at most about 5 %.
- Threads wait almost only in WaitForAddress (0x34): main thread t74 about 46 % of the time, the 5 workers 55-70 %. It is the game's own job system waiting on itself.
- Wake-to-run delay is about 5-6 us without spin, about 2 us with a 50 us idle spin. Main thread: about 3300 wakes/s, so under 1 % of time.
- Idle spin 50 us: about +2 % (125.0 / 126.5 against 120.7 / 124.8). 200 us adds nothing. Spin 50 us is now the default.

Conclusion: the critical path is guest code running in the JIT on the main thread and the workers. Reaching 200 fps needs the JIT itself to be about 1.6x faster (steps 2, 1, 3).

## Step 2 result (NZCV in host flags)

The x64 emitter tracks when the host flags still hold the NZCV just stored to cpsr_nzcv (after an add/sub/test, with only moves after it). Then side exits and CSEL use the flags directly, with no reload and no sahf. TEMP switch NXEMU_JIT_NO_FLAGS. TOTK: 126.0 -> 126.7 (2 pairs), trees fine. Kept, but it is not a lever.

## Profile at about 127 fps (spin 50, jitflags)

- Guest cores: about 69 % JIT, 23 % idle, small kernel. SharedCpuClock Begin/End/Now is about 2 %.
- GPU thread: about 91 % busy. It is now almost a limit as well, so JIT gains alone will soon stop paying.
- `DrawTimers::Enabled` (TEMP) costs about 1.8 % of the GPU thread (function-local static guard); it goes away with the TEMP code.

## GPU thread at about 127 fps

- Recipe recording (`record_shader_recipes`, on by default) costs about 6-7 % of the GPU thread. Turning it off with NXEMU_RECIPE_RECORD_DISABLE=1 gives +3 % (126.6 -> 130.3, 2 rounds). The recorder is already well optimized: it only runs when tracked registers change value, with memoized inputs.
- The rest of the profile is flat: uniform buffers about 12 % inclusive, storage buffers 13 %, vertex/index 9 %, buffer sync 7 %, dynamic state 4 %, render targets 4 %, invalidation 4 %. The per-draw buffer/texture cache locks are about 2.5 %, and they already use a cheap RecursiveMutex.
- No single lever is left on the GPU thread. Either many 1-3 % changes, or a structural split of draw preparation.

## The frame is now GPU-thread bound (2026-11-06)

- GPU fixes `gfix1`: LRU touch skipped through a tick kept in the Buffer, and DrawTimers::Enabled read from a plain global. 124.2 -> 124.5, neutral. Kept.
- FP test (TEMP `NXEMU_JIT_UNSAFE_FP=1`: ReducedErrorFP + InaccurateNaN): guest JIT time drops from 72-73 % to 69-71 %, guest idle rises, fps unchanged (127.2 -> 125.0). **Faster JIT no longer gives fps.** The GPU thread (about 91 % busy, the rest waiting on the decode queue) sets the frame rate at about 125-130 fps.
- Consequence: JIT steps 1 and 3 will not move fps until the GPU thread gets faster. The next lever is structural: move work off the GPU thread (to the Vulkan worker or more threads), or turn off recipe recording (+3 %).

## JIT and GPU both set the frame rate (2026-11-06, host-mapped UBO build)

At about 138 fps, each side was slowed with an existing switch (2 rounds each):
- JIT slower (`NXEMU_JIT_NO_MOVEAWAY=1`): 132.9 / 131.4 against base 139.3 / 136.6, **-4.3 %**;
- GPU thread slower (`NXEMU_SPLIT_SKIP_SAME=0`): 135.2 / 133.6, **-2.6 %**.

Both sit on the critical path, and the JIT more so. `NXEMU_JIT_UNSAFE_FP` is neutral now (it no longer changes JIT time), and pinning each heavy thread to its own physical core is -1.5 %.

Hot path of the 57 hottest blocks at this point: 5.78 host instructions per guest instruction (8.5 at the start). The rest of the waste is mostly JitState traffic (stores 11 %, loads 5 %), which needs cross-block register allocation.

## Step 5: plain ordered reads, single-lane FP reads (2026-11-06)

- **Ordered and exclusive reads up to 64 bits are plain loads** (`EmitReadMemoryMov`). They used `lock xadd [mem], 0`, about 20 cycles plus a store buffer drain, in every guest lock. Ordered writes use `xchg` (a full barrier), so a release store followed by an acquire load stays in order, as in the C++ x64 mapping. 128-bit keeps `cmpxchg16b`. TEMP `NXEMU_JIT_LOCKED_READS=1` restores the old code.
- **Scalar FP register reads.** The translator reads S registers as `GetQ` + `VectorGetElement32 #0`, which became `movaps` + `insertps 0xe`. After get/set elimination, a `GetQ` whose only use is lane 0 of 32 bits becomes `GetS` (`movd`, clears the upper lanes), and the lane extract then emits nothing. `ZeroExtendLongToQuad` and `VectorZeroUpper` emit nothing when their input already has clear upper lanes (`UpperLanesClear`). TEMP `NXEMU_JIT_NO_SCALARFP=1`.
- Result (TOTK, mean from 75 s, 2 rounds): old 135.4 / 133.4, FP only 137.0 / 138.1, both 139.8 / 135.7. **About +2.5 %**, from the FP part; the lock part is within noise. Trees fine at 125 s. Kit DLL `jitlock`.

## Step 6: inline indirect dispatch, SetW zero-extension tracking (2026-11-06)

- **FastDispatchHint and PopRSBHint terminals** emit the lookup inline in each block, with its own indirect jump, instead of jumping to one shared handler whose single `jmp` the host predicted badly (the shared handlers were 3.7 % of JIT samples). A miss jumps to the shared miss path with the same registers (`terminal_handler_rsb_miss`, `terminal_handler_fast_dispatch_miss`). TEMP `NXEMU_JIT_SHARED_DISPATCH=1`.
- **SetW** stores the register as is when its value comes from an op whose emitter always writes a 32-bit GPR (`IsZeroExtended32`: GetW, 32-bit reads, zero/sign extends to word, ConditionalSelect32, Add32, Sub32, And32, Or32). It saves the scratch copy and the `mov r32, r32`. TEMP `NXEMU_JIT_NO_ZEXT=1`.
- Result (2 rounds): step 5 138.4 / 139.1, dispatch only 139.4 / 140.6, both 139.1 / 140.7. **About +0.9 %**, from the dispatch part. Trees fine. Kit DLL `jitdisp`.

## Step 7: more zero extensions removed, IsZero fused with side exits

Result: TOTK, 3 rounds, mean from 75 s: 134.8 without (`NXEMU_JIT_NO_ZEXT` + `NXEMU_JIT_NO_ZFUSE`),
137.1 with, about +1.7 % (the "without" side also turns off the step 6 SetW change). Trees fine. Kit: `jitz`.

- `GprZeroExtended32` (emit_x64.h) lists the producers that write a 32-bit register. SetW and
  `ZeroExtendWordToLong` skip their `mov r32, r32` for them. A SetW of an immediate above 0x7fffffff
  goes through `mov r32, imm` instead of a 64-bit move plus a zero extension.
- The exclusive write callbacks now zero extend their u32 status, since the list relies on it.
- `IsZero32/64` whose only use is the next instruction, a side exit, emits only `test`; the side
  exit jumps on ZF. Before: `test, sete, movzx, test, jcc`.
- Bug found and fixed: a value defined in place of its argument (no code emitted) shares the
  argument's spill slot. A slot written at 32 or 64 bits and then read at 64 or 128 bits gives
  garbage in the upper part. The guest jumped to address 0 at boot. The in-place paths now require
  the argument in a register: `ZeroExtendWordToLong` (GPR), and `ZeroExtendLongToQuad` and
  `VectorZeroUpper` (XMM). The last two came from step 5, so step 5 had the same latent bug.

Hot path after step 6 (57 hottest blocks): 5.45 host instructions per guest instruction. What is
left: JitState stores 11.5 % (half of them overwritten later in the same block, kept for the side
exits), final stores 6 %, first loads 3.5 %, spills 3.3 %, `insertps` zero upper before SetQ 2.8 %
(results of scalar FP ops, the upper lanes are not known to be clear), sahf/lahf 2.4 %.

## Track 2: where the JIT time goes now, and profile-guided traces (2026-11-06)

Self-loops are not the target. In the 124 s profile they are only 5.9 % of JIT samples, so keeping
their registers in host registers across iterations would give under 1 %.

Side exit counts (TEMP `NXEMU_JIT_EXIT_STATS=1` with `NXEMU_PERF_MAP_DIR`, `exits-<pid>.txt`), TOTK
115 s, 20.9 G block entries:
- 12.0 guest instructions run per block entry;
- 45 % of entries are blocks of fewer than 8 instructions (ended by calls, returns, back edges);
- 19.8 % of entries leave through a side exit, and 15.4 % through an exit taken more than half the
  time. There the trace followed the wrong side: mostly forward branches that are almost always
  taken (for example 0x8460b200 → 0x8460b340 → 0x8460b460 → 0x8460b570 → 0x8460b5bc, five blocks in
  a row, each leaving through its side exit 92-100 % of the time).

Tiering, the ARMeilleure idea on top of dynarmic (`NXEMU_JIT_NO_TIER=1` turns it off,
`NXEMU_JIT_TIER_N` sets the threshold, default 1000):
- Tier 0 is today's block. A block with side exits starts with `dec [countdown]; jz tier_up` and
  each exit stub counts its takes.
- When the countdown reaches 0, the stub stores the PC and the block's record in `tier_request`
  and goes back to the dispatcher.
- `GetBlock` sees the request, drops the block and compiles it again (tier 1, no counters). The
  translator follows the taken path at each exit taken more than half the time
  (`TranslationOptions::follow_taken`): the inverted condition exits to the fall-through, and the
  trace goes on at the target, up to 256 instructions ahead.
- The old entry gets a `jmp` to the new code, so stale fast-dispatch, RSB and inline-dispatch
  pointers still reach tier 1.

Tiering result (TOTK, mean from 75 s, quiet machine unless noted):
- First version: 130.2 and 122.8 against 139.9 and 139.8 without tiering.
- About 100 k tier-ups per run, still about 950/s in steady state, almost all on different PCs:
  new blocks keep crossing the threshold as the player moves.
- Each compile froze the shared guest clock (`OnCompilation`). Tier-up compiles no longer notify
  (`NXEMU_JIT_TIER_NOTIFY=1` gives the old behavior). Still a loss: 128.7 (threshold 1000) and
  127.7 (threshold 10000) against 139.0.
- Tier 1 without following the taken paths (`NXEMU_JIT_TIER_NOFOLLOW=1`) loses as much, so the new
  trace layout is not the cause.
- Counters only, threshold never reached: 113.9 / 131.1 against 128.8 / 131.9 (noisy machine).
  Not the main cost.

**Not kept.** Tiering is off by default (`NXEMU_JIT_TIER=1` turns it on). The cost seems to come
from the tier-up itself: recompiling on the core thread, old code reached through an extra jump,
tier-1 code far from its neighbors. A tier 1 is only worth it if it brings more than a better
trace direction (a multi-block region with cross-block registers), compiled on a background
thread.

## Pinned guest registers (2026-11-06)

A cheaper first step towards cross-block allocation: a few guest GPRs live in a host register in
**all** A64 code, so block boundaries no longer store and reload them.

- Hosts: RSI, RDI, R12. All three are callee-saved on Win64, so host calls keep them, and no
  emitter names them. The fast-dispatch code used r12 for the table address; it now uses rcx,
  which is free once the location descriptor is computed. RBX and RBP stay with the dispatch
  code (they must survive the `LookupBlock` call).
- The guest registers come from TEMP `NXEMU_JIT_PIN` (for example `sp,19,20`; unset = none).
  They leave `gpr_order`.
- `GetW/GetX/GetSP` copy from the host register; `SetW/SetX/SetSP` write it (a W write clears the
  upper half). Side exit stores write it too.
- JitState holds them only outside run code: `RunCode`/`StepCode` load them (`GenRCP`), and the
  return to the caller stores them (`GenRCPExit`, a new `BlockOfCode` hook). `ExceptionRaised`
  stores them before its callback, because `ReturnException` reads the context. The other
  callbacks (SVC, cache ops, ISB) only halt; the context is read after `Run` returns.
- Only on Win64; elsewhere the list is empty.

## What a function region could cover (2026-11-06)

Exit stats with the terminal kind (TEMP `NXEMU_JIT_EXIT_STATS`, `regions.py`), TOTK, 19.7 G block
exits:

| Block exit | Share |
|---|---|
| Terminal with a backward target (loop back edge) | 46.9 % |
| Side exit (forward branch) | 19.6 % |
| Forward branch or fall-through link | 11.8 % |
| Return | 8.5 % |
| Indirect (BR/BLR) | 7.3 % |
| Call (BL) | 5.9 % |

Up to 78 % of block-to-block transitions stay inside a function (upper bound: an `If` terminal
counts as a loop when one of its targets is backward). Single-block loops are only 5.9 % of JIT
time, but loops made of several blocks are the most common transition. This is the strongest
case for multi-block regions.

Guest register accesses left after get/set elimination: 10.3 per block entry. X8, X9, X10 and X11
take 31 % of them (X8 11.1 %, X9 9.1 %, X10 6.1 %, X11 4.9 %), then X0, X12, X13, X19, X20.

### Pinned guest registers: result (not kept)

TEMP `NXEMU_JIT_PIN=8,9,10` keeps X8, X9 and X10 in RSI, RDI and R12 for the whole run: loaded in
the run-code prologue, stored back in the exit path, before exceptions and in side-exit stubs.

TOTK, 3 rounds each, mean from 75 s:

| Config | Rounds | Mean |
|---|---|---|
| No pin | 138.5 / 140.8 / 139.9 | 139.7 |
| X8, X9, X10 pinned | 139.4 / 139.8 / 139.1 | 139.4 |

Neutral, and the picture is correct (no miscompile). Guest register loads and stores hit L1 and
are cheap next to block dispatch and link jumps, and removing three hosts from the allocator costs
spills in large blocks. Pinning alone is not worth it. Keeping registers in host registers only
pays inside a multi-block region, where the region's own allocator can see across blocks.

## Where the time goes inside block bodies (2026-11-06)

TEMP `NXEMU_JIT_IRMAP` writes the host offset of every IR instruction of every block
(`irmap-<pid>.txt`). Samples from stackprof2 (TOTK, 15 s from 90 s, 5635 samples in JIT code on
the three guest cores) are mapped to the IR instruction that owns the host address.

The sampler suspends the thread, so a slow instruction is charged to the next one: 98-100 % of
the SetX, SetW and ZeroExtend samples sit on the first host byte of the op, right after a
ReadMemory. Samples on an op's first byte are given back to the previous op that emitted code.

| Group (after skid correction) | Share of JIT samples |
|---|---|
| Integer ALU and other | 35.9 % |
| Guest memory (reads, writes, exclusives) | 33.0 % |
| FP and vector | 16.4 % |
| Block entry and exit (terminal, cycles, side exits) | 7.5 % |
| Cold path (side-exit stubs, fastmem fallbacks) | 4.3 % |
| Flags and select | 2.8 % |

Top ops: ReadMemory64 12.4 %, GetX 8.7 %, terminal 7.0 %, ReadMemory32 6.3 %, FPCompare32 5.2 %,
cold 4.3 %, ReadMemory8 3.9 %, Add64 3.3 %.

What this says:
- The largest cost is the latency of guest loads (data cache misses in the game's working set).
  Much of GetX and Add64 is also waiting for a value that a load produced. The JIT cannot remove
  these loads; only the guest code decides them.
- Block entry and exit is 7.5 %, and JitState get/set traffic that a region could remove is a few
  percent. With the pinning result, a multi-block region JIT would gain little on TOTK.
- FPCompare32 is 5.2 % with only 6 % of its samples on its first byte, so it is its own code:
  `rcl cl, 5` is microcoded and slow. Replaced with `adc cl, cl; shl cl, 4` (same ZF:CF x 16 index).
  TEMP `NXEMU_JIT_OLD_FPCMP=1` restores the old code.

FPCompare A/B, TOTK, mean from 75 s, 6 rounds each (the machine was noisy, several runs drop to
90-115 fps for a while):

| Config | Rounds | Best clean round |
|---|---|---|
| Old (`rcl`) | 111.6 / 136.9 / 137.9 / 138.1 / 135.1 / 118.8 | 138.1 |
| New (`adc` + `shl`) | 120.0 / 121.8 / 140.1 / 133.4 / 129.1 / 140.2 | 140.2 |

The gain is below the noise (about +1.5 % on the best clean rounds). The picture is correct. Kept:
the new code is strictly fewer micro-ops for the same result.
