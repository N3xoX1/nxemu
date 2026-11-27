// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <vector>
#include <boost/container/small_vector.hpp>

#include "yuzu_common/common_types.h"
#include "yuzu_common/scratch_buffer.h"

#include "yuzu_video_core/surface.h"
#include "yuzu_video_core/texture_cache/image_base.h"
#include "yuzu_video_core/texture_cache/types.h"
#include "yuzu_video_core/textures/texture.h"

namespace Settings {
struct ResolutionScalingInfo;
}

namespace VideoCommon {

using Tegra::Texture::TICEntry;

using LevelArray = std::array<u32, MAX_MIP_LEVELS>;

struct OverlapResult {
    GPUVAddr gpu_addr;
    VAddr cpu_addr;
    SubresourceExtent resources;
};

[[nodiscard]] u32 CalculateGuestSizeInBytes(const ImageInfo& info) noexcept;

[[nodiscard]] u32 CalculateUnswizzledSizeBytes(const ImageInfo& info) noexcept;

[[nodiscard]] u32 CalculateConvertedSizeBytes(const ImageInfo& info) noexcept;

[[nodiscard]] u32 CalculateLayerStride(const ImageInfo& info) noexcept;

[[nodiscard]] u32 CalculateLayerSize(const ImageInfo& info) noexcept;

[[nodiscard]] LevelArray CalculateMipLevelOffsets(const ImageInfo& info) noexcept;

[[nodiscard]] LevelArray CalculateMipLevelSizes(const ImageInfo& info) noexcept;

[[nodiscard]] boost::container::small_vector<u32, 16> CalculateSliceOffsets(const ImageInfo& info);

[[nodiscard]] boost::container::small_vector<SubresourceBase, 16> CalculateSliceSubresources(
    const ImageInfo& info);

[[nodiscard]] u32 CalculateLevelStrideAlignment(const ImageInfo& info, u32 level);

[[nodiscard]] VideoCore::Surface::PixelFormat PixelFormatFromTIC(
    const Tegra::Texture::TICEntry& config) noexcept;

[[nodiscard]] ImageViewType RenderTargetImageViewType(const ImageInfo& info) noexcept;

[[nodiscard]] boost::container::small_vector<ImageCopy, 16> MakeShrinkImageCopies(
    const ImageInfo& dst, const ImageInfo& src, SubresourceBase base, u32 up_scale = 1,
    u32 down_shift = 0);

[[nodiscard]] boost::container::small_vector<ImageCopy, 16> MakeReinterpretImageCopies(
    const ImageInfo& src, u32 up_scale = 1, u32 down_shift = 0);

[[nodiscard]] bool IsValidEntry(const Tegra::MemoryManager& gpu_memory, const TICEntry& config);

[[nodiscard]] boost::container::small_vector<BufferImageCopy, 16> UnswizzleImage(
    Tegra::MemoryManager& gpu_memory, GPUVAddr gpu_addr, const ImageInfo& info,
    std::span<const u8> input, std::span<u8> output);

struct UnswizzleRowRun {
    GPUVAddr src_addr;
    size_t guest_bytes;
    size_t host_offset;
    size_t host_bytes;
    u32 run_tiles;
    u32 tile_rows;
    Extent3D block;
    u32 run_alignment;
};

struct UnswizzlePlan {
    boost::container::small_vector<BufferImageCopy, 16> copies;
    boost::container::small_vector<UnswizzleRowRun, 16> runs;
    size_t total_host_bytes{0};
    u32 bpp_log2{0};
    bool layout_valid{true};
};

/// Plans the unswizzle of the blocks of a 2D block-linear image that overlap the guest byte range
/// [begin, end). Reads no guest memory. layout_valid is false for layouts it does not handle.
[[nodiscard]] UnswizzlePlan PlanUnswizzleImageRows(GPUVAddr gpu_addr, const ImageInfo& info,
                                                   u64 begin, u64 end);

/// Runs a plan into `output` (at least plan.total_host_bytes bytes).
void ExecuteUnswizzlePlan(Tegra::MemoryManager& gpu_memory, const UnswizzlePlan& plan,
                          std::span<u8> output, std::vector<u8>& scratch);

void ConvertImage(std::span<const u8> input, const ImageInfo& info, std::span<u8> output,
                  std::span<BufferImageCopy> copies);

[[nodiscard]] boost::container::small_vector<BufferImageCopy, 16> FullDownloadCopies(
    const ImageInfo& info);

[[nodiscard]] Extent3D MipSize(Extent3D size, u32 level);

[[nodiscard]] Extent3D MipBlockSize(const ImageInfo& info, u32 level);

[[nodiscard]] boost::container::small_vector<SwizzleParameters, 16> FullUploadSwizzles(
    const ImageInfo& info);

void SwizzleImage(Tegra::MemoryManager& gpu_memory, GPUVAddr gpu_addr, const ImageInfo& info,
                  std::span<const BufferImageCopy> copies, std::span<const u8> memory,
                  Common::ScratchBuffer<u8>& tmp_buffer);

[[nodiscard]] bool IsBlockLinearSizeCompatible(const ImageInfo& new_info,
                                               const ImageInfo& overlap_info, u32 new_level,
                                               u32 overlap_level, bool strict_size) noexcept;

[[nodiscard]] bool IsPitchLinearSameSize(const ImageInfo& lhs, const ImageInfo& rhs,
                                         bool strict_size) noexcept;

[[nodiscard]] bool IsBlockLinearSizeCompatibleBPPRelaxed(const ImageInfo& lhs, const ImageInfo& rhs,
                                                         u32 lhs_level, u32 rhs_level) noexcept;

[[nodiscard]] std::optional<OverlapResult> ResolveOverlap(const ImageInfo& new_info,
                                                          GPUVAddr gpu_addr, VAddr cpu_addr,
                                                          const ImageBase& overlap,
                                                          bool strict_size, bool broken_views,
                                                          bool native_bgr);

[[nodiscard]] bool IsLayerStrideCompatible(const ImageInfo& lhs, const ImageInfo& rhs);

[[nodiscard]] std::optional<SubresourceBase> FindSubresource(const ImageInfo& candidate,
                                                             const ImageBase& image,
                                                             GPUVAddr candidate_addr,
                                                             RelaxedOptions options,
                                                             bool broken_views, bool native_bgr);

[[nodiscard]] bool IsSubresource(const ImageInfo& candidate, const ImageBase& image,
                                 GPUVAddr candidate_addr, RelaxedOptions options, bool broken_views,
                                 bool native_bgr);

[[nodiscard]] bool IsSubCopy(const ImageInfo& candidate, const ImageBase& image,
                             GPUVAddr candidate_addr);

void DeduceBlitImages(ImageInfo& dst_info, ImageInfo& src_info, const ImageBase* dst,
                      const ImageBase* src);

[[nodiscard]] u32 MapSizeBytes(const ImageBase& image);

[[nodiscard]] ImageInfo ScaledImageInfo(const ImageInfo& info,
                                        const Settings::ResolutionScalingInfo& resolution);

[[nodiscard]] bool ScaledLevelsMatch(const ImageInfo& info,
                                     const Settings::ResolutionScalingInfo& resolution);

[[nodiscard]] SubresourceRange ClampLevels(SubresourceRange range, s32 num_levels);

} // namespace VideoCommon
