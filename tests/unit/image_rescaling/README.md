# Small depth prepasses and resolution scaling

Mario Party Superstars renders its character portraits with a depth-only prepass
followed by color passes using an EQUAL depth test. In the 2026-10-28 capture at
2x, the depth prepass used a 460x460 viewport, while color used 230x230. The depth
image was repeatedly scaled between 480x460 and 240x230 (including row padding).
Nearest-neighbor downsampling does not preserve depth values at the native
pixel centers, so the EQUAL test rejected almost all portrait fragments.

Color images of height 288 or less were already excluded from scaling. Depth
attachments and depth texture descriptors now follow that same threshold,
keeping these passes on one pixel grid. Larger targets retain their existing
scaling behavior. Small standalone depth targets also remain native.

The test exercises the production color, depth and texture ImageInfo
constructors at portrait size, both sides of the threshold, and normal display
size. It also checks the block-depth restriction and downscale consistency.

Diagnostic evidence is in `build/face-scaling-20261028/`: replaying the original
dump reproduces the missing portraits. A separate replay experiment changes
only the 460x460 viewports and both directions of depth transfers to use native
coordinates. Both captured frames then show complete portraits, including eyes.
This experiment verifies the cause; the rebuilt emulator still needs an in-game
confirmation after relaunch.
