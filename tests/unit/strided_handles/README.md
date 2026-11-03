# Strided handles and cross-format Vulkan copies

`test_strided_handles` executes the production texture pass and independently evaluates
the descriptor address against U32 guest arithmetic. It covers 4/8/16/192/304-byte
strides, base zero and nonzero, masked and unbounded indices, FindMSB's -1 result,
wrapped products, shifted offsets, and complete-word buffer bounds.

Optional arguments:

* `--benchmark`: time only TexturePass, with 100 warmup and 10,000 measured passes
  per bounded/unbounded 304-byte case. These are synthetic shader translation costs.
* `--emit <directory>`: emit 32 SPIR-V and 32 GLSL fragment shaders through the
  production backends. Validate SPIR-V with `spirv-val --target-env vulkan1.1` and
  GLSL with `glslangValidator -S frag`. Unbounded fallback cases intentionally use
  a synthetic descriptor budget of 32768; syntax validation needs a matching GLSL
  resource configuration. This does not establish real driver resource capacity.

`test_strided_copy_gpu` is manual and requires `NXEMU_BUILD_COPY_GPU_TESTS=ON`.
It uses the real Vulkan runtime, requires the Khronos validation layer, and compares
all destination bytes, including untouched guard regions. It checks 168 cases with
R8/R16/RGBA8/RGBA16 formats, optimal/guest-linear images, two mips, array layers,
offsets, multiple regions, clipping, malformed and empty regions, and synthetic
rescaling metadata. The rescaling case models a host allocation larger than its
native metadata; it does not exercise the scaling filter. It also checks null and
non-null buffer descriptor ranges. Vulkan images use the production host allocation
policy; guest `ImageType::Linear` does not imply `VK_IMAGE_TILING_LINEAR`.

Example from the repository root on Windows:

```powershell
cmake -S . -B build/windows-x64 -DNXEMU_BUILD_TESTS=ON -DNXEMU_BUILD_COPY_GPU_TESTS=ON
cmake --build build/windows-x64 --config Release --target test_strided_handles test_strided_copy_gpu
build/windows-x64/tests/unit/Release/test_strided_handles.exe
$env:VK_LAYER_PATH = "$env:VULKAN_SDK/Bin"
$env:VK_LAYER_VALIDATE_SYNC = '1'
build/windows-x64/tests/unit/Release/test_strided_copy_gpu.exe C:/Windows/System32/vulkan-1.dll
```

The GPU test also executes 192 chained 4/2/8-byte copies reusing the same scratch
buffer, plus 17-region R32F/D32F copies in both directions, including untouched bytes.

Adding `--benchmark` after the Vulkan library path measures CPU recording time,
completed batch wall time and GPU timestamp intervals. It reports the median of
five batches after warmup for 2 KiB, 2 MiB and 7.9 MiB copies, in both 4-to-2 and
2-to-4 byte directions, with a 4-to-4 byte control. Timestamps include barriers and
any GPU idle time in the measured interval. Use `--benchmark-no-validation` instead
for performance comparisons without the validation layer. Always run the default
validation pass separately. Neither benchmark predicts game FPS.

Compressed formats, combined depth/stencil, multisample copies, 3D volumes, other
drivers and long game sessions require separate coverage. These tests are not
sanitizer runs. R32F/D32F checks cover reinterpretation, not depth rendering.
