// Timing is disabled in deterministic cache fixtures. Cache operations are unchanged.
#pragma once
#include <cstddef>
namespace VideoCommon::PerformanceDiagnostics {
enum class Category { ReadbackPlan, ReadbackRetire, ReadbackCopy, ReadbackDeviceCopy,
                      ReadbackSegments, ReadbackRuns, ReadbackSmall, ReadbackLarge,
                      ReadbackStores, ReadbackSkipped, ReadbackGpu, ReadbackGpuSkipped, ReadbackParallel,
                      FlushBufferQuery, FlushBufferSkipped, CpuWriteQuery, CpuWriteBufferSkipped, CpuWriteTextureSkipped };
struct Scope { explicit Scope(Category) {} };
inline void RecordBytes(Category, std::size_t) {}
inline void RecordEvent(Category, std::size_t = 0) {}
}
