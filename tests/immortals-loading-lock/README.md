# Immortals async-read cancellation lock order

The captured save load has two live guest threads waiting circularly: completion owns the read-request mutex and waits for the manager mutex; cancellation owns the manager mutex and waits for the read-request mutex. This differs from the VP9 copied-mutex/terminated-owner issue. The affected guest functions are byte-identical to the earlier October 9 dump; a timing change may expose the race, but the rendering patch does not implement these locks.

The build-specific IPS changes only the failed-TryLock retry/block path to return false via the existing epilogue. The caller already handles an unsuccessful cancellation and leaves the in-flight completion to finish. No other owner's lock is released. Successful cancellation, callback cleanup, memory protections and rendering optimizations are retained.

Supported main build ID: `70F3F6751D73C644BCE904CCB414E35B00000000000000000000000000000000`.
Guest address: `0x80ade0d4`; decompressed IPS offset including the NSO header: `0xada1d4`; length: eight bytes.
Original: `f6031f2a15e28452`; replacement: `e0031f2af5ffff17`.

Install the `exefs` directory as an Immortals mod under `user/load/01004A600EC0A000/Immortals Async Read Cancellation Lock Fix/`. Keep the separate VP9 mod. This is a game compatibility patch, not a generic kernel deadlock breaker, and is not applied automatically to every game.

Validation: 40 captured-code ARM64 scenarios, including 20 original blocking paths; exact caller/completion continuation; final manager/job/callback-record data identical to original serial cleanup; production IPS parser bounds and truncation checks. SDK mutex/sleep/notification operations are modeled in the ARM64 fixture. Human validation passed ten save-loading runs on v4 plus this patch; an intermittent failure cannot be ruled out by a finite run count.

Reproduce from the repository root (Unicorn in the documented local dependency directory):

```powershell
python tests/immortals-loading-lock/test_arm64.py build/immortals-loading-hang-20261013/immortals-loading-full.dmp
python tests/immortals-loading-lock/test_continuation.py build/immortals-loading-hang-20261013/immortals-loading-full.dmp
python tests/immortals-loading-lock/test_ips.py
powershell -File tests/immortals-loading-lock/test_ips.ps1
```

The ARM64 fixtures use the preserved October 13 blocked-process snapshot and its documented instrumented memory layout. The memory dump stays local and is not distributed. Regenerate the identical IPS with `python tests/immortals-loading-lock/patch.py <output-directory>`.
