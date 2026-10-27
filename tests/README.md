# Tests

Tests are opt-in: they are only built when the build is configured with
`-DNXEMU_BUILD_TESTS=ON`.

```sh
cmake -S . -B build -DNXEMU_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build -L unit --output-on-failure
```

## unit

Small programs that check one piece of production code and exit non-zero on failure.
They run in the macOS CI job.

| Test | Checks |
|---|---|
| `test_session_lifecycle` | Host thread state across kernel sessions, cancellation of Host1x fence waits (ASan, UBSan) |
| `test_spirv_constant_buffers` | SPIR-V declarations of constant buffers |
| `test_loader_metadata` | Application control metadata (NACP) parsing |
| `test_settings_runtime` | Concurrent runtime status, coherent shader progress, reentrant callbacks |
| `test_callback_drain` | In-flight subscriber lifetime, concurrent removal, exception cleanup |
| `test_architecture_storage` | Offline controller edits and profile binary format |
| `test_launch_gate` | 720 scenarios against the production EmuThread with controlled dependencies |
| `test_module_lifecycle` | Windows: real plugin setup, guest teardown, host persistence, partial setup recovery |
| `test_loader_session_isolation` | Windows: reused process ID resolves to the current title/RomFS and releases old factories |

The test targets keep assertions enabled in Release builds.

On Windows, `-DNXEMU_TEST_VULKAN_DISPLAY=ON` also enables
`test_vulkan_display_lifecycle` (labels `gpu;integration`). It uses a hidden native
window and a real Vulkan device, with isolated temporary user data. It creates and
cancels guest GPU sessions without loading a game, submits interface images and
checks device reuse. It requires a working Vulkan driver and does not measure game FPS.

The SettingsStore A/B benchmark and the review results are documented in
[the architecture review](../docs/architecture-review-2026-10-27.md).

