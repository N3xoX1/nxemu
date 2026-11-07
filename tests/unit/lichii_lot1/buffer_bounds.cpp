// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using DAddr = u64;
using BufferId = u32;
constexpr BufferId NULL_BUFFER_ID = 0;
struct Buffer
{
    bool IsInBounds(DAddr address, u32 size) const
    {
        return address >= begin && address <= end && size <= end - address;
    }
    u64 SizeBytes() const
    {
        return end - begin;
    }
    DAddr CpuAddr() const
    {
        return begin;
    }
    u32 Offset(DAddr address) const
    {
        return static_cast<u32>(address - begin);
    }
    DAddr begin{}, end{};
};
enum class ObtainBufferSynchronize
{
    None,
    FullSynchronize
};
enum class ObtainBufferOperation
{
    None,
    MarkAsWritten,
    DiscardWrite
};
struct BufferCopy
{
    u64 src_offset, dst_offset, size;
};
namespace boost::container
{
template <class T, size_t>
using small_vector = std::vector<T>;
}
struct Policy
{
    using Buffer = ::Buffer;
};
struct Notifications
{
    unsigned calls{};
    void Add(DAddr, u32)
    {
        ++calls;
    }
    void Subtract(DAddr, u32)
    {
        ++calls;
    }
    void Mark(DAddr, u32)
    {
        ++calls;
    }
    void InvalidateNativeReadRegion(DAddr, u32)
    {
        ++calls;
    }
    void MarkRegionAsGpuModified(DAddr, u32)
    {
        ++calls;
    }
    void MarkHostWrite(Buffer &)
    {
        ++calls;
    }
    template <class F>
    void ForEachUploadRange(DAddr, u32, F &&)
    {
        ++calls;
    }
    template <class F>
    void ForEachInRange(DAddr, u64, F &&)
    {
        ++calls;
    }
};
template <class P>
struct BufferCache
{
    using Buffer = typename P::Buffer;
    static constexpr u32 CACHING_PAGEBITS = 16;
    std::array<BufferId, 4> page_table{};
    std::array<Buffer, 2> slot_buffers{};
    Notifications memory_tracker, device_memory, runtime, untracked_gpu_writes,
        gpu_write_history, gpu_modified_ranges, uncommitted_gpu_modified_ranges;
    unsigned changed{}, uploads{}, cleared{};
    void MarkBufferContentChanged(Buffer &, DAddr, u64)
    {
        ++changed;
    }
    void UploadMemory(Buffer &, u64, u64, std::span<BufferCopy>)
    {
        ++uploads;
    }
    void ClearDownload(DAddr, u32)
    {
        ++cleared;
    }
    void MarkWrittenBuffer(BufferId, DAddr, u32, bool tracked = false);
    bool SynchronizeBuffer(Buffer &, DAddr, u32, boost::container::small_vector<BufferCopy, 4> &);
    bool SynchronizeBuffer(Buffer & buffer, DAddr address, u32 size)
    {
        boost::container::small_vector<BufferCopy, 4> copies;
        return SynchronizeBuffer(buffer, address, size, copies);
    }
    std::pair<Buffer *, u32> ObtainCPUBuffer(DAddr, u32, ObtainBufferSynchronize,
                                             ObtainBufferOperation);
    unsigned created{};
    BufferId FindBuffer(DAddr address, u32 size);
    BufferId CreateBuffer(DAddr, u32)
    {
        ++created;
        return 1;
    }
};
#include "find_buffer.inc"

void Require(bool condition)
{
    if (!condition) throw std::runtime_error("buffer cache bounds regression");
}
int main()
{
    BufferCache<Policy> cache;
    constexpr u64 end = 4ULL << 16;
    for (const auto address : std::array<u64, 5>{0, end, end + 1, 0x72253b2f2890ULL,
                                                 std::numeric_limits<u64>::max()})
    {
        Require(cache.FindBuffer(address, 1) == NULL_BUFFER_ID);
    }
    Require(cache.FindBuffer(1, 0) == NULL_BUFFER_ID);
    Require(cache.FindBuffer(end - 1, 2) == NULL_BUFFER_ID);
    Require(cache.FindBuffer(end - 32, std::numeric_limits<u32>::max()) == NULL_BUFFER_ID);
    Require(cache.created == 0);
    Require(cache.FindBuffer(end - 1, 1) == 1);
    Require(cache.FindBuffer(end - 65536, 65536) == 1);
    Require(cache.created == 2);
    cache.page_table[1] = 1;
    cache.slot_buffers[1] = {65536, 2 * 65536};
    Require(cache.FindBuffer(65536 + 4, 12) == 1);
    Require(cache.created == 2);
    Require(cache.FindBuffer(65536, 65537) == 1);
    Require(cache.created == 3);
    // The same rejection applies when the first page already has a cached buffer.
    cache.page_table.back() = 1;
    Require(cache.FindBuffer(end - 1, 2) == NULL_BUFFER_ID);
    Require(cache.created == 3);
    // Rejected ranges must never reach the finite memory-tracker table or post operations.
    const DAddr invalid = std::numeric_limits<u64>::max();
    Require(cache.SynchronizeBuffer(cache.slot_buffers[0], invalid, 16));
    cache.MarkWrittenBuffer(NULL_BUFFER_ID, invalid, 16);
    for (const auto operation : {ObtainBufferOperation::MarkAsWritten,
                                 ObtainBufferOperation::DiscardWrite})
    {
        const auto [buffer, offset] = cache.ObtainCPUBuffer(
            invalid, 16, ObtainBufferSynchronize::FullSynchronize, operation);
        Require(buffer == &cache.slot_buffers[0] && offset == 0);
    }
    Require(cache.memory_tracker.calls == 0 && cache.device_memory.calls == 0);
    Require(cache.runtime.calls == 0 && cache.gpu_write_history.calls == 0);
    Require(cache.untracked_gpu_writes.calls == 0 && cache.gpu_modified_ranges.calls == 0);
    Require(cache.uncommitted_gpu_modified_ranges.calls == 0);
    Require(cache.changed == 0 && cache.uploads == 0 && cache.cleared == 0);
    // Valid buffers still synchronize and publish writes normally.
    Require(cache.SynchronizeBuffer(cache.slot_buffers[1], 65536, 16));
    cache.MarkWrittenBuffer(1, 65536, 16);
    Require(cache.memory_tracker.calls == 2 && cache.device_memory.calls == 1);
    Require(cache.changed == 1 && cache.runtime.calls == 1);
}
