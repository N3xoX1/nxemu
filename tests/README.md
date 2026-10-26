# Tests

Tests are opt-in: they are only built when the build is configured with
`-DNXEMU_BUILD_TESTS=ON`.

```sh
cmake -S . -B build -DNXEMU_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build -L unit --output-on-failure
ctest --test-dir build -L platform --output-on-failure
```

## unit

Small programs that check one piece of production code and exit non-zero on failure.
They run in the macOS CI job.

| Test | Checks |
|---|---|
| `test_session_lifecycle` | Host thread state across kernel sessions, cancellation of Host1x fence waits (ASan, UBSan) |
| `test_spirv_constant_buffers` | SPIR-V declarations of constant buffers |
| `test_loader_metadata` | Application control metadata (NACP) parsing |

## platform

Native code execution (NCE) on macOS arm64. These tests run guest code on the host CPU,
so they need a real Apple Silicon Mac rather than a CI virtual machine. They are built
on macOS arm64 only (`NXEMU_BUILD_PLATFORM_TESTS`). The executables declare SDK 12.3, as
NxEmu does, so that XNU keeps x18.

| Test | Checks |
|---|---|
| `nce_macos_validate` | Memory, ARM64 rewriting and transitions with the production patcher; see `platform/nce_macos/README.md` |
| `test_direct_map` | NceDirectMap page model and bounds |
| `test_nce_settings`, `test_nce_settings_current_sdk` | Backend selection by address space width; refusal when the host does not keep x18 |
| `nce_runtime`, `nce_runtime_interrupts` | SVC, TLS and migration of guest contexts between host threads, with the real CPU module |
| `nce_runtime_modules` | NRO loading and unloading, native atomics |
| `nce_native_mapping` | Guest memory mapped through Mach memory entries |
