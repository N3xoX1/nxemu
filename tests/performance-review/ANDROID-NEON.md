# ARM64 comparison experiments

The Android project builds arm64-v8a only, with minimum API 28 and NDK
26.1.10909125. All ARM64 Android devices support NEON:
https://developer.android.com/ndk/guides/cpu-arm-neon

The production comparator already selects NEON for AArch64 instead of AVX2.
`neon-reference-strategies.cpp` compares six test-only strategies: production
NEON equality, libc, XOR/OR reductions over 64 or 256 bytes, explicit prefetch
and four-vector loads. The baseline uses ARMv8 instructions, without requiring
SVE, SVE2 or a CPU-vendor extension. No experimental strategy is installed in
production without physical-device performance evidence.

All six passed AArch64 QEMU with Cortex-A53 and maximum CPU models: every byte
position through 513 bytes, SIMD/unroll boundaries, shifted addresses, random
long spans, zero sizes and exact tails against inaccessible guard pages. The
production parallel copy fixture also passes on ARM64, including all byte
oracles, explicit scope lifetime, nested scope restoration and consumer visibility.
Generated disassembly contains NEON loads, comparisons, reductions and prefetch
instructions as expected. These are GNU/Linux ARM64 correctness builds. They do
not constitute an Android SDK/NDK build or an Android hardware timing result.

For a Windows NDK installation, build the native Android fixture with:

```powershell
./tests/performance-review/build-android-neon-strategies.ps1 -Ndk 'C:/path/to/android-ndk'
```

This wrapper has been syntax checked; an NDK installation and connected Android
device are unavailable on the current host, so its compilation and deployment
remain unverified. The GNU/Linux static test binary is not substituted for an
Android build. The Android wrapper links the C++ runtime statically, leaving the
normal Android system libraries dynamic.

After building, on an authorized ADB-connected arm64 device:

```text
adb push build/performance-live-20261011/neon-android-tests/neon-strategies /data/local/tmp/neon-strategies
adb shell chmod 755 /data/local/tmp/neon-strategies
adb shell /data/local/tmp/neon-strategies
adb shell /data/local/tmp/neon-strategies --benchmark
```

`--benchmark` first runs correctness checks, then emits 54 timing rows for three
physical working sets (4/16/64 MiB), three changed-chunk patterns and six strategies.
Every call compares an exact 64 KiB chunk as in the production readback loop,
processing 128 MiB per sample. Two warmups precede five samples; execution order
rotates and the median is reported. Source changes are outside the timed region;
every equal-chunk count is checked against an independent expected count after
timing. Actual changed fraction is printed, rather than assuming that a small
working set represents exactly ten percent. This benchmark measures comparison,
not GPU transfers, copies, thread wakeup, thermal stability or emulator FPS.

For meaningful device results, record the SoC, device and Android version, avoid
simultaneous GPU/game load, and repeat the sweep after cooling. Heterogeneous
Android CPUs can make four worker threads slower than two; desktop results do
not justify enabling them by default on mobile.
