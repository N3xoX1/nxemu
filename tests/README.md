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
The macOS CI job runs the unit suite. Linux also runs `test_shader_constant_folding`.

| Test | Checks |
|---|---|
| `test_session_lifecycle` | Host thread state across kernel sessions, cancellation of Host1x fence waits (ASan, UBSan) |
| `test_spirv_constant_buffers` | SPIR-V declarations of constant buffers |
| `test_shader_constant_folding` | Integer bitfields and AND/OR/XOR with Zero/Sign flags, algebraic identities, SSA aliases, runtime operands and unsupported flags (ASan, UBSan) |
| `test_loader_metadata` | Application control metadata (NACP) parsing |
| `test_shader_profile_*` | NXShader codec, validation, native cache/journal recovery, recorder, content selection, cross-version reuse and progressive warmup |

## Shader profiles

The 17 shader-profile tests use synthetic data and need no game files, GPU, Python
or running emulator. They run in the same CTest suite as the other unit tests:

```sh
cmake --build build --target test_shader_profile
ctest --test-dir build -L shader_profile --output-on-failure
```

The tests cover malformed and truncated profiles, checked resource references,
cache deduplication and recovery, replay order, bounded cross-version candidates,
changed dependency rejection, and the startup count/time policy. They validate
reusable shader inputs; they cannot establish coverage of unvisited game scenes.
