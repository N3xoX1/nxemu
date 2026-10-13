# Legacy timing overhead

The normal build compiles the old stage-timing hooks to empty scopes. This is
the behavior of the `Sans-Chronometres` comparison build validated in Mario and
Immortals. F9 performance capture from PR 295 uses separate instrumentation.

For an explicitly instrumented diagnostic build, define
`NXEMU_ENABLE_LEGACY_PERFORMANCE_DIAGNOSTICS=1`; the existing
`NXEMU_PERFORMANCE_DIAGNOSTICS=1` runtime variable then enables the stage timers.
Ordinary builds ignore that variable in the video timing hooks.

Run `run-legacy-diagnostics.ps1` to exercise both build modes with the actual
header. It verifies value, void and reference returns, exactly-once callback
execution, exception propagation, nested-scope depth and thread-local counters.
Only logging and formatting are stubbed. This is a correctness fixture, not a
game FPS benchmark.

Manual validation reported about 211 FPS in Mario versus the earlier master
reference of 214 FPS, and about four additional FPS in Immortals without visual
glitches. These are user observations, not new F9 capture averages.
