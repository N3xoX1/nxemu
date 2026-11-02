# PR #542 review follow-up

Both findings are valid at revision 687def5fb2. Resetting every mapped page to length one destroys the heap's pretracked spans when HeapMapper requests Map(track=false). Truncating every predecessor is quadratic for reverse page-by-page teardown of a tracked run. The new large-heap test fails on that revision before the implementation changes.

The replacement stores physical-run boundaries rather than lengths at every predecessor. Map reconstructs real continuity from the physical addresses it already obtains, including when track=false, and adjacent mappings merge automatically. No uninstalled page is treated as mapped. Continuity updates are bounded by four bitmap levels and do not allocate or rescan predecessor pages. Heap virtual-address preallocation remains unchanged; its now-unnecessary metadata pretracking call is retained as an ABI-compatible no-op.

## Windows x64 measurements

MSVC 14.44 Release, Ryzen 7 9800X3D. Two ABBA rounds per mode with no concurrent compilation, four process runs per variant and five repeats per size/run: 20 samples per cell. Both executables use the same benchmark code; the baseline is compiled against frozen 687def5fb2 headers/implementation. Times are medians. The complete 256 KiB–64 MiB matrix is in benchmark-review.csv.

| Tracked region, reverse page-by-page teardown | Before ms | After ms |
|---|---:|---:|
| 1 MiB | 0.013 | 0.004 |
| 32 MiB | 8.758 | 0.132 |
| 64 MiB | 34.687 | 0.265 |

Doubling the tracked region from 32 to 64 MiB approximately quadruples the old teardown time and doubles the new time. The continuity update loops are now bounded independently of run length; this is not merely a favorable timing result.

| Pretrack then Map(track=false) | Before | After |
|---|---|---|
| Full contiguous 1 MiB, 32 MiB and 64 MiB GetSpan | Rejected | Accepted |
| Actual 1 MiB TryFastGuestCopy regression | Ineligible | Eligible; copied bytes verified |
| Same copy with an interior unmapped page | Rejected | Rejected |

## Costs and limits

A bounded boundary search is more work than a trusted single length load. On already tracked mappings, the 8-byte same-page GetSpan median changes from 0.783 ns to 1.108 ns; a full 32 MiB GetSpan changes from 0.798 ns to 4.118 ns. The bitmap payload drops from 16 MiB to about 520 KiB.

Single-span tracked reads remain close in these measurements: 32 MiB 1.180 ms to 1.179 ms, and 64 MiB 2.407 ms to 2.413 ms. Preparing the tracked 64 MiB region changes from 61.000 us to 42.150 us.

There is a copy-only tradeoff: a warm 32 MiB ReadBlockUnsafe after Map(track=false) changes from 0.765 ms to 1.190 ms. The old PR performs thousands of page-sized copies; the corrected mapping uses the same single-span shape as the already tracked case. This result is recorded rather than hidden as a universal speedup. The host-copy policy is not modified to tune one synthetic size. Restoring GetSpan also restores actual fast-copy eligibility, which the regression test checks. No net game FPS improvement is claimed.

Validation: Windows x64 Release application build, targeted command/texture/mapping/conditional-rendering/fast-copy tests, and Linux x64 ASan/UBSan on the actual device-memory implementation. Tests cover 2,000 randomized mapping/read sequences, 64 MiB holes and reverse teardown, shuffled 32 MiB heap chunks, 1,000 middle-page unmap/remap cycles, physical remapping, null translations, sparse summary boundaries and the final device page. Concurrent mapping changes retain the existing external serialization requirement. TOTK gameplay and the revised code's ARM64 CI validation remain pending at the time of these local measurements.
