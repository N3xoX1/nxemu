// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <iosfwd>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "yuzu_common/common_types.h"
#include "yuzu_common/polyfill_thread.h"
#include "yuzu_common/unique_function.h"
#include "yuzu_shader_recompiler/environment.h"
#include "yuzu_video_core/engines/maxwell_3d.h"

namespace Tegra {
class Memorymanager;
}

namespace VideoCommon {

/// Bit of the size dependency mask set by shaders specialized on the texture pool sizes.
inline constexpr u32 TEXTURE_POOL_SIZE_BIT = 1U << 31;

/// Sizes of the graphics texture and sampler tables indexed by runtime handles.
[[nodiscard]] Shader::TexturePoolInfo GraphicsTexturePoolInfo(
    const Tegra::Engines::Maxwell3D::Regs& regs);

class GenericEnvironment : public Shader::Environment {
public:
    explicit GenericEnvironment() = default;
    explicit GenericEnvironment(Tegra::MemoryManager& gpu_memory_, GPUVAddr program_base_,
                                u32 start_address_);

    ~GenericEnvironment() override;

    [[nodiscard]] u32 TextureBoundBuffer() const final;

    [[nodiscard]] u32 LocalMemorySize() const final;

    [[nodiscard]] u32 SharedMemorySize() const final;

    [[nodiscard]] std::array<u32, 3> WorkgroupSize() const final;

    [[nodiscard]] u64 ReadInstruction(u32 address) final;

    [[nodiscard]] std::optional<u64> Analyze();

    void SetCachedSize(size_t size_bytes);

    void SetCachedCode(std::shared_ptr<const std::vector<u64>> code_, size_t size_bytes);

    std::shared_ptr<const std::vector<u64>> ShareCachedCode(size_t size_bytes);

    [[nodiscard]] size_t CachedSizeWords() const noexcept;

    [[nodiscard]] size_t CachedSizeBytes() const noexcept;

    [[nodiscard]] size_t ReadSizeBytes() const noexcept;

    [[nodiscard]] bool CanBeSerialized() const noexcept;

    [[nodiscard]] u64 CalculateHash() const;

    void Dump(u64 pipeline_hash, u64 shader_hash) override;

    void Serialize(std::ofstream& file) const;

    bool HasHLEMacroState() const override {
        return has_hle_engine_state;
    }

    bool HasCbufReplacements() const noexcept {
        return !cbuf_replacements.empty();
    }

    // Banks whose bound sizes were actually queried during shader translation.
    [[nodiscard]] u32 CbufSizeMask() const noexcept;

protected:
    std::span<const u64> Code() const noexcept {
        return shared_code ? std::span<const u64>(*shared_code) : std::span<const u64>(code);
    }
    std::optional<u64> TryFindSize();

    /// Returns std::nullopt when the handle points outside of the texture table, which the
    /// texture cache binds as a null image view.
    std::optional<Tegra::Texture::TICEntry> ReadTextureInfo(GPUVAddr tic_addr, u32 tic_limit,
                                                            bool via_header_index, u32 raw);

    Tegra::MemoryManager* gpu_memory{};
    GPUVAddr program_base{};

    std::vector<u64> code;
    std::shared_ptr<const std::vector<u64>> shared_code;
    std::unordered_map<u32, Shader::TextureType> texture_types;
    std::unordered_map<u32, Shader::TexturePixelFormat> texture_pixel_formats;
    std::unordered_map<u64, u32> cbuf_values;
    std::unordered_map<u64, Shader::ReplaceConstant> cbuf_replacements;

    u32 local_memory_size{};
    u32 texture_bound{};
    u32 shared_memory_size{};
    std::array<u32, 3> workgroup_size{};

    u32 read_lowest = std::numeric_limits<u32>::max();
    u32 read_highest = 0;

    u32 cached_lowest = std::numeric_limits<u32>::max();
    u32 cached_highest = 0;
    u32 initial_offset = 0;

    u32 viewport_transform_state = 1;

    bool has_unbound_instructions = false;
    bool has_hle_engine_state = false;
    bool frozen = false;
};

class GraphicsEnvironment final : public GenericEnvironment {
public:
    struct Snapshot {
        struct ConstantBuffer {
            size_t data_offset{};
            u32 size{};
            bool enabled{};
        };

        std::array<std::array<ConstantBuffer, Tegra::Engines::Maxwell3D::Regs::MaxConstBuffers>,
                   Tegra::Engines::Maxwell3D::Regs::MaxShaderStage>
            constant_buffers{};
        std::vector<u8> constant_buffer_data;
        // Shared between snapshots while the guest descriptor table is unchanged.
        std::shared_ptr<const std::vector<Tegra::Texture::TICEntry>> texture_descriptors;
        std::unordered_map<u64, Shader::ReplaceConstant> cbuf_replacements;
        u32 texture_limit{};
        u32 viewport_transform_state{1};
        bool via_header_index{};
    };

    explicit GraphicsEnvironment() = default;
    explicit GraphicsEnvironment(Tegra::Engines::Maxwell3D& maxwell3d_,
                                 Tegra::MemoryManager& gpu_memory_,
                                 Tegra::Engines::Maxwell3D::Regs::ShaderType program,
                                 GPUVAddr program_base_, u32 start_address_,
                                 std::shared_ptr<const std::vector<u64>> cached_code = {},
                                 size_t cached_size = 0);

    ~GraphicsEnvironment() override = default;

    // Reuses the texture descriptor copy of the previous capture when the guest
    // table did not change. Only accessed by the thread that captures snapshots.
    struct SnapshotCache {
        GPUVAddr texture_address{};
        std::shared_ptr<const std::vector<Tegra::Texture::TICEntry>> texture_descriptors;
    };

    // `new_bytes` receives the memory newly allocated by this capture.
    [[nodiscard]] static std::shared_ptr<const Snapshot> CaptureSnapshot(
        Tegra::Engines::Maxwell3D& maxwell3d, Tegra::MemoryManager& gpu_memory,
        u32 active_stage_mask, SnapshotCache& cache, size_t& new_bytes);

    void SetSnapshot(std::shared_ptr<const Snapshot> snapshot_);

    void ReleaseSnapshot();

    // Upper bound of the memory a capture allocates, given the reuse cache.
    [[nodiscard]] static size_t SnapshotSize(const Tegra::Engines::Maxwell3D& maxwell3d,
                                           u32 active_stage_mask, const SnapshotCache& cache);

    u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override;

    u32 ReadCbufSize(u32 cbuf_index) override;

    std::optional<Shader::TexturePoolInfo> ReadTexturePoolInfo() override;

    Shader::TextureType ReadTextureType(u32 handle) override;

    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override;

    bool IsTexturePixelFormatInteger(u32 handle) override;

    u32 ReadViewportTransformState() override;

    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override;

private:
    [[nodiscard]] std::optional<Tegra::Texture::TICEntry> ReadSnapshotTextureInfo(u32 raw) const;

    Tegra::Engines::Maxwell3D* maxwell3d{};
    std::shared_ptr<const Snapshot> snapshot;
    size_t stage_index{};
};

class ComputeEnvironment final : public GenericEnvironment {
public:
    explicit ComputeEnvironment() = default;
    explicit ComputeEnvironment(Tegra::Engines::KeplerCompute& kepler_compute_,
                                Tegra::MemoryManager& gpu_memory_, GPUVAddr program_base_,
                                u32 start_address_);

    ~ComputeEnvironment() override = default;

    u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override;

    u32 ReadCbufSize(u32 cbuf_index) override;

    u32 ReadTextureTableSize() override;

    Shader::TextureType ReadTextureType(u32 handle) override;

    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override;

    bool IsTexturePixelFormatInteger(u32 handle) override;

    u32 ReadViewportTransformState() override;

    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(
        [[maybe_unused]] u32 bank, [[maybe_unused]] u32 offset) override {
        return std::nullopt;
    }

private:
    Tegra::Engines::KeplerCompute* kepler_compute{};
};

class FileEnvironment final : public Shader::Environment {
public:
    FileEnvironment() = default;
    ~FileEnvironment() override = default;

    FileEnvironment& operator=(FileEnvironment&&) noexcept = default;
    FileEnvironment(FileEnvironment&&) noexcept = default;

    FileEnvironment& operator=(const FileEnvironment&) = delete;
    FileEnvironment(const FileEnvironment&) = delete;

    void Deserialize(std::ifstream& file);

    [[nodiscard]] u64 ReadInstruction(u32 address) override;

    [[nodiscard]] u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override;

    [[nodiscard]] u32 ReadCbufSize(u32 cbuf_index) override;

    [[nodiscard]] u32 ReadTextureTableSize() override;

    [[nodiscard]] std::optional<Shader::TexturePoolInfo> ReadTexturePoolInfo() override;

    [[nodiscard]] u32 CbufSizeMask() const noexcept;

    [[nodiscard]] Shader::TextureType ReadTextureType(u32 handle) override;

    [[nodiscard]] Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override;

    [[nodiscard]] bool IsTexturePixelFormatInteger(u32 handle) override;

    [[nodiscard]] u32 ReadViewportTransformState() override;

    [[nodiscard]] u32 LocalMemorySize() const override;

    [[nodiscard]] u32 SharedMemorySize() const override;

    [[nodiscard]] u32 TextureBoundBuffer() const override;

    [[nodiscard]] std::array<u32, 3> WorkgroupSize() const override;

    [[nodiscard]] std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank,
                                                                               u32 offset) override;

    [[nodiscard]] bool HasHLEMacroState() const override {
        return cbuf_replacements.size() != 0;
    }

    void Dump(u64 pipeline_hash, u64 shader_hash) override;

private:
    std::vector<u64> code;
    std::unordered_map<u32, Shader::TextureType> texture_types;
    std::unordered_map<u32, Shader::TexturePixelFormat> texture_pixel_formats;
    std::unordered_map<u64, u32> cbuf_values;
    std::unordered_map<u64, Shader::ReplaceConstant> cbuf_replacements;
    std::array<u32, 3> workgroup_size{};
    u32 local_memory_size{};
    u32 shared_memory_size{};
    u32 texture_bound{};
    u32 read_lowest{};
    u32 read_highest{};
    u32 initial_offset{};
    u32 viewport_transform_state = 1;
};

void SerializePipeline(std::span<const char> key, std::span<const GenericEnvironment* const> envs,
                       const std::filesystem::path& filename, u32 cache_version);

template <typename Key, typename Envs>
void SerializePipeline(const Key& key, const Envs& envs, const std::filesystem::path& filename,
                       u32 cache_version) {
    static_assert(std::is_trivially_copyable_v<Key>);
    static_assert(std::has_unique_object_representations_v<Key>);
    SerializePipeline(std::span(reinterpret_cast<const char*>(&key), sizeof(key)),
                      std::span(envs.data(), envs.size()), filename, cache_version);
}

/// Loads the pipelines of a cache file. The callbacks return false for a record that is not
/// needed, such as a duplicate of an earlier one, and the file is rewritten without them.
void LoadPipelines(
    std::stop_token stop_loading, const std::filesystem::path& filename, u32 expected_cache_version,
    Common::UniqueFunction<bool, std::ifstream&, FileEnvironment> load_compute,
    Common::UniqueFunction<bool, std::ifstream&, std::vector<FileEnvironment>> load_graphics);

} // namespace VideoCommon
