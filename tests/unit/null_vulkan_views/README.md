# Lot 5: Vulkan fallback image views

Base: upstream master `26a1c870`, 2026-11-07. This branch is independent of the
preceding lots. The supplied patch is the starting point; no upstream commit was
provided for the null-view change itself.

## Retained changes and review improvements

- Separate tiny backing images for 1D, 2D and 3D sampled views. The 2D image has
  six layers and cube compatibility; cube and cube-array views expose all six.
  Non-cube array views expose one valid layer. Buffer descriptors remain separate.
- Clear every backing subresource once, outside render passes, with an
  UNDEFINED-to-GENERAL transition, transfer-write access, and a visibility barrier
  before shader accesses. Shared helpers define the actual image/view descriptions
  and GPU commands used by production and tests.
- Declare backing images before their views, so reverse destruction releases views
  first. A custom move assignment also destroys the old complete resource set before
  moving a new one in; simply reordering members with default move assignment would
  still release old images before their old views.
- Dedicated image creation uses exactly the required dimensions, format and flags.
  It avoids the generic guest-image path suppressing CUBE_COMPATIBLE on some AMD
  drivers: a cube view still requires that flag. These tiny fallback images retain
  it; the existing workaround for ordinary guest images is unchanged. The behavior
  on those affected AMD devices has not been tested.
- The supplied patch's single `image_handle` was not sufficient for lazy storage
  views. Storage descriptors can request different dimensions and integer formats.
  Allocate and zero one additional image/view per requested type/format on first
  use, then reuse it. Publish the cached resource only after initialization commands
  have been recorded successfully. Images use the exact storage format, rather
  than incompatible reinterpretations of the sampled RGBA8 images.

The resource cost therefore differs from the supplied summary: **three sampled
backing images, plus lazy storage backing images for type/format combinations that
are actually requested**. No recurring GPU clear is added. The storage path retains
its cached lookup; it is not accurate to claim literally no additional CPU branch.
The native nullDescriptor path still returns null handles without these allocations.

The renderer's existing shutdown drains the scheduler and waits for the device
before destroying the caches. Callers moving/replacing image views must likewise
retire GPU use before destroying resources; the regression explicitly does this.

Vulkan references: [image/view compatibility and formats](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageViewCreateInfo.html)
and [clear usage, layout and render-pass requirements](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdClearColorImage.html).

## Verification

- `video_core` and the full unit suite compile in Release on macOS arm64.
- **64/64 unit tests pass**, including a portable description test of dimensions,
  flags, formats and layer ranges for all eight sampled texture types.
- The manual `test_null_vulkan_views_gpu` uses the actual production constructor,
  Vulkan loader, MoltenVK and Khronos validation layer. It creates all eight sampled
  types and every supported storage format/type combination, checks reuse, drains
  initialization, and exercises move construction and move assignment/destruction.
- The same native test allocates the production sampled backing descriptions and
  executes the production initialization helper. It copies all layers back to a
  host buffer prefilled with a nonzero marker and verifies every returned byte is
  zero. It checks that no Vulkan validation error occurred, including teardown.

Build the optional native test explicitly:

```sh
cmake --build build --target test_null_vulkan_views_gpu
# Configure the loader's layer/ICD paths for your installed SDK and Vulkan driver.
build/tests/unit/null_vulkan_views/test_null_vulkan_views_gpu /path/to/libvulkan
```

The test requires validation layers and a device using the fallback path; it returns
77 when native nullDescriptor makes that path inapplicable. It enables the platform
surface extension because the existing Device factory enables VK_KHR_swapchain,
though the test creates no window or surface. The local MoltenVK device reports
other emulator feature gaps; this focused resource test does not establish complete
game compatibility on that device.

## Scope and limitations

The depth/stencil change in Lichii
[724aede4](https://github.com/zortos293/Lichii/commit/724aede4ea51fae8224cdd02549cdbdf7ceecd25)
is not included. Its commit itself notes that stencil is not carried through the
reinterpretation. A complete solution needs to preserve both components. SDL3 input
mapping is unchanged.

No gameplay, Windows/Linux execution, affected AMD hardware or FPS improvement is
claimed. Resource creation/clear/readback validation is not a full shader-sampling
or storage read/write test. In particular this does not implement all native null
semantics: writes to fallback storage can persist in that fallback image, and
sampled integer/depth numeric-type handling is not changed by this dimensional fix.
