# Tests

Tests are opt-in: they are only built when the build is configured with
`-DNXEMU_BUILD_TESTS=ON`.

```sh
cmake -S . -B build -DNXEMU_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build -L unit --output-on-failure
```

## Fast unit tests

The macOS fast test job uses a standalone CMake project. It only builds test
executables and does not require the emulator or its submodules.

```sh
cmake -S tests/fast -B build/fast-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/fast-tests --target nxemu_fast_tests
ctest --test-dir build/fast-tests -L '^fast$' --output-on-failure
```

Add a self-contained suite in a directory under `tests/fast/` and register its
targets with `nxemu_fast_test(target)`. The normal local test build includes
these tests by default.

## unit

Small programs that check one piece of production code and exit non-zero on failure.
The macOS build job runs the remaining unit tests against the same build tree
used for the emulator.
For a new suite, put a `CMakeLists.txt` in its own directory directly under
`tests/unit/`, define its executable, and register it with
`nxemu_unit_test(target)`. Use `nxemu_unit_test(target LINUX)` to also include
it in the Linux CI subset. For suites that register CTest cases directly,
add their executable targets as dependencies of `nxemu_unit_tests`.

| Test | Checks |
|---|---|
| `test_conditional_engine` / `test_conditional_runtime` | Register/override/address transitions, deferred draw order, predicate lifetime and polarity, with fake GPU dependencies (ASan, UBSan); Python 3 extracts unchanged production methods |
| `test_session_lifecycle` | Host thread state across kernel sessions, cancellation of Host1x fence waits (ASan, UBSan) |
| `test_spirv_constant_buffers` | SPIR-V declarations of constant buffers |
| `test_shader_constant_folding` | Integer bitfields and AND/OR/XOR with Zero/Sign flags, algebraic identities, SSA aliases, runtime operands and unsupported flags (ASan, UBSan) |
| `test_loader_metadata` | Application control metadata (NACP) parsing |
| `test_patch_sets` | Patch sets (`.nxpatch`): in-place and cave sites, relocation of every PC-relative form a cave word may take, whole-set rejection when a word cannot run from a cave, set order and switches |
| `test_loose_romfs_paths` | Decrypted ExeFS identification, case-insensitive folder layouts and skipping loose RomFS payloads during game-list scans |
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
