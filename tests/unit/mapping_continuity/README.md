# Device memory continuity regression tests

This test links the real device memory manager against simulated memory and rasterizer services. It checks mapping changes, untracked remapping, interior holes, partial retracking, null CPU translations, empty ranges, unaligned reads, and writes that must preserve backing behind holes. Randomized reads are compared with a page-by-page oracle.

Build test_mapping_continuity with NXEMU_BUILD_TESTS=ON, then run ctest -C Release -R test_mapping_continuity --output-on-failure. On Linux, run sh tests/unit/mapping_continuity/sanitize.sh to instrument the real implementation with ASan/UBSan. An extra --benchmark argument measures contiguous reads and mapping mutations.

Continuity counts include the starting page and must remain at least one. Maintaining C[i] <= C[i+1]+1 lets invalidation stop at the first predecessor run ending before the changed range. Partial retracking must truncate predecessors too. Work is added only to mapping mutations; the transfer walk is unchanged. Concurrent reads and mapping destruction still require the existing external serialization.
