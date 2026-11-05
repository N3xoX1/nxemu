# Intel CMAA2 in NxEmu

Enable the optional Vulkan anti-aliasing backend with `-DNXEMU_ENABLE_INTEL_CMAA2=ON`.
The build requires DXC, Python 3, and the SPIR-V tools. It adapts the Intel source
and generates shader headers at build time.

The original source and license are in `src/3rd_party/intel_cmaa2`.
The original source is also available from [Intel CMAA2](https://github.com/GameTechDev/CMAA2).
