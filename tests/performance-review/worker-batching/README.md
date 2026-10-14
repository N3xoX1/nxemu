# Worker handoff batching (V15-A)

Opportunistic CPU command dispatch occurs every 32 draws instead of every 8. The original eight-draw check and periodic GPU submission threshold remain, so driver submissions occur at exactly the original draw indices. Explicit flushes, waits, command-chunk rollover, memory barriers and cache protections are unchanged. The separate notification experiment B is excluded.

Run `run.ps1 -Python <python>` in a Visual Studio developer PowerShell. It extracts the actual current FlushWork and the pre-change e7dde4e9 method. Each Windows/Android preprocessor branch checks 3.2 million draws, including 31 patterns of external flush/reset, exact submission positions and counter state, and that new opportunistic handoffs are a subset of the original ones. The Android branch runs on the host; this is not native Android validation.

Additional pre-package queue tests used extracted production command storage, mutex exchange and worker execution. Seven alternating CPU benchmark rounds over five workloads found about 51–57% lower scheduling cost for light workloads and 11% for an intermediate workload; the worker-heavy case differed by +0.41%. These are component timings, not FPS predictions. The user's latest Zelda captures (same settings, no invalidations/drops, seconds 5–15) give reference C+MH 82.44 FPS versus A 85.28 FPS (+3.44%). A's 1% low is 68.34 versus 70.09, with only nine tail samples in the short capture. No universal tail improvement is claimed.

This change retains the disjoint buffer mapping and immediate sampler fixes and introduces no title-specific behavior.
