# Intel CMAA2 in NxEmu

Vulkan builds include CMAA2 by default and use the validated SPIR-V headers
in `src/3rd_party/intel_cmaa2/spirv`. Disable it with
`-DNXEMU_ENABLE_INTEL_CMAA2=OFF`.

When changing the Intel HLSL source or its Vulkan adaptation, install DXC,
`spirv-val` and Python 3, then regenerate the headers from the repository root.

```sh
cmake -S . -B build -DNXEMU_REGENERATE_CMAA2_SHADERS=ON
cmake --build build --target regenerate_cmaa2_shaders
```

Commit the updated inputs and generated headers together. The source and
license are in `src/3rd_party/intel_cmaa2`. See the [Intel CMAA2 project](https://github.com/GameTechDev/CMAA2).
