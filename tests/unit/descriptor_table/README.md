# Descriptor-table regression and benchmark

`test_descriptor_table` verifies snapshot lifetime, guest-memory mutation between
passes, table relocation, invalidation, and dense-table fallback. Assertions remain
enabled in Release.

It also checks fragmented mappings, an unaligned first page, span-read fallback,
canonical null entries with nonzero auxiliary words, and empty-block classification
after scalar writes. Each snapshot must remain unchanged if guest memory changes
after capture. Differential trials cover contiguous, fragmented and copied memory.

`test_descriptor_writer` checks retained Vulkan descriptor contents against a fresh
reference, including sampler/layout changes, null runs, partial-block writes and
shrink/regrow. Comparisons use Vulkan fields, never structure padding.

It also exercises compact recipes with multiple array defaults, sparse and dense
overrides, restoring null entries, out-of-order writes, and a lagging worker.
Queued immutable recipes must retain their own handles when producer scratch is
overwritten. The materializer is worker-owned and only receives host values.

Build the optional `benchmark_descriptor_table` target and run it directly. Its CSV
compares the frozen scalar lookup algorithm from master against the production
DescriptorTable, using tables of 128, 4096 and 524288 entries. Stride 0 is empty,
1024 populates one entry in 1024, and 1 is dense. Every pass changes a descriptor;
checksums must match. Five trials alternate execution order and report medians.
The span-mode CSV column is 0 for copied reads, 1 for a contiguous span and 2 for
fragmented page mappings.

`benchmark_descriptor_writer` compares clear/fill/compare with the retained writer.
Both implementations must agree on final descriptors and content-change counts.
It includes long null runs, sparse valid entries and mutations between draws.
The compact mode reports producer time separately from worker expansion time;
worker time is subtracted from the measured total, never counted as eliminated
work. This is an isolated descriptor stage, not a whole-lock or game-FPS benchmark.
Each image table is followed by 4096 runtime sampler entries, mostly identical,
matching the additional sampler-array work observed in Nikoderiko.

`benchmark_descriptor_compare` isolates the comparison step: memcmp versus the
production CPU-dispatched equality routine, over a 16 MiB table split into 4 KiB
blocks. Seven trials alternate order and report medians. Aligned and unaligned
inputs cover unchanged blocks and sparse/dense mismatches at the final byte;
checksums must match. This measures comparison time, not frame rate.

From the repository root, using a configured build directory:

```powershell
cmake --build build/windows-x64 --config Release --target test_descriptor_table test_descriptor_writer benchmark_descriptor_table benchmark_descriptor_writer --parallel 2
ctest --test-dir build/windows-x64 -C Release -R '^test_descriptor_(table|writer)$' --output-on-failure
```

The stub memory manager uses an out-of-line read function and ordinary host RAM.
It does not model GPU address translation, Vulkan driver work, or the image cache.
These are lookup microbenchmarks, not game-FPS predictions. Dense lookup should be
evaluated separately from the sparse-table workload found in Nikoderiko.
