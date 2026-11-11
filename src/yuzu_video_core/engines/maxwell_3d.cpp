// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_video_core/engines/maxwell_3d.h"
#include <tuple>
#include "nxemu-video/video_settings.h"
#include "yuzu_common/bit_util.h"
#include "yuzu_common/scope_exit.h"
#include "yuzu_common/settings.h"
#include "yuzu_common/yuzu_assert.h"
#include "yuzu_video_core/dirty_flags.h"
#include "yuzu_video_core/engines/draw_manager.h"
#include "yuzu_video_core/gpu.h"
#include "yuzu_video_core/gpu_thread.h"
#include "yuzu_video_core/memory_manager.h"
#include "yuzu_video_core/rasterizer_interface.h"
#include "yuzu_video_core/textures/texture.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace Tegra::Engines
{

/// First register id that is actually a Macro call.
constexpr u32 MacroRegistersStart = 0xE00;

/// Stream size at which the decode thread hands a block to the GPU thread
constexpr size_t PublishWords = 4096;

namespace
{
std::FILE * SplitVerifyFile()
{
    static std::FILE * const file = []() -> std::FILE * {
        const char * const path = std::getenv("NXEMU_GPU_SPLIT_VERIFY");
        if (path == nullptr || path[0] == '\0')
        {
            return nullptr;
        }
        return std::fopen(path, "w");
    }();
    return file;
}
} // namespace

Maxwell3D::Maxwell3D(MemoryManager & memory_manager_) :
    draw_manager(std::make_unique<DrawManager>(this)),
    memory_manager(memory_manager_),
    macro_engine(GetMacroEngine(*this)),
    upload_state(memory_manager, regs.upload)
{
    dirty.flags.flip();
    InitializePipelineKeyRegisters();
    InitializeRegisterDefaults();
    execution_mask.reset();
    for (size_t i = 0; i < execution_mask.size(); i++)
    {
        execution_mask[i] = IsMethodExecutable(static_cast<u32>(i));
    }
    method_sink.reserve(256);
    macro_params.reserve(64);
    macro_addresses.reserve(64);
    macro_segments.reserve(8);
    stream.reserve(1U << 16);
}

namespace
{
/// The engine that copies uniform buffers on the decode thread, see UniformPrefetchSubmission
std::atomic<Maxwell3D *> prefetch_owner{};
} // namespace

Maxwell3D::~Maxwell3D()
{
    Maxwell3D * self = this;
    prefetch_owner.compare_exchange_strong(self, nullptr);
}

void Maxwell3D::BindRasterizer(VideoCore::RasterizerInterface * rasterizer_)
{
    rasterizer = rasterizer_;
    upload_state.BindRasterizer(rasterizer_);
}

void Maxwell3D::InitializePipelineKeyRegisters()
{
    // The constant buffer upload registers only say where data and binds go: the data writes
    // and the binds change the generation themselves, when they change something.
    auto& kinds = dirty.generation_kinds;
    std::fill_n(kinds.begin() + offsetof(Regs, const_buffer) / sizeof(u32),
                sizeof(Regs::const_buffer) / sizeof(u32), u8{GEN_NONE});
    for (size_t i = 0; i < Regs::MaxShaderStage; ++i) {
        kinds[MAXWELL3D_REG_INDEX(bind_groups) + i * (sizeof(Regs::BindGroup) / sizeof(u32)) +
              offsetof(Regs::BindGroup, raw_config) / sizeof(u32)] = GEN_NONE;
    }
    // Rebasing a vertex stream leaves its layout: the stride and the enable are state
    for (size_t i = 0; i < Regs::NumVertexArrays; ++i) {
        const size_t stream =
            MAXWELL3D_REG_INDEX(vertex_streams) + i * (sizeof(Regs::VertexStream) / sizeof(u32));
        kinds[stream + offsetof(Regs::VertexStream, address_high) / sizeof(u32)] = GEN_BUFFERS;
        kinds[stream + offsetof(Regs::VertexStream, address_low) / sizeof(u32)] = GEN_BUFFERS;
        const size_t limit = MAXWELL3D_REG_INDEX(vertex_stream_limits) +
                             i * (sizeof(Regs::VertexStreamLimit) / sizeof(u32));
        kinds[limit + offsetof(Regs::VertexStreamLimit, address_high) / sizeof(u32)] = GEN_BUFFERS;
        kinds[limit + offsetof(Regs::VertexStreamLimit, address_low) / sizeof(u32)] = GEN_BUFFERS;
    }

    // The draw range and the draw triggers. The topology in draw.begin and in the instanced
    // array draws reaches the draw state, which the rasterizer compares itself. The rasterizer
    // binds the index buffer again for each draw, so its address and format are left out too.
    for (const size_t method : {
             MAXWELL3D_REG_INDEX(index_buffer.start_addr_high),
             MAXWELL3D_REG_INDEX(index_buffer.start_addr_low),
             MAXWELL3D_REG_INDEX(index_buffer.limit_addr_high),
             MAXWELL3D_REG_INDEX(index_buffer.limit_addr_low),
             MAXWELL3D_REG_INDEX(index_buffer.format),
             MAXWELL3D_REG_INDEX(vertex_buffer.first), MAXWELL3D_REG_INDEX(vertex_buffer.count),
             MAXWELL3D_REG_INDEX(index_buffer.first), MAXWELL3D_REG_INDEX(index_buffer.count),
             MAXWELL3D_REG_INDEX(index_buffer32_first), MAXWELL3D_REG_INDEX(index_buffer16_first),
             MAXWELL3D_REG_INDEX(index_buffer8_first),
             MAXWELL3D_REG_INDEX(index_buffer32_subsequent),
             MAXWELL3D_REG_INDEX(index_buffer16_subsequent),
             MAXWELL3D_REG_INDEX(index_buffer8_subsequent),
             MAXWELL3D_REG_INDEX(vertex_array_instance_first),
             MAXWELL3D_REG_INDEX(vertex_array_instance_subsequent),
             MAXWELL3D_REG_INDEX(global_base_vertex_index),
             MAXWELL3D_REG_INDEX(global_base_instance_index), MAXWELL3D_REG_INDEX(vertex_id_base),
             MAXWELL3D_REG_INDEX(draw.begin), MAXWELL3D_REG_INDEX(draw.end)}) {
        kinds[method] = GEN_NONE;
    }

    auto& table = dirty.pipeline_key_registers;
    table.fill(1);
    const auto clear = [&table](size_t offset, size_t size) {
        std::fill_n(table.begin() + offset / sizeof(u32), size / sizeof(u32), u8{0});
    };
    // Constant buffer sizes reach the key through ProcessCBBind
    clear(offsetof(Regs, const_buffer), sizeof(Regs::const_buffer));
    clear(offsetof(Regs, bind_groups), sizeof(Regs::bind_groups));
    for (size_t i = 0; i < Regs::NumVertexArrays; ++i) {
        const size_t stream = offsetof(Regs, vertex_streams) + i * sizeof(Regs::VertexStream);
        clear(stream + offsetof(Regs::VertexStream, address_high), 2 * sizeof(u32));
    }
    clear(offsetof(Regs, vertex_stream_limits), sizeof(Regs::vertex_stream_limits));
    clear(offsetof(Regs, index_buffer), offsetof(Regs::IndexBuffer, format));
    clear(offsetof(Regs, index_buffer) + offsetof(Regs::IndexBuffer, first), 2 * sizeof(u32));
    for (size_t i = 0; i < Regs::NumTransformFeedbackBuffers; ++i) {
        const size_t buffer = offsetof(Regs, transform_feedback) +
                              offsetof(Regs::TransformFeedback, buffers) +
                              i * sizeof(Regs::TransformFeedback::Buffer);
        clear(buffer + offsetof(Regs::TransformFeedback::Buffer, address_high), 4 * sizeof(u32));
    }
}

void Maxwell3D::InitializeRegisterDefaults()
{
    // Initializes registers to their default values - what games expect them to be at boot. This is
    // for certain registers that may not be explicitly set by games.

    // Reset all registers to zero
    std::memset(&regs, 0, sizeof(regs));

    // Depth range near/far is not always set, but is expected to be the default 0.0f, 1.0f. This is
    // needed for ARMS.
    for (auto & viewport : regs.viewports)
    {
        viewport.depth_range_near = 0.0f;
        viewport.depth_range_far = 1.0f;
    }
    for (auto & viewport : regs.viewport_transform)
    {
        viewport.swizzle.x.Assign(Regs::ViewportSwizzle::PositiveX);
        viewport.swizzle.y.Assign(Regs::ViewportSwizzle::PositiveY);
        viewport.swizzle.z.Assign(Regs::ViewportSwizzle::PositiveZ);
        viewport.swizzle.w.Assign(Regs::ViewportSwizzle::PositiveW);
    }

    // Doom and Bomberman seems to use the uninitialized registers and just enable blend
    // so initialize blend registers with sane values
    regs.blend.color_op = Regs::Blend::Equation::Add_D3D;
    regs.blend.color_source = Regs::Blend::Factor::One_D3D;
    regs.blend.color_dest = Regs::Blend::Factor::Zero_D3D;
    regs.blend.alpha_op = Regs::Blend::Equation::Add_D3D;
    regs.blend.alpha_source = Regs::Blend::Factor::One_D3D;
    regs.blend.alpha_dest = Regs::Blend::Factor::Zero_D3D;
    for (auto & blend : regs.blend_per_target)
    {
        blend.color_op = Regs::Blend::Equation::Add_D3D;
        blend.color_source = Regs::Blend::Factor::One_D3D;
        blend.color_dest = Regs::Blend::Factor::Zero_D3D;
        blend.alpha_op = Regs::Blend::Equation::Add_D3D;
        blend.alpha_source = Regs::Blend::Factor::One_D3D;
        blend.alpha_dest = Regs::Blend::Factor::Zero_D3D;
    }
    regs.stencil_front_op.fail = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_front_op.zfail = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_front_op.zpass = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_front_op.func = Regs::ComparisonOp::Always_GL;
    regs.stencil_front_func_mask = 0xFFFFFFFF;
    regs.stencil_front_mask = 0xFFFFFFFF;
    regs.stencil_two_side_enable = 1;
    regs.stencil_back_op.fail = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_back_op.zfail = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_back_op.zpass = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_back_op.func = Regs::ComparisonOp::Always_GL;
    regs.stencil_back_func_mask = 0xFFFFFFFF;
    regs.stencil_back_mask = 0xFFFFFFFF;

    regs.depth_test_func = Regs::ComparisonOp::Always_GL;
    regs.gl_front_face = Regs::FrontFace::CounterClockWise;
    regs.gl_cull_face = Regs::CullFace::Back;

    // TODO(Rodrigo): Most games do not set a point size. I think this is a case of a
    // register carrying a default value. Assume it's OpenGL's default (1).
    regs.point_size = 1.0f;

    // TODO(bunnei): Some games do not initialize the color masks (e.g. Sonic Mania). Assuming a
    // default of enabled fixes rendering here.
    for (auto & color_mask : regs.color_mask)
    {
        color_mask.R.Assign(1);
        color_mask.G.Assign(1);
        color_mask.B.Assign(1);
        color_mask.A.Assign(1);
    }

    for (auto & format : regs.vertex_attrib_format)
    {
        format.constant.Assign(1);
    }

    // NVN games expect these values to be enabled at boot
    regs.rasterize_enable = 1;
    regs.color_target_mrt_enable = 1;
    regs.framebuffer_srgb = 1;
    regs.line_width_aliased = 1.0f;
    regs.line_width_smooth = 1.0f;
    regs.gl_front_face = Maxwell3D::Regs::FrontFace::ClockWise;
    regs.polygon_mode_back = Maxwell3D::Regs::PolygonMode::Fill;
    regs.polygon_mode_front = Maxwell3D::Regs::PolygonMode::Fill;
    regs.render_enable.mode = Maxwell3D::Regs::RenderEnable::Mode::True;

    shadow_state = regs;
    front_regs = regs;
}

bool Maxwell3D::IsMethodExecutable(u32 method)
{
    if (method >= MacroRegistersStart)
    {
        return true;
    }
    switch (method)
    {
    case MAXWELL3D_REG_INDEX(draw.end):
    case MAXWELL3D_REG_INDEX(draw.begin):
    case MAXWELL3D_REG_INDEX(vertex_buffer.first):
    case MAXWELL3D_REG_INDEX(vertex_buffer.count):
    case MAXWELL3D_REG_INDEX(index_buffer.first):
    case MAXWELL3D_REG_INDEX(index_buffer.count):
    case MAXWELL3D_REG_INDEX(draw_inline_index):
    case MAXWELL3D_REG_INDEX(index_buffer32_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer16_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer8_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer32_first):
    case MAXWELL3D_REG_INDEX(index_buffer16_first):
    case MAXWELL3D_REG_INDEX(index_buffer8_first):
    case MAXWELL3D_REG_INDEX(inline_index_2x16.even):
    case MAXWELL3D_REG_INDEX(inline_index_4x8.index0):
    case MAXWELL3D_REG_INDEX(vertex_array_instance_first):
    case MAXWELL3D_REG_INDEX(vertex_array_instance_subsequent):
    case MAXWELL3D_REG_INDEX(draw_texture.src_y0):
    case MAXWELL3D_REG_INDEX(wait_for_idle):
    case MAXWELL3D_REG_INDEX(shadow_ram_control):
    case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
    case MAXWELL3D_REG_INDEX(load_mme.instruction):
    case MAXWELL3D_REG_INDEX(load_mme.start_address):
    case MAXWELL3D_REG_INDEX(falcon[4]):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 1:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 2:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 3:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 4:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 5:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 6:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 7:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 8:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 9:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 10:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 11:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 12:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 13:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 14:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 15:
    case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config):
    case MAXWELL3D_REG_INDEX(topology_override):
    case MAXWELL3D_REG_INDEX(clear_surface):
    case MAXWELL3D_REG_INDEX(report_semaphore.query):
    case MAXWELL3D_REG_INDEX(render_enable_override):
    case MAXWELL3D_REG_INDEX(render_enable.address_high):
    case MAXWELL3D_REG_INDEX(render_enable.address_low):
    case MAXWELL3D_REG_INDEX(render_enable.mode):
    case MAXWELL3D_REG_INDEX(clear_report_value):
    case MAXWELL3D_REG_INDEX(sync_info):
    case MAXWELL3D_REG_INDEX(launch_dma):
    case MAXWELL3D_REG_INDEX(inline_data):
    case MAXWELL3D_REG_INDEX(fragment_barrier):
    case MAXWELL3D_REG_INDEX(invalidate_texture_data_cache):
    case MAXWELL3D_REG_INDEX(tiled_cache_barrier):
        return true;
    default:
        return false;
    }
}

void Maxwell3D::ProcessMacro(u32 method, const u32 * base_start, u32 amount, bool is_last_call)
{
    if (executing_macro == 0)
    {
        // A macro call must begin by writing the macro method's register, not its argument.
        if ((method % 2) != 0) [[unlikely]]
        {
            ASSERT_MSG(false, "Can't start macro execution by writing to the ARGS register");
        }
        executing_macro = method;
    }

    macro_params.insert(macro_params.end(), base_start, base_start + amount);
    const size_t addr_begin = macro_addresses.size();
    macro_addresses.resize(addr_begin + amount);
    for (size_t i = 0; i < amount; i++)
    {
        macro_addresses[addr_begin + i] = current_dma_segment + i * sizeof(u32);
    }
    // A macro fallback can read GPU-written parameters at every accuracy level.
    macro_segments.emplace_back(current_dma_segment, amount);
    current_macro_dirty |= current_dirty;
    current_macro_payload |= current_payload;
    current_dirty = false;
    current_payload = false;

    // Call the macro when there are no more parameters in the command buffer
    if (is_last_call)
    {
        ConsumeSink();
        CallMacroMethod(executing_macro, macro_params);
        macro_params.clear();
        macro_addresses.clear();
        macro_segments.clear();
        current_macro_dirty = false;
        current_macro_payload = false;
    }
}

void Maxwell3D::RefreshParametersImpl(const std::vector<std::pair<GPUVAddr, size_t>> & segments,
                                      std::vector<u32> & params)
{
    size_t current_index = 0;
    for (auto & segment : segments)
    {
        if (segment.first == 0)
        {
            current_index += segment.second;
            continue;
        }
        memory_manager.ReadBlock(segment.first, &params[current_index],
                                 sizeof(u32) * segment.second);
        current_index += segment.second;
    }
}

u32 Maxwell3D::GetMaxCurrentVertices()
{
    u32 num_vertices = 0;
    for (size_t index = 0; index < Regs::NumVertexArrays; ++index)
    {
        const auto & array = regs.vertex_streams[index];
        if (array.enable == 0)
        {
            continue;
        }
        const auto & attribute = regs.vertex_attrib_format[index];
        if (attribute.constant)
        {
            num_vertices = std::max(num_vertices, 1U);
            continue;
        }
        const auto & limit = regs.vertex_stream_limits[index];
        const GPUVAddr gpu_addr_begin = array.Address();
        const GPUVAddr gpu_addr_end = limit.Address() + 1;
        const u32 address_size = static_cast<u32>(gpu_addr_end - gpu_addr_begin);
        num_vertices = std::max(
            num_vertices, address_size / std::max(attribute.SizeInBytes(), array.stride.Value()));
        break;
    }
    return num_vertices;
}

size_t Maxwell3D::IndexBufferLayoutSize()
{
    const GPUVAddr start_address = regs.index_buffer.StartAddress();
    const GPUVAddr end_address = regs.index_buffer.EndAddress();
    const size_t byte_size = regs.index_buffer.FormatSizeInBytes();
    const size_t address_size = static_cast<size_t>(end_address - start_address);
    // The pages of a sparse allocation are mapped one part at a time, the indices can be anywhere
    // in the allocation
    if (const auto sparse = memory_manager.GetSparseRegion(start_address)) {
        const size_t sparse_size = sparse->first + sparse->second - start_address;
        return std::min(sparse_size, address_size) / byte_size;
    }
    return std::min(memory_manager.GetMemoryLayoutSize(start_address), address_size) / byte_size;
}

size_t Maxwell3D::EstimateIndexBufferSize()
{
    GPUVAddr start_address = regs.index_buffer.StartAddress();
    GPUVAddr end_address = regs.index_buffer.EndAddress();
    static constexpr std::array<size_t, 3> max_sizes = {std::numeric_limits<u8>::max(),
                                                        std::numeric_limits<u16>::max(),
                                                        std::numeric_limits<u32>::max()};
    const size_t byte_size = regs.index_buffer.FormatSizeInBytes();
    const size_t log2_byte_size = Common::Log2Ceil64(byte_size);
    const size_t cap{GetMaxCurrentVertices() * 4 * byte_size};
    const size_t lower_cap =
        std::min<size_t>(static_cast<size_t>(end_address - start_address), cap);
    if (memory_manager.GetSparseRegion(start_address)) {
        return IndexBufferLayoutSize();
    }
    return std::min<size_t>(
        memory_manager.GetMemoryLayoutSize(start_address, byte_size * max_sizes[log2_byte_size]) /
            byte_size,
        lower_cap);
}

u32 Maxwell3D::ProcessShadowRam(u32 method, u32 argument)
{
    // Keep track of the register value in shadow_state when requested.
    const auto control = shadow_state.shadow_ram_control;
    if (control == Regs::ShadowRamControl::Track ||
        control == Regs::ShadowRamControl::TrackWithFilter)
    {
        shadow_state.reg_array[method] = argument;
        return argument;
    }
    if (control == Regs::ShadowRamControl::Replay)
    {
        return shadow_state.reg_array[method];
    }
    return argument;
}

void Maxwell3D::PushMethod(u32 method, u32 argument)
{
    FrontWrite(method, ProcessShadowRam(method, argument));
}

void Maxwell3D::PushMethodRun(u32 method, std::span<const CommandHeader> arguments)
{
    // Shadow mode changes are executable methods: the pusher ends the run before them.
    const auto control = shadow_state.shadow_ram_control;
    if (control == Regs::ShadowRamControl::Replay)
    {
        for (size_t i = 0; i < arguments.size(); ++i, ++method)
        {
            FrontWrite(method, shadow_state.reg_array[method]);
        }
        return;
    }
    if (control == Regs::ShadowRamControl::Track ||
        control == Regs::ShadowRamControl::TrackWithFilter)
    {
        for (const auto& argument : arguments)
        {
            shadow_state.reg_array[method] = argument.argument;
            FrontWrite(method++, argument.argument);
        }
        return;
    }
    // One stream entry for the whole run
    const u32 count = static_cast<u32>(arguments.size());
    if (count == 0)
    {
        return;
    }
    if (skip_same_writes)
    {
        // Only the changed values go into the stream, as shorter runs
        for (const auto& argument : arguments)
        {
            FrontWrite(method++, argument.argument);
        }
        return;
    }
    StreamHeader(StreamRegs, count);
    stream.push_back(method);
    const size_t base = stream.size();
    stream.resize(base + count);
    u32 * const out = stream.data() + base;
    for (u32 i = 0; i < count; ++i)
    {
        const u32 value = arguments[i].argument;
        front_regs.reg_array[method + i] = value;
        out[i] = value;
    }
    stream_run_pos = base - 2;
    stream_run_next = method + count;
    stream_run_open = true;
}

void Maxwell3D::ConsumeSinkImpl()
{
    SCOPE_EXIT
    {
        method_sink.clear();
    };
    const auto control = shadow_state.shadow_ram_control;
    if (control == Regs::ShadowRamControl::Track ||
        control == Regs::ShadowRamControl::TrackWithFilter)
    {

        for (auto [method, value] : method_sink)
        {
            shadow_state.reg_array[method] = value;
            FrontWrite(method, value);
        }
        return;
    }
    if (control == Regs::ShadowRamControl::Replay)
    {
        for (auto [method, value] : method_sink)
        {
            FrontWrite(method, shadow_state.reg_array[method]);
        }
        return;
    }
    for (auto [method, value] : method_sink)
    {
        FrontWrite(method, value);
    }
}

void Maxwell3D::ProcessDirtyRegisters(u32 method, u32 argument)
{
    const u32 old_value = regs.reg_array[method];
    if (old_value == argument)
    {
        return;
    }
    regs.reg_array[method] = argument;
    ++generations[dirty.generation_kinds[method]];
    dirty.MarkShaderProfileRegister(method, old_value, argument);
    dirty.pipeline_key_changed |= dirty.pipeline_key_registers[method] != 0;

    static_assert(std::tuple_size_v<DirtyState::Tables> == 2);
    const auto first = dirty.tables[0][method];
    const auto second = dirty.tables[1][method];
    if (first < 64 && second < 64) {
        dirty.flags |= DirtyState::Flags{(u64{1} << first) | (u64{1} << second)};
    } else {
        dirty.flags[first] = true;
        if (first != second) {
            dirty.flags[second] = true;
        }
    }
}

void Maxwell3D::ProcessMethodCall(u32 method, u32 argument, u32 nonshadow_argument)
{
    // Methods with side effects (barriers, queries, uploads, constant buffer binds) can change
    // what the next draw reads even when the value is the same
    ++generations[dirty.generation_kinds[method]];
    switch (method)
    {
    case MAXWELL3D_REG_INDEX(wait_for_idle):
        return rasterizer->WaitForIdle();
    // The decode side runs these, see FrontMethodCall
    case MAXWELL3D_REG_INDEX(shadow_ram_control):
    case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
    case MAXWELL3D_REG_INDEX(load_mme.instruction):
        return;
    case MAXWELL3D_REG_INDEX(load_mme.start_address):
        // Keep the pointer equal to the decode side one
        regs.load_mme.start_address_ptr++;
        return;
    case MAXWELL3D_REG_INDEX(falcon[4]):
        return ProcessFirmwareCall4();
    case MAXWELL3D_REG_INDEX(const_buffer.buffer):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 1:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 2:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 3:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 4:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 5:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 6:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 7:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 8:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 9:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 10:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 11:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 12:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 13:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 14:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 15:
        return ProcessCBData(argument);
    case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
        return ProcessCBBind(0);
    case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
        return ProcessCBBind(1);
    case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
        return ProcessCBBind(2);
    case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
        return ProcessCBBind(3);
    case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config):
        return ProcessCBBind(4);
    case MAXWELL3D_REG_INDEX(report_semaphore.query):
        return ProcessQueryGet();
    case MAXWELL3D_REG_INDEX(render_enable.address_high):
    case MAXWELL3D_REG_INDEX(render_enable.address_low):
        if (regs.render_enable_override == Regs::RenderEnable::Override::UseRenderEnable &&
            regs.render_enable.mode != Regs::RenderEnable::Mode::True &&
            regs.render_enable.mode != Regs::RenderEnable::Mode::False)
        {
            ProcessQueryCondition();
        }
        return;
    case MAXWELL3D_REG_INDEX(render_enable_override):
    case MAXWELL3D_REG_INDEX(render_enable.mode):
        return ProcessQueryCondition();
    case MAXWELL3D_REG_INDEX(clear_report_value):
        return ProcessCounterReset();
    case MAXWELL3D_REG_INDEX(sync_info):
        return ProcessSyncPoint();
    case MAXWELL3D_REG_INDEX(launch_dma):
        return upload_state.ProcessExec(regs.launch_dma.memory_layout.Value() ==
                                        Regs::LaunchDMA::Layout::Pitch);
    case MAXWELL3D_REG_INDEX(inline_data):
        upload_state.ProcessData(argument);
        return;
    case MAXWELL3D_REG_INDEX(fragment_barrier):
        return rasterizer->FragmentBarrier();
    case MAXWELL3D_REG_INDEX(invalidate_texture_data_cache):
        rasterizer->InvalidateGPUCache();
        return rasterizer->WaitForIdle();
    case MAXWELL3D_REG_INDEX(tiled_cache_barrier):
        return rasterizer->TiledCacheBarrier();
    default:
        draw_manager->ProcessMethodCall(method, argument);
        break;
    }
}

void Maxwell3D::CallMacroMethod(u32 method, const std::vector<u32> & parameters)
{
    // Reset the current macro.
    executing_macro = 0;

    // Lookup the macro offset
    const u32 entry =
        ((method - MacroRegistersStart) >> 1) % static_cast<u32>(macro_positions.size());

    // Execute the current macro.
    macro_engine->Execute(macro_positions[entry], parameters);

    // The execute side runs the deferred instanced draw
    StreamHeader(StreamDeferred, 0);
}

void Maxwell3D::CallMethod(u32 method, u32 method_argument, bool is_last_call)
{
    // It is an error to write to a register other than the current macro's ARG register before
    // it has finished execution.
    // ASSERT calls a non-inlined lambda; keep it off this per-method path when the check passes.
    if (executing_macro != 0 && method != executing_macro + 1) [[unlikely]]
    {
        ASSERT(method == executing_macro + 1);
    }

    // Methods after 0xE00 are special, they're actually triggers for some microcode that was
    // uploaded to the GPU during initialization.
    if (method >= MacroRegistersStart)
    {
        ProcessMacro(method, &method_argument, 1, is_last_call);
        return;
    }

    static_assert(Regs::NUM_REGS == MacroRegistersStart,
                  "Every method below the macro range must be a Maxwell3D register");

    const u32 argument = ProcessShadowRam(method, method_argument);
    if (!execution_mask[method])
    {
        // No side effect: a register write
        FrontWrite(method, argument);
        return;
    }
    front_regs.reg_array[method] = argument;
    StreamHeader(StreamCall, is_last_call ? 1 : 0);
    stream.push_back(method);
    stream.push_back(argument);
    stream.push_back(method_argument);
    FrontMethodCall(method, argument, method_argument);
    // Hand work to the GPU thread regularly so it runs while this side decodes
    if (stream.size() >= PublishWords && IsThreaded())
    {
        Publish();
    }
}

void Maxwell3D::FrontMethodCall(u32 method, u32 argument, u32 nonshadow_argument)
{
    switch (method)
    {
    case MAXWELL3D_REG_INDEX(shadow_ram_control):
        shadow_state.shadow_ram_control = static_cast<Regs::ShadowRamControl>(nonshadow_argument);
        return;
    case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
        // Queued HLE calls point to the cached macros that ClearCode deletes
        SyncBack();
        return macro_engine->ClearCode(front_regs.load_mme.instruction_ptr);
    case MAXWELL3D_REG_INDEX(load_mme.instruction):
        return macro_engine->AddCode(front_regs.load_mme.instruction_ptr, argument);
    case MAXWELL3D_REG_INDEX(load_mme.start_address):
        return ProcessMacroBind(argument);
    case MAXWELL3D_REG_INDEX(falcon[4]):
        front_regs.shadow_scratch[0] = 1;
        return;
    case MAXWELL3D_REG_INDEX(const_buffer.buffer):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 1:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 2:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 3:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 4:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 5:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 6:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 7:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 8:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 9:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 10:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 11:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 12:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 13:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 14:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 15:
        front_regs.const_buffer.offset += static_cast<u32>(sizeof(u32));
        stream_writes_memory = true;
        return;
    case MAXWELL3D_REG_INDEX(report_semaphore.query):
    case MAXWELL3D_REG_INDEX(launch_dma):
    case MAXWELL3D_REG_INDEX(inline_data):
        stream_writes_memory = true;
        return;
    case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
        return FrontBindUniform(0);
    case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
        return FrontBindUniform(1);
    case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
        return FrontBindUniform(2);
    case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
        return FrontBindUniform(3);
    case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config):
        return FrontBindUniform(4);
    default:
        return;
    }
}

void Maxwell3D::FrontBindUniform(size_t stage)
{
    const auto & group = front_regs.bind_groups[stage];
    const u32 index = group.shader_slot;
    if (index >= front_uniforms[stage].size())
    {
        return;
    }
    FrontUniform & uniform = front_uniforms[stage][index];
    if (group.valid == 0)
    {
        uniform = {};
        return;
    }
    uniform = {front_regs.const_buffer.Address(), front_regs.const_buffer.size};
    FrontPrefetchUniform(stage, index);
}

void Maxwell3D::FrontPrefetchUniform(size_t stage, u32 index)
{
    if (!prefetch_enabled || prefetch_blocked)
    {
        return;
    }
    const FrontUniform & uniform = front_uniforms[stage][index];
    if (uniform.size == 0)
    {
        return;
    }
    const std::optional<DAddr> device_addr = memory_manager.GpuToCpuAddress(uniform.address);
    if (!device_addr)
    {
        return;
    }
    VideoCore::RasterizerInterface::UniformPrefetch prefetch{};
    if (!rasterizer->PrefetchUniform(stage, index, *device_addr, uniform.size, prefetch))
    {
        return;
    }
    StreamHeader(StreamPrefetch, static_cast<u32>(stage) | (index << 3));
    stream.push_back(static_cast<u32>(prefetch.device_addr));
    stream.push_back(static_cast<u32>(prefetch.device_addr >> 32));
    stream.push_back(prefetch.size);
    stream.push_back(prefetch.offset);
}

void Maxwell3D::FrontPrefetchReset()
{
    const u64 position = rasterizer->UniformPrefetchPosition();
    StreamHeader(StreamPrefetchReset, 0);
    stream.push_back(static_cast<u32>(position));
    stream.push_back(static_cast<u32>(position >> 32));
}

void Maxwell3D::UniformPrefetchSubmission()
{
    if (!prefetch_enabled)
    {
        if (rasterizer == nullptr || !IsThreaded())
        {
            return;
        }
        Maxwell3D * owner = nullptr;
        if (!prefetch_owner.compare_exchange_strong(owner, this) && owner != this)
        {
            return;
        }
        prefetch_enabled = true;
    }
    // The copies of the previous command lists may be stale now
    FrontPrefetchReset();
    prefetch_blocked = false;
    for (size_t stage = 0; stage < front_uniforms.size(); ++stage)
    {
        for (u32 index = 0; index < front_uniforms[stage].size(); ++index)
        {
            FrontPrefetchUniform(stage, index);
        }
    }
}

void Maxwell3D::UniformPrefetchBarrier()
{
    if (!prefetch_enabled || prefetch_blocked)
    {
        return;
    }
    FrontPrefetchReset();
    prefetch_blocked = true;
}

void Maxwell3D::FrontHLEMacro(CachedMacro * program, const std::vector<u32> & parameters,
                              u32 method)
{
    if (hle_count == hle_calls.size())
    {
        hle_calls.emplace_back();
    }
    HLECall & call = hle_calls[hle_count];
    call.program = program;
    call.method = method;
    call.params.assign(parameters.begin(), parameters.end());
    call.segments.assign(macro_segments.begin(), macro_segments.end());
    call.addresses.assign(macro_addresses.begin(), macro_addresses.end());
    call.dirty = current_macro_dirty;
    call.recheck = current_macro_payload && IsThreaded();
    StreamHeader(StreamHLE, static_cast<u32>(hle_count));
    ++hle_count;
    stream_writes_memory = true;

    const bool threaded = IsThreaded();
    if ((threaded ? current_macro_payload : current_macro_dirty) &&
        program->FrontEffectsUseParameters())
    {
        // The register effects need the parameters the GPU wrote
        Publish(false);
        std::vector<u32> refreshed(parameters.begin(), parameters.end());
        bool dirty = current_macro_dirty;
        back_queue->RunExclusive([&]() {
            if (threaded)
            {
                dirty = AnySegmentDirty(macro_segments) || dirty;
            }
            RefreshParametersImpl(macro_segments, refreshed);
        });
        program->FrontEffects(refreshed, dirty);
        return;
    }
    program->FrontEffects(parameters, current_macro_dirty);
}

bool Maxwell3D::AnySegmentDirty(const std::vector<std::pair<GPUVAddr, size_t>> & segments) const
{
    for (const auto & [address, count] : segments)
    {
        if (address != 0 && memory_manager.IsMemoryDirty(address, count * sizeof(u32)))
        {
            return true;
        }
    }
    return false;
}

void Maxwell3D::FrontRefreshParameters()
{
    if (!IsThreaded())
    {
        if (!current_macro_dirty)
        {
            return;
        }
        DrainStream();
        RefreshParametersImpl(macro_segments, macro_params);
        return;
    }
    if (!current_macro_payload)
    {
        return;
    }
    // The parameters come from their own GP entry: the queued work can write them or make
    // them GPU written. The macro runs here, so wait for the GPU thread.
    Publish();
    back_queue->RunExclusive([this]() {
        current_macro_dirty = current_macro_dirty || AnySegmentDirty(macro_segments);
        RefreshParametersImpl(macro_segments, macro_params);
    });
}

bool Maxwell3D::IsThreaded() const
{
    return back_queue != nullptr && back_queue->IsThreaded();
}

void Maxwell3D::SyncBack()
{
    if (!IsThreaded())
    {
        DrainStream();
        return;
    }
    Publish();
    back_queue->WaitIdle();
}

void Maxwell3D::Publish(bool verify)
{
    if (!IsThreaded())
    {
        DrainStream(verify);
        return;
    }
    if (stream.empty())
    {
        return;
    }
    StreamBlock * block = nullptr;
    {
        std::scoped_lock lock{block_mutex};
        if (!free_blocks.empty())
        {
            block = free_blocks.back();
            free_blocks.pop_back();
        }
    }
    if (block == nullptr)
    {
        block_storage.push_back(std::make_unique<StreamBlock>());
        block = block_storage.back().get();
        block->words.reserve(stream.capacity());
    }
    std::swap(block->words, stream);
    std::swap(block->hle, hle_calls);
    block->hle_count = hle_count;
    stream.clear();
    hle_count = 0;
    stream_run_open = false;
    stream_writes_memory = false;
    if (verify && SplitVerifyFile() != nullptr) [[unlikely]]
    {
        if (!block->front)
        {
            block->front = std::make_unique<Regs>();
        }
        *block->front = front_regs;
    }
    else if (block->front)
    {
        block->front.reset();
    }
    back_queue->Push([this, block]() { RunBlock(block); });
}

void Maxwell3D::RunBlock(StreamBlock * block)
{
    RunWords(block->words.data(), block->words.size(), block->hle);
    ++stream_drains;
    if (block->front) [[unlikely]]
    {
        VerifyAgainst(*block->front);
    }
    std::scoped_lock lock{block_mutex};
    free_blocks.push_back(block);
}

void Maxwell3D::BackCallMethod(u32 method, u32 argument, bool is_last_call)
{
    ProcessDirtyRegisters(method, argument);
    ProcessMethodCall(method, argument, argument);
}

void Maxwell3D::BackCallMultiMethod(u32 method, const u32 * base_start, u32 amount)
{
    if (method == MAXWELL3D_REG_INDEX(inline_data))
    {
        upload_state.ProcessData(base_start, amount);
        return;
    }
    for (u32 i = 0; i < amount; i++)
    {
        BackCallMethod(method, base_start[i], i + 1 == amount);
    }
}


void Maxwell3D::DrainStream(bool verify)
{
    if (stream.empty() || stream_draining)
    {
        return;
    }
    stream_draining = true;
    stream_run_open = false;
    RunWords(stream.data(), stream.size(), hle_calls);
    stream.clear();
    hle_count = 0;
    stream_writes_memory = false;
    stream_draining = false;
    ++stream_drains;
    if (verify && SplitVerifyFile() != nullptr) [[unlikely]]
    {
        VerifyRegisters();
    }
}

void Maxwell3D::RunWords(const u32 * words, size_t size, std::vector<HLECall> & calls)
{
    size_t pos = 0;
    while (pos < size)
    {
        const u32 header = words[pos++];
        const u32 count = header >> 8;
        switch (static_cast<StreamKind>(header & 0xFF))
        {
        case StreamRegs:
        {
            const u32 method = words[pos++];
            for (u32 i = 0; i < count; ++i)
            {
                ProcessDirtyRegisters(method + i, words[pos + i]);
            }
            pos += count;
            break;
        }
        case StreamCall:
        {
            const u32 method = words[pos];
            const u32 argument = words[pos + 1];
            const u32 nonshadow_argument = words[pos + 2];
            pos += 3;
            ProcessDirtyRegisters(method, argument);
            ProcessMethodCall(method, argument, nonshadow_argument);
            break;
        }
        case StreamCBData:
            ProcessCBMultiData(words + pos, count);
            pos += count;
            break;
        case StreamInline:
            ++generations[GEN_STATE];
            upload_state.ProcessData(words + pos, count);
            pos += count;
            break;
        case StreamHLE:
        {
            // Draw macros only change the draw range, other macros may write any state
            ++generations[calls[count].program->OnlyDraws() ? GEN_NONE : GEN_STATE];
            // The dirty state was taken when the parameters were fetched. DmaPusher drains the
            // stream before that, so queued draws can't make them GPU written later.
            HLECall & call = calls[count];
            if (call.recheck)
            {
                // The decode thread fetched the parameters before the queued work ran
                call.dirty = call.dirty || AnySegmentDirty(call.segments);
                RefreshParametersImpl(call.segments, call.params);
            }
            back_hle = &call;
            call.program->Execute(call.params, call.method);
            back_hle = nullptr;
            break;
        }
        case StreamDeferred:
            draw_manager->DrawDeferred();
            break;
        case StreamPrefetch:
        {
            ++generations[GEN_BUFFERS];
            buffer_changes.rebound_slots[count & 7] |= 1U << (count >> 3);
            const VideoCore::RasterizerInterface::UniformPrefetch prefetch{
                .device_addr = static_cast<DAddr>(words[pos]) | (static_cast<DAddr>(words[pos + 1]) << 32),
                .size = words[pos + 2],
                .offset = words[pos + 3],
            };
            pos += 4;
            rasterizer->ApplyUniformPrefetch(count & 7, count >> 3, prefetch);
            break;
        }
        case StreamPrefetchReset:
            ++generations[GEN_STATE];
            rasterizer->ResetUniformPrefetch(static_cast<u64>(words[pos]) | (static_cast<u64>(words[pos + 1]) << 32));
            pos += 2;
            break;
        default:
            ASSERT_MSG(false, "Bad Maxwell3D stream entry");
            pos = size;
            break;
        }
    }
}

void Maxwell3D::VerifyRegisters()
{
    VerifyAgainst(front_regs);
}

void Maxwell3D::VerifyAgainst(const Regs & front_file)
{
    // Reports each register the first time the two sides differ after a drain
    static std::bitset<Regs::NUM_REGS> reported;
    static u32 total = 0;
    std::FILE * const file = SplitVerifyFile();
    for (u32 method = 0; method < Regs::NUM_REGS; ++method)
    {
        const u32 front = front_file.reg_array[method];
        const u32 back = regs.reg_array[method];
        if (front == back || reported[method] || total >= 4096)
        {
            continue;
        }
        reported[method] = true;
        ++total;
        std::fprintf(file, "drain=%llu reg=0x%03X front=0x%08X back=0x%08X\n",
                     static_cast<unsigned long long>(stream_drains), method, front, back);
        std::fflush(file);
    }
}

void Maxwell3D::CallMultiMethod(u32 method, const u32 * base_start, u32 amount,
                                u32 methods_pending)
{
    // Methods after 0xE00 are special, they're actually triggers for some microcode that was
    // uploaded to the GPU during initialization.
    if (method >= MacroRegistersStart)
    {
        ProcessMacro(method, base_start, amount, amount == methods_pending);
        return;
    }
    switch (method)
    {
    case MAXWELL3D_REG_INDEX(const_buffer.buffer):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 1:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 2:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 3:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 4:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 5:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 6:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 7:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 8:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 9:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 10:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 11:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 12:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 13:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 14:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 15:
        front_regs.const_buffer.offset += amount * static_cast<u32>(sizeof(u32));
        StreamHeader(StreamCBData, amount);
        stream.insert(stream.end(), base_start, base_start + amount);
        stream_writes_memory = true;
        break;
    case MAXWELL3D_REG_INDEX(inline_data):
        StreamHeader(StreamInline, amount);
        stream.insert(stream.end(), base_start, base_start + amount);
        stream_writes_memory = true;
        return;
    default:
        for (u32 i = 0; i < amount; i++)
        {
            CallMethod(method, base_start[i], methods_pending - i <= 1);
        }
        break;
    }
}

void Maxwell3D::ProcessMacroUpload(u32 data)
{
    macro_engine->AddCode(regs.load_mme.instruction_ptr++, data);
}

void Maxwell3D::ProcessMacroBind(u32 data)
{
    macro_positions[front_regs.load_mme.start_address_ptr++] = data;
}

void Maxwell3D::ProcessFirmwareCall4()
{
    LOG_DEBUG(HW_GPU, "(STUBBED) called");

    // Firmware call 4 is a blob that changes some registers depending on its parameters.
    // These registers don't affect emulation and so are stubbed by setting 0xd00 to 1.
    regs.shadow_scratch[0] = 1;
}

void Maxwell3D::ProcessQueryGet()
{
    VideoCommon::QueryPropertiesFlags flags{};
    if (regs.report_semaphore.query.short_query == 0)
    {
        flags |= VideoCommon::QueryPropertiesFlags::HasTimeout;
    }
    const GPUVAddr sequence_address{regs.report_semaphore.Address()};
    const VideoCommon::QueryType query_type =
        static_cast<VideoCommon::QueryType>(regs.report_semaphore.query.report.Value());
    const u32 payload = regs.report_semaphore.payload;
    const u32 subreport = regs.report_semaphore.query.sub_report;
    switch (regs.report_semaphore.query.operation)
    {
    case Regs::ReportSemaphore::Operation::Release:
        if (regs.report_semaphore.query.short_query != 0)
        {
            flags |= VideoCommon::QueryPropertiesFlags::IsAFence;
        }
        rasterizer->Query(sequence_address, query_type, flags, payload, subreport);
        break;
    case Regs::ReportSemaphore::Operation::Acquire:
        // TODO(Blinkhawk): Under this operation, the GPU waits for the CPU to write a value that
        // matches the current payload.
        UNIMPLEMENTED_MSG("Unimplemented query operation ACQUIRE");
        break;
    case Regs::ReportSemaphore::Operation::ReportOnly:
        rasterizer->Query(sequence_address, query_type, flags, payload, subreport);
        break;
    case Regs::ReportSemaphore::Operation::Trap:
        UNIMPLEMENTED_MSG("Unimplemented query operation TRAP");
        break;
    default:
        UNIMPLEMENTED_MSG("Unknown query operation");
        break;
    }
}

void Maxwell3D::ProcessQueryCondition()
{
    // Deferred instances still belong to the previous CPU and host predicate.
    draw_manager->DrawDeferred();
    if (rasterizer->AccelerateConditionalRendering())
    {
        execute_on = true;
        return;
    }
    const GPUVAddr condition_address{regs.render_enable.Address()};
    switch (regs.render_enable_override)
    {
    case Regs::RenderEnable::Override::AlwaysRender:
        execute_on = true;
        break;
    case Regs::RenderEnable::Override::NeverRender:
        execute_on = false;
        break;
    case Regs::RenderEnable::Override::UseRenderEnable:
    {
        switch (regs.render_enable.mode)
        {
        case Regs::RenderEnable::Mode::True:
        {
            execute_on = true;
            break;
        }
        case Regs::RenderEnable::Mode::False:
        {
            execute_on = false;
            break;
        }
        case Regs::RenderEnable::Mode::Conditional:
        {
            Regs::ReportSemaphore::Compare cmp;
            memory_manager.ReadBlock(condition_address, &cmp, sizeof(cmp));
            execute_on = cmp.initial_sequence != 0U && cmp.initial_mode != 0U;
            break;
        }
        case Regs::RenderEnable::Mode::IfEqual:
        {
            Regs::ReportSemaphore::Compare cmp;
            memory_manager.ReadBlock(condition_address, &cmp, sizeof(cmp));
            execute_on = cmp.initial_sequence == cmp.current_sequence &&
                         cmp.initial_mode == cmp.current_mode;
            break;
        }
        case Regs::RenderEnable::Mode::IfNotEqual:
        {
            Regs::ReportSemaphore::Compare cmp;
            memory_manager.ReadBlock(condition_address, &cmp, sizeof(cmp));
            execute_on = cmp.initial_sequence != cmp.current_sequence ||
                         cmp.initial_mode != cmp.current_mode;
            break;
        }
        default:
        {
            UNIMPLEMENTED_MSG("Uninplemented Condition Mode!");
            execute_on = true;
            break;
        }
        }
        break;
    }
    }
}

void Maxwell3D::ProcessCounterReset()
{
    const auto query_type = [clear_report = regs.clear_report_value]() {
        switch (clear_report)
        {
        case Tegra::Engines::Maxwell3D::Regs::ClearReport::ZPassPixelCount:
            return VideoCommon::QueryType::ZPassPixelCount64;
        case Tegra::Engines::Maxwell3D::Regs::ClearReport::StreamingPrimitivesSucceeded:
            return VideoCommon::QueryType::StreamingPrimitivesSucceeded;
        case Tegra::Engines::Maxwell3D::Regs::ClearReport::PrimitivesGenerated:
            return VideoCommon::QueryType::PrimitivesGenerated;
        case Tegra::Engines::Maxwell3D::Regs::ClearReport::VtgPrimitivesOut:
            return VideoCommon::QueryType::VtgPrimitivesOut;
        default:
            LOG_DEBUG(HW_GPU, "Unimplemented counter reset={}", clear_report);
            return VideoCommon::QueryType::Payload;
        }
    }();
    rasterizer->ResetCounter(query_type);
}

void Maxwell3D::ProcessSyncPoint()
{
    const u32 sync_point = regs.sync_info.sync_point.Value();
    [[maybe_unused]] const u32 cache_flush = regs.sync_info.clean_l2.Value();
    rasterizer->SignalSyncPoint(sync_point);
}

void Maxwell3D::ProcessCBBind(size_t stage_index)
{
    // Bind the buffer currently in CB_ADDRESS to the specified index in the desired shader
    // stage.
    const auto & bind_data = regs.bind_groups[stage_index];
    auto & buffer = state.shader_stages[stage_index].const_buffers[bind_data.shader_slot];
    if (buffer.enabled != (bind_data.valid.Value() != 0) || buffer.size != regs.const_buffer.size)
    {
        dirty.pipeline_key_changed = true;
    }
    // Binding the same buffer again leaves what the next draw reads
    if (buffer.enabled != (bind_data.valid.Value() != 0) || buffer.size != regs.const_buffer.size)
    {
        ++generations[GEN_STATE];
    }
    else if (buffer.address != regs.const_buffer.Address())
    {
        ++generations[GEN_BUFFERS];
        buffer_changes.rebound_slots[stage_index] |= 1U << bind_data.shader_slot;
    }
    buffer.enabled = bind_data.valid.Value() != 0;
    buffer.address = regs.const_buffer.Address();
    buffer.size = regs.const_buffer.size;

    const bool is_enabled = bind_data.valid.Value() != 0;
    if (!is_enabled)
    {
        rasterizer->DisableGraphicsUniformBuffer(stage_index, bind_data.shader_slot);
        return;
    }
    const GPUVAddr gpu_addr = regs.const_buffer.Address();
    const u32 size = regs.const_buffer.size;
    rasterizer->BindGraphicsUniformBuffer(stage_index, bind_data.shader_slot, gpu_addr, size);
}

void Maxwell3D::ProcessCBMultiData(const u32 * start_base, u32 amount)
{
    // Write the input value to the current const buffer at the current position.
    const GPUVAddr buffer_address = regs.const_buffer.Address();
    ASSERT(buffer_address != 0);

    // Don't allow writing past the end of the buffer.
    ASSERT(regs.const_buffer.offset <= regs.const_buffer.size);

    const GPUVAddr address{buffer_address + regs.const_buffer.offset};
    const size_t copy_size = amount * sizeof(u32);
    memory_manager.WriteConstBufferCached(address, start_base, copy_size);
    ++generations[GEN_BUFFERS];
    // Uploads mostly write one buffer in a row
    auto& changes = buffer_changes;
    if (changes.num_writes != 0 &&
        changes.writes[changes.num_writes - 1].first + changes.writes[changes.num_writes - 1].second ==
            address)
    {
        changes.writes[changes.num_writes - 1].second += copy_size;
    }
    else if (changes.num_writes < BufferChanges::MAX_WRITES)
    {
        changes.writes[changes.num_writes++] = {address, copy_size};
    }
    else
    {
        changes.too_many_writes = true;
    }

    // Increment the current buffer position.
    regs.const_buffer.offset += static_cast<u32>(copy_size);
}

void Maxwell3D::ProcessCBData(u32 value)
{
    ProcessCBMultiData(&value, 1);
}

Texture::TICEntry Maxwell3D::GetTICEntry(u32 tic_index) const
{
    const GPUVAddr tic_address_gpu{regs.tex_header.Address() +
                                   tic_index * sizeof(Texture::TICEntry)};
    Texture::TICEntry tic_entry;
    memory_manager.ReadBlockUnsafe(tic_address_gpu, &tic_entry, sizeof(Texture::TICEntry));
    return tic_entry;
}

Texture::TSCEntry Maxwell3D::GetTSCEntry(u32 tsc_index) const
{
    const GPUVAddr tsc_address_gpu{regs.tex_sampler.Address() +
                                   tsc_index * sizeof(Texture::TSCEntry)};
    Texture::TSCEntry tsc_entry;
    memory_manager.ReadBlockUnsafe(tsc_address_gpu, &tsc_entry, sizeof(Texture::TSCEntry));
    return tsc_entry;
}

u32 Maxwell3D::GetRegisterValue(u32 method) const
{
    ASSERT_MSG(method < Regs::NUM_REGS, "Invalid Maxwell3D register");
    return regs.reg_array[method];
}

void Maxwell3D::SetHLEReplacementAttributeType(u32 bank, u32 offset,
                                               HLEReplacementAttributeType name)
{
    const u64 key = (static_cast<u64>(bank) << 32) | offset;
    replace_table.emplace(key, name);
}

} // namespace Tegra::Engines
