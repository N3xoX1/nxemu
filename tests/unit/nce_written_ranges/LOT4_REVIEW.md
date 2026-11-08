# Lot 4: GPU dirty memory collection

Base: upstream master `cf6db084`, 2026-11-07. This branch is independent of Lots 1–3.

The Gather serialization is adapted from Lichii
[7e1d6455572ac894d4098231ab197beee5b5072c](https://github.com/zortos293/Lichii/commit/7e1d6455572ac894d4098231ab197beee5b5072c).
The CLI restart/handoff changes in that commit are outside this lot.

## Review

All three proposed fixes are retained:

- `gather_guard` covers swapping, callbacks and clearing of **both** rounded and
  exact front buffers. Protecting the swap alone would leave traversal exposed to
  a later gather. `guard` is released before callbacks, allowing collectors to
  continue while a gather processes its batch.
- Different-window retirement uses compare/exchange against the observed complete
  address/mask pair. Another collector can modify that pair while the first waits
  for `guard`. Unconditional exchange lost that newer range. The candidate is
  appended before CAS for allocation safety; a failed CAS rolls back only that
  candidate under `guard`, then retries. Same-window updates retain the atomic
  mask-merge path. Gather's exchange also takes `guard`, so it cannot detach the
  candidate queue halfway through successful retirement.
- Validity concerns a 2-KiB window **index**, not a byte address: the bound is
  `2^(39-11)`, not `2^39`. The all-ones sentinel is consequently invalid. Empty
  gathers no longer queue the sentinel; normal address zero and the last valid
  window remain representable.

Lock order inside this class is `gather_guard` then `guard`; collectors acquire
only `guard`, so there is no reverse internal acquisition. The reviewed Vulkan
and OpenGL invalidation callbacks mark cache ranges and do not recursively gather.
Callbacks must remain non-reentrant with respect to Gather on the same manager.
This change does not establish safety for arbitrary callbacks that invert external
cache locks, nor does it alter the emulator's guest-memory visibility protocol.

## Verification

The supplied tests were expanded to include:

- both exact and rounded buffers while the first gather callback is paused;
- simultaneous collectors and gatherers, followed by a final drain and an exact
  comparison against every expected range;
- the last valid window, the first invalid index, the all-ones sentinel and empty
  repeated gathers;
- the existing exact-write tests that preserve neighboring undownloaded GPU data.

The callback test starts the second gather explicitly before its timed observation;
final batch contents are also checked. Scheduling stress tests are not a formal
proof of every possible interleaving.

The normal test is instrumented with ASan/UBSan on non-MSVC builds. A separate
ThreadSanitizer executable instruments the same real header and tests. Both are
registered in the Linux CI subset. No sanitizer checks were disabled.

Running the expanded test against the original master header reproduced missing
collector ranges, incorrect gather overlap and invalid-window handling, together
with TSan reports on front-buffer swaps/accesses. The corrected version passed
20 repeated direct TSan runs.

The loader metadata fixture also receives the same `loaderSettings` correction
used in preceding lots so the full unit suite links.

No real-game VP9 playback, Windows/Linux execution or FPS gain is claimed. The
expected benefit is correctness and stability; serializing overlapping gathers
can introduce contention, and no throughput benchmark was performed.

Final validation on macOS arm64: the `nxemu-os` shared module and complete unit
suite compiled successfully; **64/64 CTest cases passed**, including ASan/UBSan
and TSan variants of the written-range regression. `git diff --check` passed.
