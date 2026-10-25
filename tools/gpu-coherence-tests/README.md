# GPU buffer coherence regression tests

These standalone tests require CMake, a C++20 compiler, and the repository's
Boost and fmt dependencies. They enable no renderer diagnostics.

```sh
cmake -S tools/gpu-coherence-tests -B build/gpu-coherence-tests
cmake --build build/gpu-coherence-tests --config Release
ctest --test-dir build/gpu-coherence-tests -C Release --output-on-failure
```

`gpu-dirty-ranges` exercises the production CPU invalidation collector across
2-KiB mask windows, full guest pages, unaligned ranges, accumulation, and a
producer running concurrently with collection. It verifies exact 64-byte coverage.

`cpu-write-readback` compiles the production `WriteMemory` method into a fixture
using the real memory tracker, GPU range set and conservative write history.
Two disjoint CPU writes in one GPU-dirty page must both survive an older GPU
download, in either order. Unrelated GPU ranges must not force exact lookups
for a region that has never been GPU-written. The fixture models download
retirement; it does not replace a real Vulkan/game regression run.

Both regressions fail against the uncorrected production methods. The V4 build
combines both fixes with the write-history filter. Manual measurements at the
same test point were 350 FPS for V0 and 349 FPS for V4; this single observation
does not establish a statistically significant performance difference or prove
that every intermittent Odyssey visual artifact is resolved.
