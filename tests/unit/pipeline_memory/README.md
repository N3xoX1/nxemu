# Pipeline memory and Windows JIT commitment

Reviewed against upstream/master `26a1c870`.

## Changes retained

- Release ordinary graphics, compute, transform-feedback compute and VTG compute shader modules after the native pipeline creation calls return, on success and handled failure. Select the graphics configuration function before submitting native work, because specialization inspects module presence. The worker remains the only subsequent reader of those modules. NXShader and synchronous builds use the same constructor paths.
- Commit the Windows x64 code cache in 4 MiB batches, including the complete requested range. Commit only the new suffix and publish the committed size only after `VirtualAlloc` succeeds. NXEmu already reserves its cache without committing the full reservation; importing another allocator is unnecessary.
- Move copy-region vectors into the scheduler commands in image copies, uploads (including the MSAA staging path) and downloads. The originating vectors are no longer used afterward, and the scheduler moves the commands into its storage. Inline small-vector elements can still be moved; this does not claim all CPU copies disappear.

## Improvements over the supplied patch

- Check JIT reservation bounds before adding offset and length. Clamp alignment padding with subtraction to avoid overflow near `SIZE_MAX`.
- Handle standard C++ exceptions in compute pipeline construction as graphics pipelines already do. This includes allocation failures while building descriptor metadata, releases the module and reports build completion instead of letting that failure bypass cleanup.

## Regression tests

`test_jit_commit_rw` and `test_jit_commit_rwx` compile the actual Windows `EnsureMemoryCommitted` method extracted from production. A mock `VirtualAlloc` checks call addresses, sizes, protections and failure handling on every host. Cases cover the 2 MiB constant pool plus 16 MiB prelude, large emissions, reuse of committed batches, a partial final batch, exhausted/invalid reservations, allocation failure/retry and arithmetic near `SIZE_MAX`. The unchanged master method fails this fixture. This is not a native Windows allocation test.

Build and run the automatic suite:

```sh
cmake --build build --target video_core nxemu_unit_tests -j8
ctest --test-dir build -L unit --output-on-failure -j8
```

The manual `test_pipeline_module_gpu` target uses the production `ComputePipeline` constructor, scheduler, native build queue and a real Vulkan device. It intercepts module destruction to check early release, blocks a worker to check pending-build retention, and tests synchronous/asynchronous success, injected Vulkan errors and injected C++ allocation failures. Successful pipelines are dispatched after module destruction. The embedded shader is the output of `glslangValidator -V` for:

```glsl
#version 450
layout(local_size_x=1) in;
void main() {}
```

```sh
cmake --build build --target test_pipeline_module_gpu -j8
# Requires the Khronos validation layer and an installed Vulkan driver/loader.
build/tests/unit/pipeline_memory/test_pipeline_module_gpu /absolute/path/to/Vulkan/library
```

## Results and limits

- Release `video_core`, all unit-test targets, and both manual GPU-test targets built on macOS arm64.
- 65/65 unit tests passed, including both commitment fixtures.
- The compute lifetime test passed all six cases under MoltenVK with zero Vulkan validation errors, including dispatches after module destruction.
- Existing `test_fast_copy_gpu` passed 40 GPU cache/readback checks under MoltenVK. That existing test was run without enabling validation and does not cover every moved-vector/MSAA combination.
- Graphics/VTG/transform-feedback module cleanup and configuration ordering were reviewed and compiled; the manual lifetime test executes compute pipelines only.
- No native Windows run, full application build, gameplay regression run, or FPS/memory benchmark was performed. MoltenVK reports existing unavailable emulator features; these focused checks do not establish full driver compatibility.

The larger Dynarmic block-table rewrite, GPU readback synchronization changes and depth/stencil changes remain outside this patch.

## Sources

- [Lichii e9f31461: commit x64 code cache memory as code is emitted](https://github.com/zortos293/Lichii/commit/e9f31461b6ff98643bc2bfe330914d57b5facf38).
- [Vulkan: vkDestroyShaderModule](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyShaderModule.html), including lifetime and host synchronization requirements. Native creation calls finish before destruction; no GPU idle wait is required to release these modules.
- [Microsoft: VirtualAlloc](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc), including reservation, commitment, page rounding and failure semantics.

The supplied patch did not identify separate Lichii commit hashes for shader-module release or moved texture-copy captures; no attribution to an unverified commit is asserted here.
