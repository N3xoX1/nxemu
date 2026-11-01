# x64 register allocation regression test

The executable links the production CPU-module objects. It compiles and executes
AArch64 blocks containing dependent GPR/SIMD additions, repeated value references,
CMP/CSEL and SVC. All GPR and SIMD results are checked against a separate calculation.
The corpus exercises register pressure, short/complex blocks, safe IR optimizations
enabled/disabled, warm-cache reuse and range invalidation/recompilation.
It needs an x86-64 host; it does not test the ARM64 backend or NCE.
The Linux x64 workflow builds and runs it alongside the existing focused tests.

```powershell
cmake --preset windows-x64 -DNXEMU_BUILD_TESTS=ON
cmake --build --preset windows-x64-release --target test_dynarmic_regalloc --parallel 12
ctest --test-dir build/windows-x64 -C Release -R '^test_dynarmic_regalloc$' --output-on-failure
cmake --build --preset windows-x64-debug --target test_dynarmic_regalloc --parallel 12
ctest --test-dir build/windows-x64 -C Debug -R '^test_dynarmic_regalloc$' --output-on-failure
build/windows-x64/tests/unit/Release/test_dynarmic_regalloc.exe --benchmark
```

Default runs use 100 blocks per workload. `--benchmark` uses 10,000 distinct short
blocks and 10,000 complex blocks, then 100 blocks with IR optimizations disabled.
Output separates cold total time, time inside the JIT compilation callback and warm
execution time. Every run checks correctness even with `NDEBUG` defined.

For A/B measurements, build the same test and options against both revisions.
Five-run median compilation times on a Ryzen 7 9800X3D, Windows x64, MSVC 19.44
Release with the existing Dynarmic `/GL`/LTCG settings:

| Workload | master `103ee3e351` | Optimized |
|---|---:|---:|
| 10,000 short blocks | 1759.560 ms | 579.619 ms |
| 10,000 complex blocks | 11082.320 ms | 4147.727 ms |

Warm execution remained in the same range. These are synthetic JIT compilation
measurements, not game FPS or an ARM64 performance claim.
