# Vulkan vertex input

`test_vertex_input` compiles the unchanged production update, stride notification,
format conversion and pipeline dynamic-state list against real Maxwell register
layouts and Vulkan description types. A recording fixture replaces the scheduler
and driver. Release builds use explicit checks, not disabled `assert` statements.

Coverage: initial attribute 31, changes to attributes 0 and 31, divisor-only and
stride-only updates, zero stride, repeated strides, sparse/shared binding 31,
all-constant layouts, command-buffer invalidation, channel layout changes,
unrelated dirty-bit preservation, host limits, and independent dynamic extensions.
This test does not render or verify a driver's behavior.

```powershell
cmake --build build/windows-x64 --config Release --target test_vertex_input
ctest --test-dir build/windows-x64 -C Release -R '^test_vertex_input$' --output-on-failure
```

The optional comparison target requires the reviewed original patch and a Git ref
from before the fix. The recorded comparison used baseline
`ac58bea5e30706706b3953f4669ba2cef7853322`.

```powershell
cmake -S . -B build/windows-x64 `
  '-DNXEMU_VERTEX_INPUT_REFERENCE_PATCH=C:/Users/fabri/Downloads/NXEmu_Lot1_Vulkan_VertexInput_optimized.patch' `
  '-DNXEMU_VERTEX_INPUT_BASELINE=ac58bea5e30706706b3953f4669ba2cef7853322'
cmake --build build/windows-x64 --config Release --target bench_vertex_input
& build/windows-x64/tests/unit/Release/bench_vertex_input.exe
& build/windows-x64/tests/unit/Release/bench_vertex_input.exe --original
# The original is expected to exit 1: missing attribute 31.
& build/windows-x64/tests/unit/Release/bench_vertex_input.exe --benchmark
```

Use an optimized build with no competing compilation. Each cell reports median,
minimum and maximum from 11 samples, in randomized variant order. Each sample
executes 500,000 updates or 3,000,000 clean/rebase iterations. Layouts use 1, 8 and
32 attributes with distinct bindings. Recording copies the complete descriptions
to the capture; it does not run Vulkan, allocate real scheduler command chunks or
measure frame times. The fixture's device supports requested formats directly.
Rebase scenarios cover the stride hook and update dispatch, not buffer lookup,
upload or `vkCmdBindVertexBuffers`.

`original_invalid` is useful for identifying host overhead, but is not a correct
rendering reference. In particular, the original stride-only case does no update.
Compare the supplied and improved variants for the cost of complete, correct
vertex-input descriptions. FPS, frame-time tails, pipeline compilation on hosts
needing compaction, and visual regressions require a separate game A/B run.
