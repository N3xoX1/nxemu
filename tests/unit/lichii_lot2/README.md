# Lichii Lot 2 review

Base: nxemu upstream `cf6db084`, reviewed on 2026-11-07. This branch starts from
master independently of Lot 1.

## Sources and retained changes

| Optimization | Lichii source | nxemu adaptation |
| --- | --- | --- |
| Alpha-test pipeline keys | [01fce03d](https://github.com/zortos293/Lichii/commit/01fce03d24380ae647c07f02dd063981baae041a) | Canonicalize unused references in live keys, guest captures, imported recipes and export matching. |
| Shader graph traversal and repeated address tracking | [0380b062](https://github.com/zortos293/Lichii/commit/0380b0628d4be15fb3eec4e32585c39e507c3851) | Keep linear lookup for small searches, switch to a hash set above 64 nodes, and cache complete tracking results per root/bias during collection. |
| Maxwell control-flow lookup | [f8f29b57](https://github.com/zortos293/Lichii/commit/f8f29b57694c6c5d3d73a16733665604476aa356) | Use the predecessor of an ordered-set upper bound, provide the successor as the split-insertion hint, and determine sibling order with two forward walks. |
| TIC/TSC reads and logging | [a667551c](https://github.com/zortos293/Lichii/commit/a667551cdf6e9044e43e1d0c37320c3ec82a1213) | Use one pointer lookup and memcpy within a conservative 4 KiB boundary; retain the generic reader otherwise. Move per-draw invalid-index logs to trace. |

Diagnostic logging, pipeline-worker scheduling and other unrelated changes in these
commits are not included.

## Review findings

**Alpha references and profile compatibility.** Always/Never do not use the
reference, including their GL and D3D encodings. All comparing operations keep its
original bits. Normalize after reading the register, before hashing/capture.
`FromWords` validates the original serialized fields before normalization, so older
recipes remain readable and narrowing errors are still rejected.

The supplied patch did not update offline export matching: old cache keys retained
nonzero references while their projected journals became canonical. It also emitted
the original guest words after parsing them. Normalize the source keys and emit the
parsed canonical words. The legacy-export test proves that old records still find
their environments, inactive-reference variants merge, and an active comparison
continues to distinguish references.

**Traversal correctness.** FIFO scheduling, rightmost-first argument order,
identity resolution and early predicate matches are preserved on DAGs. Marking the
root visited also prevents a Phi cycle from visiting it twice. The hash table is
allocated only for searches above the threshold. Storage tracking retains nxemu's
existing conservative rules for loaded pointers, ambiguous merges and cyclic SSA.

The supplied patch omitted Lichii's cache for repeated address roots. The adapted
cache lives only for one collection pass, before IR replacement starts. Biased and
unbiased results are separate; failed results retain the indirect-pointer flag so
fallback diagnostics and classification stay consistent.

**Ordered CFG lookup.** Blocks are ordered by begin address and their half-open
ranges do not overlap. Only the predecessor can contain a requested PC. Empty
sets, gaps, exact starts/ends and successful splits are tested. Using the actual
successor as the insertion hint avoids another lookup after splitting. Sibling
ordering is compared with positional order for every pair in lists up to 257 nodes,
including identical nodes.

**Descriptor safety.** A 4 KiB boundary is conservative for the production memory
manager's device pages. Pointer lookup must succeed before copying. Cross-page,
sparse and unmapped reads continue through `ReadBlockUnsafe`; neither path flushes
the renderer. Real GPU/device memory-manager tests compare against the original
reader on misaligned descriptors, discontiguous GPU mappings, fragmented device
mappings beneath big GPU pages, partially unmapped reads, sparse pages and changed
descriptors. The fast-suite stub separately verifies direct/fallback call counts.

**Test infrastructure.** The loader metadata fixture needs `loaderSettings`, as in
Lot 1. Multi-chunk tracking tests also require matching ASan container annotations
in their fixture and the production IR allocation methods; those IR sources are
compiled with sanitizers for that test instead of disabling container checks.

## Validation

- Release compilation of `shader_recompiler` and `video_core`, macOS arm64.
- Full unit suite: **67/67 passed**, including 19 shader-profile cases.
- Standalone fast suite: **3/3 passed**, including descriptor direct/fallback checks.
- New graph/control-flow/real-memory descriptor tests run under ASan/UBSan on
  non-MSVC compilers. Existing tracking regressions cover repeated roots and retain
  their loaded-pointer and Phi-loop safety cases.
- Alpha tests cover all 16 GL/D3D operations; inactive host keys are compared across
  all 128 dynamic-feature combinations.

## Local CPU measurements

Synthetic microbenchmarks, Release Clang on macOS arm64. Times are medians in
microseconds; fixture construction is outside the timed tracking pass. These are
CPU algorithm measurements, not game FPS or end-to-end shader compilation results.

The BFS reference uses the original linear visited lookup, small-vector capacity
and visit order. The checked-in benchmark can be run with:

```sh
cmake --build build --target bench_lichii_graph_search
build/tests/unit/lichii_lot2/bench_lichii_graph_search --benchmark
```

| DAG nodes | Linear lookup | Adaptive lookup | Ratio |
| ---: | ---: | ---: | ---: |
| 1 | 0.099 | 0.097 | 1.02x |
| 8 | 0.186 | 0.208 | 0.89x |
| 64 | 1.267 | 1.266 | 1.00x |
| 512 | 43.343 | 20.745 | 2.09x |
| 4096 | 2291.958 | 168.444 | 13.61x |
| 20000 | 57147.750 | 1013.861 | 56.37x |

A separate local harness linked the unmodified tracking pass from `cf6db084`
under renamed entry points against the same Release IR library. It compared full
passes over a 512-node address chain, with logging filtered, and verified that both
versions converted the same number of accesses and selected one descriptor. Median of seven runs:

| Accesses sharing one root | Baseline pass | Updated pass | Ratio |
| ---: | ---: | ---: | ---: |
| 1 | 39.708 | 41.500 | 0.96x |
| 32 | 1438.750 | 41.125 | 34.98x |
| 256 | 11960.250 | 58.459 | 204.59x |

Small searches and one-off tracking can be slightly slower: these optimizations
trade small bookkeeping costs for avoiding large repeated/quadratic work. The
large ratios deliberately stress that work and must not be extrapolated to FPS.
No descriptor/CFG timing, real-game session, Windows/Linux execution or GPU-driver
rendering validation is claimed.
