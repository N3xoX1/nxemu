// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <boost/container/static_vector.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <utility>

#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/cityhash.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/div_ceil.h"
#include "yuzu_common/fs/fs.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/polyfill_ranges.h"
#include "yuzu_shader_recompiler/environment.h"
#include "yuzu_shader_recompiler/exception.h"
#include "yuzu_video_core/engines/kepler_compute.h"
#include "yuzu_video_core/memory_manager.h"
#include "yuzu_video_core/shader_environment.h"
#include "yuzu_video_core/texture_cache/format_lookup_table.h"
#include "yuzu_video_core/textures/texture.h"

namespace VideoCommon {

constexpr std::array<char, 8> MAGIC_NUMBER{'y', 'u', 'z', 'u', 'c', 'a', 'c', 'h'};

constexpr size_t INST_SIZE = sizeof(u64);

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

static u64 MakeCbufKey(u32 index, u32 offset) {
    return (static_cast<u64>(index) << 32) | offset;
}

// Reserved keys outside the guest address space; they keep the cache layout unchanged.
constexpr u64 POOL_TEXTURES_KEY = (u64{UINT32_MAX} << 32) | (UINT32_MAX - 1);
constexpr u64 POOL_SAMPLERS_KEY = (u64{UINT32_MAX} << 32) | (UINT32_MAX - 2);
constexpr u32 POOL_LINKED_SAMPLERS_BIT = 1U << 31;

Shader::TexturePoolInfo GraphicsTexturePoolInfo(const Maxwell& regs) {
    // Handles hold a 20 bit texture index and a 12 bit sampler index.
    const u32 num_textures = std::min(regs.tex_header.limit + 1U, 1U << 20);
    const bool linked = regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding;
    return {
        .num_textures = num_textures,
        .num_samplers = linked ? num_textures : std::min(regs.tex_sampler.limit + 1U, 1U << 12),
        .linked_samplers = linked,
    };
}

static u32 CbufSizeQueryMask(const std::unordered_map<u64, u32>& values) {
    u32 mask{values.contains(POOL_TEXTURES_KEY) ? TEXTURE_POOL_SIZE_BIT : 0U};
    for (const auto& [key, value] : values) {
        const u32 bank = static_cast<u32>(key >> 32);
        if (static_cast<u32>(key) == UINT32_MAX && bank < 32) {
            mask |= 1U << bank;
        }
    }
    return mask;
}

u32 GenericEnvironment::CbufSizeMask() const noexcept {
    return CbufSizeQueryMask(cbuf_values);
}

u32 FileEnvironment::CbufSizeMask() const noexcept {
    return CbufSizeQueryMask(cbuf_values);
}

static Shader::TextureType ConvertTextureType(
    const std::optional<Tegra::Texture::TICEntry>& optional_entry) {
    if (!optional_entry) {
        return Shader::TextureType::Color2D;
    }
    const Tegra::Texture::TICEntry& entry{*optional_entry};
    switch (entry.texture_type) {
    case Tegra::Texture::TextureType::Texture1D:
        return Shader::TextureType::Color1D;
    case Tegra::Texture::TextureType::Texture2D:
    case Tegra::Texture::TextureType::Texture2DNoMipmap:
        return entry.normalized_coords ? Shader::TextureType::Color2D
                                       : Shader::TextureType::Color2DRect;
    case Tegra::Texture::TextureType::Texture3D:
        return Shader::TextureType::Color3D;
    case Tegra::Texture::TextureType::TextureCubemap:
        return Shader::TextureType::ColorCube;
    case Tegra::Texture::TextureType::Texture1DArray:
        return Shader::TextureType::ColorArray1D;
    case Tegra::Texture::TextureType::Texture2DArray:
        return Shader::TextureType::ColorArray2D;
    case Tegra::Texture::TextureType::Texture1DBuffer:
        return Shader::TextureType::Buffer;
    case Tegra::Texture::TextureType::TextureCubeArray:
        return Shader::TextureType::ColorArrayCube;
    default:
        UNIMPLEMENTED();
        return Shader::TextureType::Color2D;
    }
}

static Shader::TexturePixelFormat ConvertTexturePixelFormat(
    const std::optional<Tegra::Texture::TICEntry>& optional_entry) {
    if (!optional_entry) {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    const Tegra::Texture::TICEntry& entry{*optional_entry};
    return static_cast<Shader::TexturePixelFormat>(
        PixelFormatFromTextureInfo(entry.format, entry.r_type, entry.g_type, entry.b_type,
                                   entry.a_type, entry.srgb_conversion));
}

static std::string_view StageToPrefix(Shader::Stage stage) {
    switch (stage) {
    case Shader::Stage::VertexB:
        return "VB";
    case Shader::Stage::TessellationControl:
        return "TC";
    case Shader::Stage::TessellationEval:
        return "TE";
    case Shader::Stage::Geometry:
        return "GS";
    case Shader::Stage::Fragment:
        return "FS";
    case Shader::Stage::Compute:
        return "CS";
    case Shader::Stage::VertexA:
        return "VA";
    default:
        return "UK";
    }
}

static void DumpImpl(u64 pipeline_hash, u64 shader_hash, std::span<const u64> code,
                     [[maybe_unused]] u32 read_highest, [[maybe_unused]] u32 read_lowest,
                     u32 initial_offset, Shader::Stage stage) {
    const auto shader_dir{Common::FS::GetYuzuPath(Common::FS::YuzuPath::DumpDir)};
    const auto base_dir{shader_dir / "shaders"};
    if (!Common::FS::CreateDir(shader_dir) || !Common::FS::CreateDir(base_dir)) {
        LOG_ERROR(Common_Filesystem, "Failed to create shader dump directories");
        return;
    }
    const auto prefix = StageToPrefix(stage);
    const auto name{base_dir /
                    fmt::format("{:016x}_{}_{:016x}.ash", pipeline_hash, prefix, shader_hash)};
    std::fstream shader_file(name, std::ios::out | std::ios::binary);
    ASSERT(initial_offset % sizeof(u64) == 0);
    const size_t jump_index = initial_offset / sizeof(u64);
    const size_t code_size = code.size_bytes() - initial_offset;
    shader_file.write(reinterpret_cast<const char*>(&code[jump_index]), code_size);

    // + 1 instruction, due to the fact that we skip the final self branch instruction in the code,
    // but we need to consider it for padding, otherwise nvdisasm rages.
    const size_t padding_needed = (32 - ((code_size + INST_SIZE) % 32)) % 32;
    for (size_t i = 0; i < INST_SIZE + padding_needed; i++) {
        shader_file.put(0);
    }
}

GenericEnvironment::GenericEnvironment(Tegra::MemoryManager& gpu_memory_, GPUVAddr program_base_,
                                       u32 start_address_)
    : gpu_memory{&gpu_memory_}, program_base{program_base_} {
    start_address = start_address_;
}

GenericEnvironment::~GenericEnvironment() = default;

u32 GenericEnvironment::TextureBoundBuffer() const {
    return texture_bound;
}

u32 GenericEnvironment::LocalMemorySize() const {
    return local_memory_size;
}

u32 GenericEnvironment::SharedMemorySize() const {
    return shared_memory_size;
}

std::array<u32, 3> GenericEnvironment::WorkgroupSize() const {
    return workgroup_size;
}

u64 GenericEnvironment::ReadInstruction(u32 address) {
    read_lowest = std::min(read_lowest, address);
    read_highest = std::max(read_highest, address);

    if (address >= cached_lowest &&
        (address < cached_highest || (frozen && address == cached_highest))) {
        return Code()[(address - cached_lowest) / INST_SIZE];
    }
    if (frozen) {
        throw Shader::LogicError("Frozen shader environment requested uncached instruction 0x{:x}",
                                 address);
    }
    has_unbound_instructions = true;
    return gpu_memory->Read<u64>(program_base + address);
}

std::optional<u64> GenericEnvironment::Analyze() {
    const std::optional<u64> size{TryFindSize()};
    if (!size) {
        return std::nullopt;
    }
    cached_lowest = start_address;
    cached_highest = start_address + static_cast<u32>(*size);
    return Common::CityHash64(reinterpret_cast<const char*>(code.data()), *size);
}

void GenericEnvironment::SetCachedSize(size_t size_bytes) {
    cached_lowest = start_address;
    cached_highest = start_address + static_cast<u32>(size_bytes);
    code.resize(CachedSizeWords());
    gpu_memory->ReadBlock(program_base + cached_lowest, code.data(), code.size() * sizeof(u64));
}

void GenericEnvironment::SetCachedCode(std::shared_ptr<const std::vector<u64>> code_,
                                       size_t size_bytes) {
    cached_lowest = start_address;
    cached_highest = start_address + static_cast<u32>(size_bytes);
    shared_code = std::move(code_);
}

std::shared_ptr<const std::vector<u64>> GenericEnvironment::ShareCachedCode(size_t size_bytes) {
    // Preserve the same padding as SetCachedSize; subsequent pipeline variants
    // share the immutable bytes until ShaderCache invalidates this ShaderInfo.
    SetCachedSize(size_bytes);
    shared_code = std::make_shared<const std::vector<u64>>(std::move(code));
    return shared_code;
}

size_t GenericEnvironment::CachedSizeWords() const noexcept {
    return CachedSizeBytes() / INST_SIZE;
}

size_t GenericEnvironment::CachedSizeBytes() const noexcept {
    return static_cast<size_t>(cached_highest) - cached_lowest + INST_SIZE;
}

size_t GenericEnvironment::ReadSizeBytes() const noexcept {
    return read_highest - read_lowest + INST_SIZE;
}

bool GenericEnvironment::CanBeSerialized() const noexcept {
    return !has_unbound_instructions;
}

u64 GenericEnvironment::CalculateHash() const {
    const size_t size{ReadSizeBytes()};
    const auto data{std::make_unique<char[]>(size)};
    gpu_memory->ReadBlock(program_base + read_lowest, data.get(), size);
    return Common::CityHash64(data.get(), size);
}

void GenericEnvironment::Dump(u64 pipeline_hash, u64 shader_hash) {
    DumpImpl(pipeline_hash, shader_hash, Code(), read_highest, read_lowest, initial_offset, stage);
}

void GenericEnvironment::Serialize(std::ofstream& file) const {
    const u64 code_size{static_cast<u64>(CachedSizeBytes())};
    const u64 num_texture_types{static_cast<u64>(texture_types.size())};
    const u64 num_texture_pixel_formats{static_cast<u64>(texture_pixel_formats.size())};
    const u64 num_cbuf_values{static_cast<u64>(cbuf_values.size())};
    const u64 num_cbuf_replacement_values{static_cast<u64>(cbuf_replacements.size())};

    file.write(reinterpret_cast<const char*>(&code_size), sizeof(code_size))
        .write(reinterpret_cast<const char*>(&num_texture_types), sizeof(num_texture_types))
        .write(reinterpret_cast<const char*>(&num_texture_pixel_formats),
               sizeof(num_texture_pixel_formats))
        .write(reinterpret_cast<const char*>(&num_cbuf_values), sizeof(num_cbuf_values))
        .write(reinterpret_cast<const char*>(&num_cbuf_replacement_values),
               sizeof(num_cbuf_replacement_values))
        .write(reinterpret_cast<const char*>(&local_memory_size), sizeof(local_memory_size))
        .write(reinterpret_cast<const char*>(&texture_bound), sizeof(texture_bound))
        .write(reinterpret_cast<const char*>(&start_address), sizeof(start_address))
        .write(reinterpret_cast<const char*>(&cached_lowest), sizeof(cached_lowest))
        .write(reinterpret_cast<const char*>(&cached_highest), sizeof(cached_highest))
        .write(reinterpret_cast<const char*>(&viewport_transform_state),
               sizeof(viewport_transform_state))
        .write(reinterpret_cast<const char*>(&stage), sizeof(stage))
        .write(reinterpret_cast<const char*>(Code().data()), code_size);
    for (const auto& [key, type] : texture_types) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&type), sizeof(type));
    }
    for (const auto& [key, format] : texture_pixel_formats) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&format), sizeof(format));
    }
    for (const auto& [key, type] : cbuf_values) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&type), sizeof(type));
    }
    for (const auto& [key, type] : cbuf_replacements) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&type), sizeof(type));
    }
    if (stage == Shader::Stage::Compute) {
        file.write(reinterpret_cast<const char*>(&workgroup_size), sizeof(workgroup_size))
            .write(reinterpret_cast<const char*>(&shared_memory_size), sizeof(shared_memory_size));
    } else {
        file.write(reinterpret_cast<const char*>(&sph), sizeof(sph));
        if (stage == Shader::Stage::Geometry) {
            file.write(reinterpret_cast<const char*>(&gp_passthrough_mask),
                       sizeof(gp_passthrough_mask));
        }
    }
}

std::optional<u64> GenericEnvironment::TryFindSize() {
    static constexpr size_t BLOCK_SIZE = 0x1000;
    static constexpr size_t MAXIMUM_SIZE = 0x100000;

    static constexpr u64 SELF_BRANCH_A = 0xE2400FFFFF87000FULL;
    static constexpr u64 SELF_BRANCH_B = 0xE2400FFFFF07000FULL;

    GPUVAddr guest_addr{program_base + start_address};
    size_t offset{0};
    size_t size{BLOCK_SIZE};
    while (size <= MAXIMUM_SIZE) {
        code.resize(size / INST_SIZE);
        u64* const data = code.data() + offset / INST_SIZE;
        gpu_memory->ReadBlock(guest_addr, data, BLOCK_SIZE);
        for (size_t index = 0; index < BLOCK_SIZE; index += INST_SIZE) {
            const u64 inst = data[index / INST_SIZE];
            if (inst == SELF_BRANCH_A || inst == SELF_BRANCH_B) {
                return offset + index;
            }
        }
        guest_addr += BLOCK_SIZE;
        size += BLOCK_SIZE;
        offset += BLOCK_SIZE;
    }
    return std::nullopt;
}

std::optional<Tegra::Texture::TICEntry> GenericEnvironment::ReadTextureInfo(
    GPUVAddr tic_addr, u32 tic_limit, bool via_header_index, u32 raw) {
    const auto handle{Tegra::Texture::TexturePair(raw, via_header_index)};
    if (handle.first > tic_limit) {
        // Games leave unused sampler slots with an invalid handle, such as 0xFFFFFFFF
        LOG_DEBUG(HW_GPU, "Invalid texture handle=0x{:08x}", raw);
        return std::nullopt;
    }
    const GPUVAddr descriptor_addr{tic_addr + handle.first * sizeof(Tegra::Texture::TICEntry)};
    Tegra::Texture::TICEntry entry;
    gpu_memory->ReadBlock(descriptor_addr, &entry, sizeof(entry));
    return entry;
}

GraphicsEnvironment::GraphicsEnvironment(Tegra::Engines::Maxwell3D& maxwell3d_,
                                         Tegra::MemoryManager& gpu_memory_,
                                         Maxwell::ShaderType program, GPUVAddr program_base_,
                                         u32 start_address_,
                                         std::shared_ptr<const std::vector<u64>> cached_code,
                                         size_t cached_size)
    : GenericEnvironment{gpu_memory_, program_base_, start_address_}, maxwell3d{&maxwell3d_} {
    if (cached_code && cached_code->size() * sizeof(u64) >= sizeof(sph)) {
        // ShaderCache owns validated immutable bytecode. Re-reading its header
        // through ReadBlock could synchronize guest memory on every variant.
        std::memcpy(&sph, cached_code->data(), sizeof(sph));
        SetCachedCode(std::move(cached_code), cached_size);
    } else {
        gpu_memory->ReadBlock(program_base + start_address, &sph, sizeof(sph));
        if (cached_code) {
            SetCachedCode(std::move(cached_code), cached_size);
        }
    }
    initial_offset = sizeof(sph);
    gp_passthrough_mask = maxwell3d->regs.post_vtg_shader_attrib_skip_mask;
    switch (program) {
    case Maxwell::ShaderType::VertexA:
        stage = Shader::Stage::VertexA;
        stage_index = 0;
        break;
    case Maxwell::ShaderType::VertexB:
        stage = Shader::Stage::VertexB;
        stage_index = 0;
        break;
    case Maxwell::ShaderType::TessellationInit:
        stage = Shader::Stage::TessellationControl;
        stage_index = 1;
        break;
    case Maxwell::ShaderType::Tessellation:
        stage = Shader::Stage::TessellationEval;
        stage_index = 2;
        break;
    case Maxwell::ShaderType::Geometry:
        stage = Shader::Stage::Geometry;
        stage_index = 3;
        break;
    case Maxwell::ShaderType::Pixel:
        stage = Shader::Stage::Fragment;
        stage_index = 4;
        break;
    default:
        ASSERT_MSG(false, "Invalid program={}", program);
        break;
    }
    const u64 local_size{sph.LocalMemorySize()};
    ASSERT(local_size <= std::numeric_limits<u32>::max());
    local_memory_size = static_cast<u32>(local_size) + sph.common3.shader_local_memory_crs_size;
    texture_bound = maxwell3d->regs.bindless_texture_const_buffer_slot;
    is_proprietary_driver = texture_bound == 2;
    has_hle_engine_state =
        maxwell3d->engine_state == Tegra::Engines::Maxwell3D::EngineHint::OnHLEMacro;
}

u32 GraphicsEnvironment::ReadCbufSize(u32 cbuf_index) {
    const u32 size = snapshot
        ? (snapshot->constant_buffers[stage_index][cbuf_index].enabled
               ? snapshot->constant_buffers[stage_index][cbuf_index].size : 0)
        : (maxwell3d->state.shader_stages[stage_index].const_buffers[cbuf_index].enabled
               ? maxwell3d->state.shader_stages[stage_index].const_buffers[cbuf_index].size : 0);
    // Reserved offset outside the guest address space; keeps cache layout unchanged.
    cbuf_values.emplace(MakeCbufKey(cbuf_index, UINT32_MAX), size);
    return size;
}

namespace {
// Compares guest memory with a previous copy without allocating.
bool GuestMemoryEquals(Tegra::MemoryManager& gpu_memory, GPUVAddr address, const void* data,
                       size_t size) {
    std::array<u8, 4096> chunk;
    const u8* expected = static_cast<const u8*>(data);
    for (size_t offset = 0; offset < size; offset += chunk.size()) {
        const size_t chunk_size = std::min(chunk.size(), size - offset);
        gpu_memory.ReadBlockUnsafe(address + offset, chunk.data(), chunk_size);
        if (std::memcmp(chunk.data(), expected + offset, chunk_size) != 0) {
            return false;
        }
    }
    return true;
}
} // namespace

std::shared_ptr<const GraphicsEnvironment::Snapshot> GraphicsEnvironment::CaptureSnapshot(
    Tegra::Engines::Maxwell3D& maxwell3d, Tegra::MemoryManager& gpu_memory,
    u32 active_stage_mask, SnapshotCache& cache) {
    constexpr size_t MaxConstantBufferSnapshotBytes = 16 * 1024 * 1024;
    constexpr size_t MaxTextureSnapshotBytes = 16 * 1024 * 1024;
    Snapshot layout;
    struct Copy {
        GPUVAddr address;
        size_t size;
        size_t offset;
    };
    boost::container::static_vector<Copy, Maxwell::MaxShaderStage * Maxwell::MaxConstBuffers> copies;
    size_t total_cbuf_size{};
    for (size_t stage_index = 0; stage_index < maxwell3d.state.shader_stages.size(); ++stage_index) {
        if ((active_stage_mask & (1U << stage_index)) == 0) {
            continue;
        }
        const auto& stage = maxwell3d.state.shader_stages[stage_index];
        for (size_t index = 0; index < stage.const_buffers.size(); ++index) {
            const auto& cbuf = stage.const_buffers[index];
            auto& dest = layout.constant_buffers[stage_index][index];
            dest.enabled = cbuf.enabled;
            dest.size = cbuf.size;
            if (!cbuf.enabled || cbuf.size == 0) {
                continue;
            }
            const size_t size = static_cast<size_t>(cbuf.size) + sizeof(u32) - 1;
            // Games commonly bind the same constants to multiple shader stages.
            const auto it = std::ranges::find_if(copies, [&](const Copy& copy) {
                return copy.address == cbuf.address && copy.size >= size;
            });
            if (it != copies.end()) {
                dest.data_offset = it->offset;
                continue;
            }
            if (size > MaxConstantBufferSnapshotBytes - total_cbuf_size) {
                return {};
            }
            // The synchronous path reads constant buffers straight from guest
            // memory too (GraphicsEnvironment::ReadCbufValue), so a buffer the
            // host GPU has modified gives the same result here. Rejecting such
            // buffers sent most new pipelines back to synchronous translation.
            dest.data_offset = total_cbuf_size;
            copies.push_back({cbuf.address, size, total_cbuf_size});
            total_cbuf_size += size;
        }
    }
    const size_t texture_count = static_cast<size_t>(maxwell3d.regs.tex_header.limit) + 1;
    if (texture_count > MaxTextureSnapshotBytes / sizeof(Tegra::Texture::TICEntry)) {
        return {};
    }
    const size_t texture_bytes = texture_count * sizeof(Tegra::Texture::TICEntry);
    const GPUVAddr texture_address = maxwell3d.regs.tex_header.Address();
    const bool is_hle = maxwell3d.engine_state == Tegra::Engines::Maxwell3D::EngineHint::OnHLEMacro;
    const size_t replacement_bytes = is_hle ? maxwell3d.replace_table.size() * 64 : 0;
    auto result = cache.memory.TryMakeShared<Snapshot>(
        sizeof(Snapshot) + total_cbuf_size + replacement_bytes, std::move(layout));
    if (!result) {
        return {};
    }
    // The synchronous path reads descriptors with a flushing ReadBlock. Flush
    // once here instead of giving up on the asynchronous build.
    const bool textures_dirty = gpu_memory.IsMemoryDirty(texture_address, texture_bytes);
    result->constant_buffer_data.resize(total_cbuf_size);
    for (const auto& copy : copies) {
        gpu_memory.ReadBlockUnsafe(copy.address,
                                  result->constant_buffer_data.data() + copy.offset, copy.size);
    }

    const auto& regs = maxwell3d.regs;
    result->via_header_index =
        regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding;
    result->texture_pool = GraphicsTexturePoolInfo(regs);
    result->texture_limit = regs.tex_header.limit;
    result->viewport_transform_state = regs.viewport_scale_offset_enabled;

    if (textures_dirty) {
        cache.texture_descriptors.reset();
    }
    if (!textures_dirty && cache.texture_descriptors && cache.texture_address == texture_address &&
        cache.texture_descriptors->size() == texture_count &&
        GuestMemoryEquals(gpu_memory, texture_address, cache.texture_descriptors->data(),
                          texture_bytes)) {
        // New pipelines arrive in bursts with the same descriptor table; sharing
        // it keeps one copy alive instead of one per queued build.
        result->texture_descriptors = cache.texture_descriptors;
    } else {
        // Stop retaining a stale table before reserving its replacement. Jobs still
        // using it keep both its storage and its charge alive through shared ownership.
        cache.texture_descriptors.reset();
        auto descriptors = cache.memory.TryMakeShared<std::vector<Tegra::Texture::TICEntry>>(
            texture_bytes, texture_count);
        if (!descriptors) {
            return {};
        }
        if (textures_dirty) {
            gpu_memory.ReadBlock(texture_address, descriptors->data(), texture_bytes);
        } else {
            gpu_memory.ReadBlockUnsafe(texture_address, descriptors->data(), texture_bytes);
        }
        cache.texture_address = texture_address;
        cache.texture_descriptors = descriptors;
        result->texture_descriptors = std::move(descriptors);
    }

    if (is_hle) {
        result->cbuf_replacements.reserve(maxwell3d.replace_table.size());
        for (const auto& [key, value] : maxwell3d.replace_table) {
            const auto converted = [&] {
                using Type = Tegra::Engines::Maxwell3D::HLEReplacementAttributeType;
                switch (value) {
                case Type::BaseVertex:
                    return Shader::ReplaceConstant::BaseVertex;
                case Type::BaseInstance:
                    return Shader::ReplaceConstant::BaseInstance;
                case Type::DrawID:
                    return Shader::ReplaceConstant::DrawID;
                default:
                    UNREACHABLE();
                }
            }();
            result->cbuf_replacements.emplace(key, converted);
        }
    }
    return result;
}

void GraphicsEnvironment::SetSnapshot(std::shared_ptr<const Snapshot> snapshot_) {
    snapshot = std::move(snapshot_);
    frozen = snapshot != nullptr;
    if (frozen) {
        // Workers must never dereference live engine state or guest memory.
        maxwell3d = nullptr;
        gpu_memory = nullptr;
    }
}

void GraphicsEnvironment::ReleaseSnapshot() {
    // Only serialization is allowed after this point. Keep the environment frozen.
    snapshot.reset();
}

std::optional<Tegra::Texture::TICEntry> GraphicsEnvironment::ReadSnapshotTextureInfo(u32 raw) const {
    ASSERT(snapshot);
    const auto handle = Tegra::Texture::TexturePair(raw, snapshot->via_header_index);
    if (handle.first > snapshot->texture_limit || !snapshot->texture_descriptors ||
        handle.first >= snapshot->texture_descriptors->size()) {
        return std::nullopt;
    }
    return (*snapshot->texture_descriptors)[handle.first];
}

u32 GraphicsEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    u32 value{};
    if (snapshot) {
        if (stage_index >= snapshot->constant_buffers.size() ||
            cbuf_index >= snapshot->constant_buffers[stage_index].size()) {
            throw Shader::LogicError("Frozen shader requested invalid constant buffer {}", cbuf_index);
        }
        const auto& cbuf = snapshot->constant_buffers[stage_index][cbuf_index];
        if (!cbuf.enabled) {
            throw Shader::LogicError("Frozen shader requested disabled constant buffer {}", cbuf_index);
        }
        if (cbuf_offset < cbuf.size) {
            std::memcpy(&value,
                        snapshot->constant_buffer_data.data() + cbuf.data_offset + cbuf_offset,
                        sizeof(value));
        }
    } else {
        const auto& cbuf{maxwell3d->state.shader_stages[stage_index].const_buffers[cbuf_index]};
        ASSERT(cbuf.enabled);
        if (cbuf_offset < cbuf.size) {
            value = gpu_memory->Read<u32>(cbuf.address + cbuf_offset);
        }
    }
    cbuf_values.emplace(MakeCbufKey(cbuf_index, cbuf_offset), value);
    return value;
}

std::optional<Shader::TexturePoolInfo> GraphicsEnvironment::ReadTexturePoolInfo() {
    const Shader::TexturePoolInfo pool{snapshot ? snapshot->texture_pool
                                              : GraphicsTexturePoolInfo(maxwell3d->regs)};
    cbuf_values.emplace(POOL_TEXTURES_KEY, pool.num_textures);
    cbuf_values.emplace(POOL_SAMPLERS_KEY,
                        pool.num_samplers | (pool.linked_samplers ? POOL_LINKED_SAMPLERS_BIT : 0U));
    return pool;
}

std::optional<Shader::ReplaceConstant> GraphicsEnvironment::GetReplaceConstBuffer(u32 bank,
                                                                                  u32 offset) {
    if (!has_hle_engine_state) {
        return std::nullopt;
    }
    const u64 key = (static_cast<u64>(bank) << 32) | static_cast<u64>(offset);
    if (snapshot) {
        const auto it = snapshot->cbuf_replacements.find(key);
        if (it == snapshot->cbuf_replacements.end()) {
            return std::nullopt;
        }
        cbuf_replacements.emplace(key, it->second);
        return it->second;
    }
    auto it = maxwell3d->replace_table.find(key);
    if (it == maxwell3d->replace_table.end()) {
        return std::nullopt;
    }
    const auto converted_value = [](Tegra::Engines::Maxwell3D::HLEReplacementAttributeType name) {
        switch (name) {
        case Tegra::Engines::Maxwell3D::HLEReplacementAttributeType::BaseVertex:
            return Shader::ReplaceConstant::BaseVertex;
        case Tegra::Engines::Maxwell3D::HLEReplacementAttributeType::BaseInstance:
            return Shader::ReplaceConstant::BaseInstance;
        case Tegra::Engines::Maxwell3D::HLEReplacementAttributeType::DrawID:
            return Shader::ReplaceConstant::DrawID;
        default:
            UNREACHABLE();
        }
    }(it->second);
    cbuf_replacements.emplace(key, converted_value);
    return converted_value;
}

Shader::TextureType GraphicsEnvironment::ReadTextureType(u32 handle) {
    const auto entry = snapshot
        ? ReadSnapshotTextureInfo(handle)
        : ReadTextureInfo(maxwell3d->regs.tex_header.Address(), maxwell3d->regs.tex_header.limit,
                          maxwell3d->regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding,
                          handle);
    const Shader::TextureType result{ConvertTextureType(entry)};
    texture_types.emplace(handle, result);
    return result;
}

Shader::TexturePixelFormat GraphicsEnvironment::ReadTexturePixelFormat(u32 handle) {
    const auto entry = snapshot
        ? ReadSnapshotTextureInfo(handle)
        : ReadTextureInfo(maxwell3d->regs.tex_header.Address(), maxwell3d->regs.tex_header.limit,
                          maxwell3d->regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding,
                          handle);
    const Shader::TexturePixelFormat result(ConvertTexturePixelFormat(entry));
    texture_pixel_formats.emplace(handle, result);
    return result;
}

bool GraphicsEnvironment::IsTexturePixelFormatInteger(u32 handle) {
    return VideoCore::Surface::IsPixelFormatInteger(
        static_cast<VideoCore::Surface::PixelFormat>(ReadTexturePixelFormat(handle)));
}

u32 GraphicsEnvironment::ReadViewportTransformState() {
    viewport_transform_state =
        snapshot ? snapshot->viewport_transform_state : maxwell3d->regs.viewport_scale_offset_enabled;
    return viewport_transform_state;
}

ComputeEnvironment::ComputeEnvironment(Tegra::Engines::KeplerCompute& kepler_compute_,
                                       Tegra::MemoryManager& gpu_memory_, GPUVAddr program_base_,
                                       u32 start_address_)
    : GenericEnvironment{gpu_memory_, program_base_, start_address_}, kepler_compute{
                                                                          &kepler_compute_} {
    const auto& qmd{kepler_compute->launch_description};
    stage = Shader::Stage::Compute;
    local_memory_size = qmd.local_pos_alloc + qmd.local_crs_alloc;
    texture_bound = kepler_compute->regs.tex_cb_index;
    is_proprietary_driver = texture_bound == 2;
    shared_memory_size = qmd.shared_alloc;
    workgroup_size = {qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z};
}

u32 ComputeEnvironment::ReadCbufSize(u32 cbuf_index) {
    const auto& qmd{kepler_compute->launch_description};
    const bool enabled = ((qmd.const_buffer_enable_mask.Value() >> cbuf_index) & 1) != 0;
    const u32 size = enabled ? qmd.const_buffer_config[cbuf_index].size.Value() : 0;
    cbuf_values.emplace(MakeCbufKey(cbuf_index, UINT32_MAX), size);
    return size;
}

u32 ComputeEnvironment::ReadTextureTableSize() {
    const u32 size = std::min(kepler_compute->regs.tic.limit + 1U, 1U << 20);
    cbuf_values.emplace(MakeCbufKey(UINT32_MAX, UINT32_MAX), size);
    return size;
}

u32 FileEnvironment::ReadTextureTableSize() {
    const auto it = cbuf_values.find(MakeCbufKey(UINT32_MAX, UINT32_MAX));
    return it != cbuf_values.end() ? it->second : 0;
}

std::optional<Shader::TexturePoolInfo> FileEnvironment::ReadTexturePoolInfo() {
    const auto textures = cbuf_values.find(POOL_TEXTURES_KEY);
    const auto samplers = cbuf_values.find(POOL_SAMPLERS_KEY);
    if (textures == cbuf_values.end() || samplers == cbuf_values.end()) {
        return std::nullopt;
    }
    return Shader::TexturePoolInfo{
        .num_textures = textures->second,
        .num_samplers = samplers->second & ~POOL_LINKED_SAMPLERS_BIT,
        .linked_samplers = (samplers->second & POOL_LINKED_SAMPLERS_BIT) != 0,
    };
}

u32 ComputeEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    const auto& qmd{kepler_compute->launch_description};
    ASSERT(((qmd.const_buffer_enable_mask.Value() >> cbuf_index) & 1) != 0);
    const auto& cbuf{qmd.const_buffer_config[cbuf_index]};
    u32 value{};
    if (cbuf_offset < cbuf.size) {
        value = gpu_memory->Read<u32>(cbuf.Address() + cbuf_offset);
    }
    cbuf_values.emplace(MakeCbufKey(cbuf_index, cbuf_offset), value);
    return value;
}

Shader::TextureType ComputeEnvironment::ReadTextureType(u32 handle) {
    const auto& regs{kepler_compute->regs};
    const auto& qmd{kepler_compute->launch_description};
    auto entry = ReadTextureInfo(regs.tic.Address(), regs.tic.limit, qmd.linked_tsc != 0, handle);
    const Shader::TextureType result{ConvertTextureType(entry)};
    texture_types.emplace(handle, result);
    return result;
}

Shader::TexturePixelFormat ComputeEnvironment::ReadTexturePixelFormat(u32 handle) {
    const auto& regs{kepler_compute->regs};
    const auto& qmd{kepler_compute->launch_description};
    auto entry = ReadTextureInfo(regs.tic.Address(), regs.tic.limit, qmd.linked_tsc != 0, handle);
    const Shader::TexturePixelFormat result(ConvertTexturePixelFormat(entry));
    texture_pixel_formats.emplace(handle, result);
    return result;
}

bool ComputeEnvironment::IsTexturePixelFormatInteger(u32 handle) {
    return VideoCore::Surface::IsPixelFormatInteger(
        static_cast<VideoCore::Surface::PixelFormat>(ReadTexturePixelFormat(handle)));
}

u32 ComputeEnvironment::ReadViewportTransformState() {
    return viewport_transform_state;
}

void FileEnvironment::Deserialize(std::ifstream& file) {
    u64 code_size{};
    u64 num_texture_types{};
    u64 num_texture_pixel_formats{};
    u64 num_cbuf_values{};
    u64 num_cbuf_replacement_values{};
    file.read(reinterpret_cast<char*>(&code_size), sizeof(code_size))
        .read(reinterpret_cast<char*>(&num_texture_types), sizeof(num_texture_types))
        .read(reinterpret_cast<char*>(&num_texture_pixel_formats),
              sizeof(num_texture_pixel_formats))
        .read(reinterpret_cast<char*>(&num_cbuf_values), sizeof(num_cbuf_values))
        .read(reinterpret_cast<char*>(&num_cbuf_replacement_values),
              sizeof(num_cbuf_replacement_values))
        .read(reinterpret_cast<char*>(&local_memory_size), sizeof(local_memory_size))
        .read(reinterpret_cast<char*>(&texture_bound), sizeof(texture_bound))
        .read(reinterpret_cast<char*>(&start_address), sizeof(start_address))
        .read(reinterpret_cast<char*>(&read_lowest), sizeof(read_lowest))
        .read(reinterpret_cast<char*>(&read_highest), sizeof(read_highest))
        .read(reinterpret_cast<char*>(&viewport_transform_state), sizeof(viewport_transform_state))
        .read(reinterpret_cast<char*>(&stage), sizeof(stage));
    // A crash can leave the last cache entry incomplete. Validate serialized sizes before
    // allocating memory; otherwise arbitrary bytes can turn into a huge allocation request.
    if (!file || stage > Shader::Stage::VertexA || code_size % sizeof(u64) != 0) {
        throw std::ios_base::failure("Invalid shader cache environment header");
    }
    const auto position = file.tellg();
    file.seekg(0, std::ios::end);
    const auto end = file.tellg();
    file.seekg(position);
    if (!file || position < 0 || end < position) {
        throw std::ios_base::failure("Invalid shader cache file bounds");
    }
    u64 remaining = static_cast<u64>(end - position);
    if (code_size > remaining) {
        throw std::ios_base::failure("Invalid shader cache code size");
    }
    remaining -= code_size;
    const auto consume_entries = [&remaining](u64 count, size_t entry_size) {
        if (count > remaining / entry_size) {
            throw std::ios_base::failure("Invalid shader cache entry count");
        }
        remaining -= count * entry_size;
    };
    consume_entries(num_texture_types, sizeof(u32) + sizeof(Shader::TextureType));
    consume_entries(num_texture_pixel_formats, sizeof(u32) + sizeof(Shader::TexturePixelFormat));
    consume_entries(num_cbuf_values, sizeof(u64) + sizeof(u32));
    consume_entries(num_cbuf_replacement_values, sizeof(u64) + sizeof(Shader::ReplaceConstant));
    code.resize(Common::DivCeil(code_size, sizeof(u64)));
    file.read(reinterpret_cast<char*>(code.data()), code_size);
    for (size_t i = 0; i < num_texture_types; ++i) {
        u32 key;
        Shader::TextureType type;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&type), sizeof(type));
        texture_types.emplace(key, type);
    }
    for (size_t i = 0; i < num_texture_pixel_formats; ++i) {
        u32 key;
        Shader::TexturePixelFormat format;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&format), sizeof(format));
        texture_pixel_formats.emplace(key, format);
    }
    for (size_t i = 0; i < num_cbuf_values; ++i) {
        u64 key;
        u32 value;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&value), sizeof(value));
        cbuf_values.emplace(key, value);
    }
    for (size_t i = 0; i < num_cbuf_replacement_values; ++i) {
        u64 key;
        Shader::ReplaceConstant value;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&value), sizeof(value));
        cbuf_replacements.emplace(key, value);
    }
    if (stage == Shader::Stage::Compute) {
        file.read(reinterpret_cast<char*>(&workgroup_size), sizeof(workgroup_size))
            .read(reinterpret_cast<char*>(&shared_memory_size), sizeof(shared_memory_size));
        initial_offset = 0;
    } else {
        file.read(reinterpret_cast<char*>(&sph), sizeof(sph));
        initial_offset = sizeof(sph);
        if (stage == Shader::Stage::Geometry) {
            file.read(reinterpret_cast<char*>(&gp_passthrough_mask), sizeof(gp_passthrough_mask));
        }
    }
    if (!file) {
        throw std::ios_base::failure("Truncated shader cache environment");
    }
    is_proprietary_driver = texture_bound == 2;
}

void FileEnvironment::Dump(u64 pipeline_hash, u64 shader_hash) {
    DumpImpl(pipeline_hash, shader_hash, code, read_highest, read_lowest, initial_offset, stage);
}

u64 FileEnvironment::ReadInstruction(u32 address) {
    if (address < read_lowest || address > read_highest) {
        throw Shader::LogicError("Out of bounds address {}", address);
    }
    return code[(address - read_lowest) / sizeof(u64)];
}

u32 FileEnvironment::ReadCbufSize(u32 cbuf_index) {
    const auto it = cbuf_values.find(MakeCbufKey(cbuf_index, UINT32_MAX));
    return it != cbuf_values.end() ? it->second : 0;
}

u32 FileEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    const auto it{cbuf_values.find(MakeCbufKey(cbuf_index, cbuf_offset))};
    if (it == cbuf_values.end()) {
        throw Shader::LogicError("Uncached read texture type");
    }
    return it->second;
}

Shader::TextureType FileEnvironment::ReadTextureType(u32 handle) {
    const auto it{texture_types.find(handle)};
    if (it == texture_types.end()) {
        throw Shader::LogicError("Uncached read texture type");
    }
    return it->second;
}

Shader::TexturePixelFormat FileEnvironment::ReadTexturePixelFormat(u32 handle) {
    const auto it{texture_pixel_formats.find(handle)};
    if (it == texture_pixel_formats.end()) {
        throw Shader::LogicError("Uncached read texture pixel format");
    }
    return it->second;
}

bool FileEnvironment::IsTexturePixelFormatInteger(u32 handle) {
    return VideoCore::Surface::IsPixelFormatInteger(
        static_cast<VideoCore::Surface::PixelFormat>(ReadTexturePixelFormat(handle)));
}

u32 FileEnvironment::ReadViewportTransformState() {
    return viewport_transform_state;
}

u32 FileEnvironment::LocalMemorySize() const {
    return local_memory_size;
}

u32 FileEnvironment::SharedMemorySize() const {
    return shared_memory_size;
}

u32 FileEnvironment::TextureBoundBuffer() const {
    return texture_bound;
}

std::array<u32, 3> FileEnvironment::WorkgroupSize() const {
    return workgroup_size;
}

std::optional<Shader::ReplaceConstant> FileEnvironment::GetReplaceConstBuffer(u32 bank,
                                                                              u32 offset) {
    const u64 key = (static_cast<u64>(bank) << 32) | static_cast<u64>(offset);
    auto it = cbuf_replacements.find(key);
    if (it == cbuf_replacements.end()) {
        return std::nullopt;
    }
    return it->second;
}

void SerializePipeline(std::span<const char> key, std::span<const GenericEnvironment* const> envs,
                       const std::filesystem::path& filename, u32 cache_version) try {
    std::ofstream file(filename, std::ios::binary | std::ios::ate | std::ios::app);
    file.exceptions(std::ifstream::failbit);
    if (!file.is_open()) {
        LOG_ERROR(Common_Filesystem, "Failed to open pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
        return;
    }
    if (file.tellp() == 0) {
        // Write header
        file.write(MAGIC_NUMBER.data(), MAGIC_NUMBER.size())
            .write(reinterpret_cast<const char*>(&cache_version), sizeof(cache_version));
    }
    if (!std::ranges::all_of(envs, &GenericEnvironment::CanBeSerialized)) {
        return;
    }
    const u32 num_envs{static_cast<u32>(envs.size())};
    file.write(reinterpret_cast<const char*>(&num_envs), sizeof(num_envs));
    for (const GenericEnvironment* const env : envs) {
        env->Serialize(file);
    }
    file.write(key.data(), key.size_bytes());

} catch (const std::ios_base::failure& e) {
    LOG_ERROR(Common_Filesystem, "{}", e.what());
    if (!Common::FS::RemoveFile(filename)) {
        LOG_ERROR(Common_Filesystem, "Failed to delete pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
    }
}

namespace {
/// Rewrites a pipeline cache file with only the given byte ranges of it
void CompactPipelineCache(std::ifstream& file, const std::filesystem::path& filename,
                          std::span<const std::pair<std::streamoff, std::streamoff>> ranges) try {
    std::filesystem::path temp{filename};
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.exceptions(std::ofstream::failbit);
        std::vector<char> buffer;
        for (const auto& [begin, end] : ranges) {
            buffer.resize(static_cast<size_t>(end - begin));
            file.seekg(begin);
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        }
    }
    file.close();
    std::filesystem::rename(temp, filename);
} catch (const std::exception& e) {
    LOG_WARNING(Common_Filesystem, "Failed to compact pipeline cache file: {}", e.what());
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path{filename}.concat(".tmp"), ec);
}
} // Anonymous namespace

void LoadPipelines(
    std::stop_token stop_loading, const std::filesystem::path& filename, u32 expected_cache_version,
    Common::UniqueFunction<bool, std::ifstream&, FileEnvironment> load_compute,
    Common::UniqueFunction<bool, std::ifstream&, std::vector<FileEnvironment>> load_graphics) try {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return;
    }
    file.exceptions(std::ifstream::failbit);
    const auto end{file.tellg()};
    file.seekg(0, std::ios::beg);

    std::array<char, 8> magic_number;
    u32 cache_version;
    file.read(magic_number.data(), magic_number.size())
        .read(reinterpret_cast<char*>(&cache_version), sizeof(cache_version));
    if (magic_number != MAGIC_NUMBER || cache_version != expected_cache_version) {
        file.close();
        if (Common::FS::RemoveFile(filename)) {
            if (magic_number != MAGIC_NUMBER) {
                LOG_ERROR(Common_Filesystem, "Invalid pipeline cache file");
            }
            if (cache_version != expected_cache_version) {
                LOG_INFO(Common_Filesystem, "Deleting old pipeline cache");
            }
        } else {
            LOG_ERROR(Common_Filesystem,
                      "Invalid pipeline cache file and failed to delete it in \"{}\"",
                      Common::FS::PathToUTF8String(filename));
        }
        return;
    }
    // Byte ranges of the records to keep, the header included
    std::vector<std::pair<std::streamoff, std::streamoff>> kept{{0, file.tellg()}};
    bool dropped{};
    while (file.tellg() != end) {
        if (stop_loading.stop_requested()) {
            return;
        }
        const std::streamoff record_start{file.tellg()};
        u32 num_envs{};
        file.read(reinterpret_cast<char*>(&num_envs), sizeof(num_envs));
        if (num_envs == 0 || num_envs > 6) {
            throw std::ios_base::failure("Invalid shader cache stage count");
        }
        std::vector<FileEnvironment> envs(num_envs);
        for (FileEnvironment& env : envs) {
            env.Deserialize(file);
        }
        bool keep{};
        if (envs.front().ShaderStage() == Shader::Stage::Compute) {
            if (envs.size() != 1) {
                throw std::ios_base::failure("Invalid compute shader cache stage count");
            }
            keep = load_compute(file, std::move(envs.front()));
        } else {
            keep = load_graphics(file, std::move(envs));
        }
        const std::streamoff record_end{file.tellg()};
        if (!keep) {
            dropped = true;
        } else if (kept.back().second == record_start) {
            kept.back().second = record_end;
        } else {
            kept.emplace_back(record_start, record_end);
        }
    }
    if (dropped) {
        CompactPipelineCache(file, filename, kept);
    }

} catch (const std::ios_base::failure& e) {
    LOG_ERROR(Common_Filesystem, "{}", e.what());
    if (!Common::FS::RemoveFile(filename)) {
        LOG_ERROR(Common_Filesystem, "Failed to delete pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
    }
}

} // namespace VideoCommon
