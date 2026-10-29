# Stable IGN alpha coverage

Single-sample alpha-to-coverage uses Interleaved Gradient Noise instead of the
quantized 2x2 coverage mask. The IGN constants follow Jorge Jimenez's
[SIGGRAPH 2014 presentation](https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare/).

The fragment epilogue compares the output alpha directly with
`fract(52.9829189 * fract(x * 0.06711056 + y * 0.00583715))`.
`x` and `y` are the raw host `FragCoord` pixel centers. They are not divided by
the guest resolution scale. This keeps the grain at one render pixel at 2x/4x
instead of making blocks of guest pixels. There is no time, frame counter or
jitter seed, so fixed screen pixels have fixed thresholds. Coverage increases
monotonically with alpha rather than jumping between eight levels.

This remains a single-sample coverage approximation, not alpha blending or
order-independent transparency. Moving objects move through the screen-space
thresholds. Multisample targets keep their existing native alpha-to-coverage.
Integer attachments and shaders without an alpha output are left alone.

The test checks continuous coverage at 1x/2x/3x/4x/6x, alpha endpoints, NaNs,
infinities, monotonicity, mixed 4x4 coverage and absence of a repeating 2x2 mask.
It compiles the production SPIR-V backend and verifies the unchanged shader at
different resolution settings, no texture reads/loops, and the fixed arithmetic
budget (three multiplies, one add and two fractions). The ordered alpha
comparison naturally handles endpoints and NaNs without extra clamp operations.
NoContraction decorations keep compiler fusion from changing the thresholds.

Local GPU verification on the RTX 5070 Ti rendered the production test module
twice at 128x128, 256x256, 512x512 and 4096x256. Every pixel matched the CPU
reference; successive renders were identical. At alpha 0.5, coverage ranged
from 0.500168 to 0.500549. These are correctness measurements, not an in-game
FPS benchmark. The generated module also passes `spirv-val --target-env vulkan1.1`.

The Vulkan pipeline cache version is incremented so cached 2x2 shaders are not
reused. In-game fade appearance and the macOS driver still require verification
on those workloads; the GPU test above covers the Windows Vulkan implementation.
