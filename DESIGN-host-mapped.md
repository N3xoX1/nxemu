# Design: guest RAM as Vulkan memory (track U1)

Status: uniform buffers, with the flicker fixed (2026-11-07). Delete before merging.

**Cleanup (2026-11-06):** the TEMP counters, timers and env switches named in this file were removed before the commit. They stay here as a record of how each result was measured. The kept defaults are the ones that were measured "on".

## Why

At about 130 fps on TOTK the GPU thread is the limit. Uniform buffer binds cost 12 % of it (`BindHostGraphicsUniformBuffer`: `SynchronizeBuffer` 4.5 %, `TouchBuffer`, usage tracking, the streamed copy). The Switch shares memory between CPU and GPU. If the guest RAM is device memory, a uniform buffer that only the guest CPU writes can be read by the GPU in place.

## Driver check

`scratchpad/hostimport/hostimport.cpp`: on the RTX 5070 Ti (Windows), a pagefile-backed section view, like `HostMemory`, imports with `VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT`, also whole at 8 GiB. Import alignment 4 KiB. Memory type: host visible + coherent, system heap.

## How it works

- `vulkan_device.h`: `VK_EXT_external_memory_host` is now loaded when supported (it was only listed). `IsExtExternalMemoryHostSupported`.
- `BufferCacheRuntime::InitHostMappedGuestMemory` (first uniform bind): imports `DeviceMemoryManager::PhysicalBase()` / `PhysicalSize()` as one `VkDeviceMemory`, and creates one `VkBuffer` per 1 GiB window, each 64 KiB longer, so a uniform range always fits one window with a 32-bit offset.
- `DeviceMemoryManager::MarkTimelineWrite` / `HasTimelineWrite`: a sticky bit per device page. `MemoryManager::WriteBlockImpl` (inline constant buffer data, I2M, DMA, every engine write) marks it.
  - Those writes are ordered in the GPU timeline. A draw queued before the write must see the old data, so these pages keep the copy paths.
- `BindHostGraphicsUniformBuffer` (Vulkan only) binds in place when the range has no timeline write, is not GPU modified, is physically contiguous (`GetSpan`) and is aligned to `minUniformBufferOffsetAlignment`. Otherwise it takes the old path.
- TEMP: `NXEMU_HOST_MAPPED_UBO=0` turns it off; a log line every 600 frames counts in-place and copied binds.

## Hazards accepted for the prototype

- Guest CPU writes to a range that queued host GPU work still reads. On the console the game must wait on a fence; the emulator signals fences after host completion, so this should hold. Watch for flicker.
- A buffer download (GPU data written back to guest RAM) while an earlier in-place draw is still queued.
- `MemoryManager::Write<T>` (semaphores, small writes) does not mark pages.

## Result (TOTK)

- 80 % of uniform binds go in place (about 420,000 vs 106,000 per 600 frames). Correct rendering at 100 and 125 s.
- Fps (same DLL, interleaved, mean from 75 s): off 136.6 / 133.7, on 140.1 / 137.9. **+2.8 %.**

## Flicker found (2026-11-06)

- A window move flickers once on release. Not on master; it goes away with the in-place binds off (same DLL). So the "guest CPU writes a range that queued GPU work still reads" hazard is real.
- Off on the main branch: TOTK 134.6 fps (2 runs), against 140.8 with it on (3 runs, earlier the same evening).
- Likely path, not checked yet: during the move the present and submit side stalls, guest syncpoint waits time out, the game rewrites its uniform data, and the delayed draws read the new data.
- A correct fix needs to know, when the guest writes a page, whether queued GPU work reads it in place (write protect those pages until their fence, and wait on a fault), or keep the guest from running ahead of the host GPU.

## Flicker fix (2026-11-07)

Two paths told the guest that GPU work was done before the host GPU ran it. With copies that is harmless (each draw reads its own copy); with in-place reads the guest rewrote data that queued draws still read.

- Syncpoints: `FenceManager::SubmitFence` runs the signal at once when the fence needs no delay, and the fence is a stub. Now `BufferCache::TakeInPlaceBinds` (set by every in-place bind) makes the next fence a real, delayed one. Later fences stay delayed through `pending_delayed_fences`.
- Semaphore payloads: `QueryCacheBase::CounterReport` published fast payloads into guest RAM at once, even for fences. Now `RasterizerInterface::InPlaceReadsPending` (the Vulkan runtime keeps the scheduler tick of the last in-place bind) keeps them behind their fence, like a payload behind a readback.
- Fixing only the syncpoints did not stop the flicker. Both together did.
- The payload result must stay fault protected. Publishing it late without protection (so a guest read sees the old value instead of waiting) brought the flicker back. The guest path that needs this is not known.

TOTK, interleaved, same DLLs, mean from 75 s (the machine was slower than on 2026-11-06: Parsec and other work running):

| Round | Off | In place + fix |
|---|---|---|
| 1 | 120.3 | 124.4 |
| 2 | 121.5 | (stuck on the save menu, dropped) |
| 3 | 120.5 | 125.3 |

**About +3.7 %.** The trees at 125 s are fine, and the window move no longer flickers.

Still open: the hazards above on buffer downloads and on `MemoryManager::Write<T>`; other games.
