# Device memory continuity regression tests

The real device memory manager runs against simulated memory/rasterizer services. Random reads and writes are checked against a page-by-page oracle. Additional cases reproduce the heap session sequence: TrackContinuity before mapping, Map(track=false), shuffled chunks, interior holes, partial remapping, null CPU translations, and reverse page-by-page teardown of 64 MiB. Sparse device addresses exercise summary-word boundaries and the final address-space page. Both GetSpan overloads are covered. The fast-copy test additionally verifies that a 1 MiB heap mapping remains eligible for TryFastGuestCopy and becomes ineligible after unmapping an interior page.

Build test_mapping_continuity and test_fast_copy with NXEMU_BUILD_TESTS=ON, then run ctest -C Release -R 'test_(mapping_continuity|fast_copy)$' --output-on-failure. On Linux, sh tests/unit/mapping_continuity/sanitize.sh instruments the actual device-memory implementation with ASan/UBSan.

## Representation and cost bounds

A leaf bit marks a boundary between physically nonadjacent mappings or between mapped and unmapped pages. Adjacent unmapped pages form one run too. Each summary bit indicates a nonempty word in the preceding level. Initially every page is unmapped and all bitmaps are zero.

Map derives boundaries from the physical pages it already installs, independently of the legacy track argument; no extra process-memory traversal is required. It also updates the right edge of the changed range. Unmap does the same when clearing entries. The old pretracking entry point remains for Host1x ABI compatibility, but no longer needs to anticipate mappings that are not installed. Heap address preallocation itself is unchanged.

Updating one boundary touches at most four words for the 34-bit device address space. It never visits predecessor pages. Consequently continuity maintenance for N changed pages is O(N), including reverse teardown and repeated changes in the middle of a large run. This bound concerns continuity bookkeeping; it does not change the existing reverse-alias container's costs.

Queries ascend nonempty-word summaries and descend to the first boundary, stopping at the requested end. There are at most four ascending and three descending levels; ordinary same-page GetSpan calls avoid the search. No run-tree allocations, new per-access locks, or global synchronization are introduced. The bitmap payload is about 520 KiB instead of the previous 16 MiB count array. Concurrent reads and mapping changes still require the existing external serialization.

## Review reproduction and benchmarks

Baseline: PR revision 687def5fb2, with the same benchmark harness compiled against its frozen headers/implementation. Candidate: the revised boundary bookkeeping. The old implementation fails the first large-heap test because GetSpan rejects the full Map(track=false) range.

Run test_mapping_continuity --benchmark for TrackContinuity followed by Map(track=false), or --benchmark-tracked for Map(track=true). CSV columns: pages, repeat, full_span_available, prepare_us, read_us, full_getspan_us, reverse_unmap_us, small_getspan_us. Each read sample is the average of 32 real ReadBlockUnsafe transfers. The span loops perform 200,000 queries with observable results. All pages are physically contiguous, and sizes range from 256 KiB to 64 MiB. Prepare includes the pretracking call and Map. Reverse unmap removes pages individually from the end.

Results and measured tradeoffs are recorded in benchmark-review.csv. These are host microbenchmarks, not game FPS or GPU timings. Restoring a contiguous span is also required by actual fast-copy and descriptor-reading guards. It does not imply every synthetic memcpy shape becomes faster: large copied spans can exercise different host-copy behavior than thousands of page-sized copies.

Windows x64 Release and Linux x64 ASan/UBSan pass the correctness tests. Windows timings use MSVC 14.44 on a Ryzen 7 9800X3D, two ABBA rounds per mode, four process runs per variant, five samples per size/run (20 samples per cell). Detailed interpretation is in REVIEW.md. TOTK gameplay validation remains outstanding.
