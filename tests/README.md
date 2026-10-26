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

