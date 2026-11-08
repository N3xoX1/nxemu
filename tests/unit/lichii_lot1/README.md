# Lichii Lot 1 review

Reviewed against nxemu upstream `cf6db084` on 2026-11-07.

## Source and decisions

| Change | Source | Decision |
| --- | --- | --- |
| Destroy pooled shader objects | [59047f0e](https://github.com/zortos293/Lichii/commit/59047f0e6fd149e8187092ec0d9f419aa03e875f) | Adopt; publish slots only after successful construction and keep the pool valid if consolidation allocation fails. |
| Scalar/vector SPIR-V Phi types | [47ad28fe](https://github.com/zortos293/Lichii/commit/47ad28fe373e4a6131fd3dd6bbbabcd948a07d1b) | Adopt all 20 value types; reject undeclared type IDs with `Sirit::ValidId`. |
| Nested returns in AMD demote reordering | [77cbc3e7](https://github.com/zortos293/Lichii/commit/77cbc3e7c331c2e3c7cefb9d1ddbfa5a368b270b) | Count returns at every depth and after demotes in the same block; require exactly one epilogue. |
| Buffer-cache address bounds | [a407703f](https://github.com/zortos293/Lichii/commit/a407703f6d29a5ab73328b4ededb12cf29b2758d) | Adopt only the bounds fix from this larger commit; also reject crossing/empty ranges and stop null-buffer consumers from accessing memory tracking. |

`d56039ac` is not the source used for buffer bounds. No audio, texture-collection,
ASTC scheduling or descriptor-ring changes from the fork are included here.

## Findings in the supplied patch

- **Compilation:** `Id == Id{}` does not compile with nxemu's Sirit. Its existing
  `Sirit::ValidId` API provides the required check.
- **Pool lifetime:** destroying `Storage` only invokes its empty union destructor.
  Destroying each constructed `T` releases instruction Phi operands, block vectors
  and flow-block allocations. `Create` must increment the slot count after the
  constructor succeeds. Vulkan's `ShaderPools` already releases blocks before
  instructions, including reverse member destruction at shutdown; retain that order.
  Allocate a consolidated chunk before clearing the old chunks so allocation failure
  leaves the pool usable. This temporarily retains old and new chunk storage together.
- **Phi availability:** integer types depend on both shader usage and profile support;
  FP16/FP64 declarations depend on the existing lowering/usage information. The check
  rejects a missing declaration and preserves existing type-lowering policy.
- **Demote safety:** the supplied patch correctly avoids stopping at the first demote
  while counting epilogues. Also reject zero epilogues: otherwise the reverse search
  for a return block has no match and subsequent iterator dereferences are invalid.
  Deep demotes remain in place, and multiple returns disable the transformation.
- **Buffer safety:** nxemu's table covers **2^34 bytes**, rather than the fork's
  2^36 bytes. Derive the limit from the actual table. Checking the first page before
  subtracting the address makes the complete-range check safe from unsigned wrap.
  Returning a null buffer alone is insufficient: synchronization can still index the
  finite memory-tracker table using the rejected address. Skip null-buffer uploads,
  write tracking and `ObtainCPUBuffer` post operations as well.
- **Test infrastructure:** the upstream loader-metadata fixture still supplied
  `g_settings` after production moved to `loaderSettings`; provide the current setting
  object so the complete unit suite links.

## Regression coverage

- `test_shader_object_pool`: owned allocations, repeated release, full root,
  multiple chunks, destructor cleanup, and failed construction in both an existing
  and a newly allocated chunk. Available in the standalone fast suite and Linux CI.
- `test_shader_phi`: the production `EmitPhi` path for all 20 value types, missing
  declarations, unsupported integer profiles, and invalid IR types. It verifies the
  emitted result type and incoming operand/label. Passing an output directory writes
  complete compute modules for external validation.
- `test_shader_demote`: nested early returns, a demote followed by an epilogue in
  the same nested block, a supported single-return reorder, and no-return fallback.
- `test_buffer_cache_bounds`: exact end, crossing ranges, huge addresses/sizes,
  empty ranges, cached hits/misses and null-buffer consumers, including absence of
  memory-tracker/write notifications for rejected ranges. Valid paths still notify.

The private demote pass and buffer methods are extracted unchanged during the build;
fixtures replace unrelated GPU dependencies. The Phi test calls the linked production
emitter. New regression executables use ASan/UBSan on non-MSVC builds.

## Validation and limits

- Release compilation of `shader_recompiler` and `video_core` on macOS arm64.
- Complete unit suite: **67/67 passed**.
- **29** emitted modules accepted by `spirv-val --target-env vulkan1.0`.
- The new pool test is also built/run by the standalone fast suite.

These are correctness and lifetime fixes. No game FPS, long-session RSS plateau,
AMD-driver rendering, Windows/Linux execution or end-to-end game compatibility
measurement is claimed. The invalid-address guard does not diagnose the origin of
corrupt guest buffer addresses. GPU fixtures do not replace a real renderer run.
