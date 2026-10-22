# Vulkan query lifetime regression tests

These standalone tests are separate from the emulator build. They do not enable renderer instrumentation or change emulator settings.

Prerequisites: CMake, a C++20 compiler, Vulkan 1.2 with host query reset and timeline semaphores, and the Khronos validation layer from the Vulkan SDK.

```powershell
cmake -S tools/vulkan-query-lifetime -B build/query-lifetime-tests -A x64
cmake --build build/query-lifetime-tests --config Release
ctest --test-dir build/query-lifetime-tests -C Release --output-on-failure
```

`query-reset-lifetime` verifies three lifetime boundaries:

- A host reset cannot order a reset between two query uses already recorded in one command buffer. The validation callback rejects that invalid submission before it reaches the driver (expected VUID 00807).
- CPU query results can already be available while a later GPU result copy is still pending. A timeline gate holds that copy; the test leaves the pool intact until its fence completes.
- Resetting and reusing the pool after GPU completion produces no validation errors.

The pending-copy case deliberately does not issue an invalid host reset: some validation implementations do not intercept it, and it can hang the GPU. Expected validation errors from the first negative case do not constitute a test failure; any additional validation error in the positive cases does.

`query-bank-operations` exercises the production traversal helper with partially filled banks, recycling that destroys links when references are released, a concurrent recycler, and an empty query. It reproduces the old traversal failure before checking the fixed helper. Its alternating CPU microbenchmarks compare old and fixed traversal/retirement; these timings are not game FPS measurements.

The independent Win32 acquire-semaphore regression test is retained in `tools/vulkan-acquire-lifetime`. Its presence does not mean the reverted acquire-semaphore candidate is part of the runtime fix:

```powershell
cmake -S tools/vulkan-acquire-lifetime -B build/acquire-lifetime-tests -A x64
cmake --build build/acquire-lifetime-tests --config Release
ctest --test-dir build/acquire-lifetime-tests -C Release --output-on-failure
```

That test rejects reuse of an acquisition semaphore while its consuming submission is pending (expected VUID 01779), then verifies reuse after the consuming fence completes.
