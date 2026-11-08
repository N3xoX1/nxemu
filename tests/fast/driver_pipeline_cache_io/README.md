# Lichii Lot 3: Vulkan driver cache persistence

Reviewed against nxemu upstream `cf6db084` on 2026-11-07. Adapted from Lichii
[ea33ad1865970ee731e5f74dbcc26ca472798afe](https://github.com/zortos293/Lichii/commit/ea33ad1865970ee731e5f74dbcc26ca472798afe).
This branch starts independently from master, without Lot 1 or Lot 2.

## Problem and behavior

The original save opened `vulkan_pipelines.bin` with truncation before the new
contents were written. Its failure handler then removed that destination. A failed
write or process exit could therefore lose the previous usable cache.

The new save snapshots the driver before opening any file, then exclusively
creates a unique temporary sibling. It writes the unchanged magic/version/payload
format, flushes stdio buffers, commits the file with `fsync` or `_commit`, and
checks close before publishing. Publication uses a same-filesystem rename on POSIX
and `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)` on Windows. No failure path
removes the destination. Temporary cleanup is best effort and only targets the
current writer's own file.

The reader checks lengths, every header/payload read, magic and version. Invalid,
short or unavailable files cause an empty Vulkan cache to be created. Short-file
handling no longer depends on matching a particular `ios_base::failure` type.

## Improvements over the supplied patch

- **Separate staging for each writer:** the fixed `.tmp` name could be truncated
  or renamed by another instance. Exclusive creation and retry handle collisions.
- **Checked synchronization and close:** a successful write into a buffered stream
  alone does not prove that committing or closing the file succeeded.
- **Test the production reader:** the supplied regression only exercised saving.
  The renderer and tests now call the same parser, including short reads after
  obtaining a file's size.
- **Preserve files on read failures:** deleting an invalid path after opening it
  could remove a good replacement published by another process. Invalid files are
  ignored and can be replaced by the next successful save.
- **Best-effort optional cache saving:** standard I/O/allocation failures are logged
  without deleting a cache or escaping the renderer's save path.

The loader-metadata unit-test fixture also receives the same `loaderSettings`
correction used in Lots 1/2 so the complete unit suite can link.

## Regression coverage

The test uses real files and the production helper, with deterministic failure
operations at the filesystem boundary:

- Exact legacy wrapper layout, embedded NUL bytes, replacement, Unicode paths,
  and empty payloads.
- Every wrapper length from zero through eleven bytes; incorrect magic/version;
  failed size queries and simulated truncation during header/payload reads.
- Open errors, partial writes in each of the three write calls, commit errors,
  close errors, and failed replacement of the same existing cache. Previous bytes
  remain identical and loadable; failed first saves publish no destination.
- A real replacement failure against a nonempty directory.
- A staging-name collision that must leave the other writer's file untouched.
- On POSIX, abrupt child-process exits after each write, after commit, and after
  close. The previous cache survives and a subsequent save succeeds despite stale
  staging files.
- Four concurrent writers and a concurrent reader. Reads must always see a
  complete snapshot. Windows sharing violations may reject a save while a CRT
  reader holds the destination; a save after readers close must succeed.

The regression runs under ASan/UBSan on non-MSVC builds and is registered in the
standalone fast suite, full unit suite and Linux CI subset.

```sh
cmake -S tests/fast -B build/fast-standalone -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/fast-standalone --target nxemu_fast_tests
ctest --test-dir build/fast-standalone -L '^fast$' --output-on-failure
```

## Validation and limits

- Release `video_core` compilation on macOS arm64.
- Full unit suite: **64/64 passed**.
- Standalone fast suite: **4/4 passed**.

This tests process interruption and I/O failure, not an actual power cut. File
contents are synchronized before rename; POSIX directory metadata is not explicitly
fsynced, so full power-loss durability is not claimed. An interrupted process can
leave its temporary sibling; loads ignore it. There is no wildcard sweep that could
delete another live writer's temporary file.

Concurrent successful saves follow last-writer-wins semantics; driver cache blobs
are not merged across processes. The legacy format has no payload checksum or
stored payload length, so this change does not detect arbitrary opaque-driver-blob
corruption. Driver/device compatibility remains governed by the existing Vulkan
creation path. No real-game, Windows/Linux runtime, power-loss or FPS validation is
claimed.
