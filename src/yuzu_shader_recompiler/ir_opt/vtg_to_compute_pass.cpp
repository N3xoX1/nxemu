// SPDX-FileCopyrightText: Copyright 2026 NxEmu Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <optional>

#include "yuzu_common/div_ceil.h"
#include "yuzu_shader_recompiler/frontend/ir/attribute.h"
#include "yuzu_shader_recompiler/frontend/ir/basic_block.h"
#include "yuzu_shader_recompiler/frontend/ir/ir_emitter.h"
#include "yuzu_shader_recompiler/frontend/ir/program.h"
#include "yuzu_shader_recompiler/frontend/ir/value.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"
#include "yuzu_shader_recompiler/vtg_as_compute.h"

namespace Shader::Optimization {
namespace {
constexpr u32 WORD_SIZE = sizeof(u32);
constexpr size_t MAX_INPUT_VERTICES = 6;

class Emitter : public IR::IREmitter {
public:
    explicit Emitter(IR::Block& block_, IR::Block::iterator point_)
        : IR::IREmitter{block_, point_}, point{point_} {}

    IR::U32 Add(const IR::U32& a, const IR::U32& b) {
        return IR::U32{IAdd(a, b)};
    }

    IR::U32 Sub(const IR::U32& a, const IR::U32& b) {
        return IR::U32{ISub(a, b)};
    }

    IR::U32 Mul(const IR::U32& a, u32 b) {
        return b == 1 ? a : IMul(a, Imm32(b));
    }

    IR::U32 Mod(const IR::U32& a, const IR::U32& b) {
        return Sub(a, IMul(IDiv(a, b), b));
    }

    IR::U32 LoadStorage(u32 binding, const IR::U32& byte_offset) {
        const auto it{block->PrependNewInst(point, IR::Opcode::LoadStorage32,
                                            {IR::Value{binding}, IR::Value{byte_offset}})};
        return IR::U32{IR::Value{&*it}};
    }

    void WriteStorage(u32 binding, const IR::U32& byte_offset, const IR::U32& value) {
        block->PrependNewInst(point, IR::Opcode::WriteStorage32,
                              {IR::Value{binding}, IR::Value{byte_offset}, IR::Value{value}});
    }

    IR::U32 LoadWord(u32 binding, u32 word) {
        return LoadStorage(binding, Imm32(word * WORD_SIZE));
    }

    /// Reads up to four bytes from any byte offset of a storage buffer
    IR::U32 LoadUnaligned(u32 binding, const IR::U32& byte_offset, const IR::U32& num_bytes) {
        const IR::U32 misalign{BitwiseAnd(byte_offset, Imm32(3U))};
        const IR::U32 base{Sub(byte_offset, misalign)};
        const IR::U32 shift{ShiftLeftLogical(misalign, Imm32(3U))};
        const IR::U32 low{ShiftRightLogical(LoadStorage(binding, base), shift)};
        // The next word is only touched by reads crossing into it, as it may not exist
        const IR::U1 crosses{ILessThan(Imm32(WORD_SIZE), Add(misalign, num_bytes), false)};
        const IR::U32 next{Add(base, IR::U32{Select(crosses, Imm32(WORD_SIZE), Imm32(0U))})};
        const IR::U32 high_shift{BitwiseAnd(Sub(Imm32(32U), shift), Imm32(31U))};
        const IR::U32 high{ShiftLeftLogical(LoadStorage(binding, next), high_shift)};
        return BitwiseOr(low, IR::U32{Select(crosses, high, Imm32(0U))});
    }

private:
    IR::Block::iterator point;
};

IR::Block::iterator IteratorTo(IR::Inst& inst) {
    return IR::Block::InstructionList::s_iterator_to(inst);
}

u32 ReserveStorageBuffer(Info& info, bool is_written) {
    const u32 index{static_cast<u32>(info.storage_buffers_descriptors.size())};
    info.storage_buffers_descriptors.push_back({
        .cbuf_index = static_cast<u32>(Info::MAX_CBUFS),
        .cbuf_offset = index,
        .count = 1,
        .is_written = is_written,
    });
    return index;
}

bool IsFloatLike(VtgComponentType type) {
    return type != VtgComponentType::UInt && type != VtgComponentType::SInt;
}

/// Converts a component as read from a vertex buffer into the value the shader receives
IR::F32 DecodeComponent(Emitter& ir, const VtgVertexAttribute& attribute, const IR::U32& raw,
                        u32 bits) {
    const auto unsigned_value{[&] {
        return bits >= 32 ? raw : ir.BitFieldExtract(raw, ir.Imm32(0U), ir.Imm32(bits), false);
    }};
    const auto signed_value{[&] {
        return bits >= 32 ? raw : ir.BitFieldExtract(raw, ir.Imm32(0U), ir.Imm32(bits), true);
    }};
    switch (attribute.type) {
    case VtgComponentType::Float:
        switch (bits) {
        case 32:
            return ir.BitCast<IR::F32>(raw);
        case 16:
            return IR::F32{ir.CompositeExtract(ir.UnpackHalf2x16(unsigned_value()), 0)};
        default: {
            // Small unsigned floats are half floats missing the sign and low mantissa bits
            const IR::U32 half{ir.ShiftLeftLogical(unsigned_value(), ir.Imm32(15U - bits))};
            return IR::F32{ir.CompositeExtract(ir.UnpackHalf2x16(half), 0)};
        }
        }
    case VtgComponentType::UNorm: {
        const f64 max_value{static_cast<f64>((u64{1} << bits) - 1)};
        return IR::F32{ir.FPMul(IR::F32{ir.ConvertUToF(32, 32, unsigned_value())},
                                ir.Imm32(static_cast<f32>(1.0 / max_value)))};
    }
    case VtgComponentType::SNorm: {
        const f64 max_value{bits > 1 ? static_cast<f64>((u64{1} << (bits - 1)) - 1) : 1.0};
        const IR::F32 value{ir.FPMul(IR::F32{ir.ConvertSToF(32, 32, signed_value())},
                                     ir.Imm32(static_cast<f32>(1.0 / max_value)))};
        return IR::F32{ir.FPMax(value, ir.Imm32(-1.0f))};
    }
    case VtgComponentType::UScaled:
        return IR::F32{ir.ConvertUToF(32, 32, unsigned_value())};
    case VtgComponentType::SScaled:
        return IR::F32{ir.ConvertSToF(32, 32, signed_value())};
    case VtgComponentType::UInt:
        return ir.BitCast<IR::F32>(unsigned_value());
    case VtgComponentType::SInt:
        return ir.BitCast<IR::F32>(signed_value());
    case VtgComponentType::Disabled:
        break;
    }
    return ir.Imm32(0.0f);
}

IR::F32 DefaultComponent(Emitter& ir, const VtgVertexAttribute& attribute, u32 component) {
    if (component != 3) {
        return ir.Imm32(0.0f);
    }
    return IsFloatLike(attribute.type) ? ir.Imm32(1.0f) : ir.BitCast<IR::F32>(ir.Imm32(1U));
}

/// Value of a varying missing from the buffer it is read from
IR::F32 MissingVarying(Emitter& ir, IR::Attribute attribute) {
    const bool is_w{IR::IsGeneric(attribute) && IR::GenericAttributeElement(attribute) == 3};
    return ir.Imm32(is_w ? 1.0f : 0.0f);
}

bool IsIndexedOrUnsupported(const IR::Program& program) {
    const Info& info{program.info};
    if (info.loads_indexed_attributes || info.stores_indexed_attributes ||
        info.uses_render_area) {
        return true;
    }
    for (const IR::Block* const block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            switch (inst.GetOpcode()) {
            case IR::Opcode::GetAttributeIndexed:
            case IR::Opcode::SetAttributeIndexed:
            case IR::Opcode::GetPatch:
            case IR::Opcode::SetPatch:
                return true;
            default:
                break;
            }
        }
    }
    return false;
}

void FinishComputeProgram(IR::Program& program) {
    program.stage = Stage::Compute;
    program.workgroup_size = {VTG_LOCAL_SIZE, 1, 1};
    program.info.uses_workgroup_id = true;
    program.info.uses_local_invocation_id = true;
    program.info.used_storage_buffer_types |= IR::Type::U32;
    program.info.loads = {};
    program.info.stores = {};
}

/// Index of the invocation across the whole dispatch
IR::U32 GlobalInvocation(Emitter& ir) {
    return ir.Add(ir.IMul(ir.WorkgroupIdX(), ir.Imm32(VTG_LOCAL_SIZE)), ir.LocalInvocationIdX());
}

IR::U32 PrimitiveVertex(Emitter& ir, VtgTopology topology, const IR::U32& primitive, u32 vertex,
                        const IR::U32& vertex_count) {
    const auto offset{[&](const IR::U32& base, u32 value) {
        return value == 0 ? base : ir.Add(base, ir.Imm32(value));
    }};
    const IR::U32 odd{ir.BitwiseAnd(primitive, ir.Imm32(1U))};
    const IR::U32 half{ir.ShiftRightLogical(primitive, ir.Imm32(1U))};
    switch (topology) {
    case VtgTopology::Points:
        return primitive;
    case VtgTopology::Lines:
        return offset(ir.Mul(primitive, 2), vertex);
    case VtgTopology::LineLoop:
        return ir.Mod(offset(primitive, vertex), ir.UMax(vertex_count, ir.Imm32(1U)));
    case VtgTopology::LineStrip:
    case VtgTopology::LineStripAdjacency:
        return offset(primitive, vertex);
    case VtgTopology::Triangles:
        return offset(ir.Mul(primitive, 3), vertex);
    case VtgTopology::TriangleStrip:
        switch (vertex) {
        case 0:
            return primitive;
        case 1:
            return ir.Add(offset(primitive, 1), odd);
        default:
            return ir.Sub(offset(primitive, 2), odd);
        }
    case VtgTopology::TriangleFan:
        return vertex == 0 ? ir.Imm32(0U) : offset(primitive, vertex);
    case VtgTopology::Quads: {
        const IR::U32 base{ir.Mul(half, 4)};
        return vertex == 0 ? base : ir.Add(offset(base, vertex), odd);
    }
    case VtgTopology::QuadStrip: {
        const IR::U32 base{ir.Mul(half, 2)};
        switch (vertex) {
        case 0:
            return base;
        case 1:
            return ir.Add(offset(base, 1), ir.Mul(odd, 2));
        default:
            return ir.Sub(offset(base, 3), odd);
        }
    }
    case VtgTopology::LinesAdjacency:
        return offset(ir.Mul(primitive, 4), vertex);
    case VtgTopology::TrianglesAdjacency:
        return offset(ir.Mul(primitive, 6), vertex);
    case VtgTopology::TriangleStripAdjacency:
        return offset(ir.Mul(primitive, 2), vertex);
    }
    return primitive;
}
} // Anonymous namespace

bool VtgVertexToCompute(IR::Program& program, const VtgVertexInputs& inputs,
                        const VtgVaryingLayout& layout, VtgVertexBindings& bindings) {
    Info& info{program.info};
    if (program.stage != Stage::VertexB || IsIndexedOrUnsupported(program)) {
        return false;
    }
    std::array<bool, VtgDrawInfo::NUM_ATTRIBUTES> uses_buffer{};
    size_t num_buffers{};
    for (size_t index = 0; index < inputs.size(); ++index) {
        const VtgVertexAttribute& attribute{inputs[index]};
        if (!info.loads.Generic(index) || attribute.type == VtgComponentType::Disabled) {
            continue;
        }
        if (attribute.components == 0 || attribute.buffer >= uses_buffer.size()) {
            return false;
        }
        num_buffers += uses_buffer[attribute.buffer] ? 0 : 1;
        uses_buffer[attribute.buffer] = true;
    }
    if (info.storage_buffers_descriptors.size() + 3 + num_buffers > Info::MAX_SSBOS) {
        return false;
    }
    bindings.base = static_cast<u32>(info.storage_buffers_descriptors.size());
    bindings.draw_info = ReserveStorageBuffer(info, false);
    bindings.output = ReserveStorageBuffer(info, true);
    bindings.index_buffer = ReserveStorageBuffer(info, false);
    std::array<u32, VtgDrawInfo::NUM_ATTRIBUTES> buffer_bindings{};
    for (size_t buffer = 0; buffer < uses_buffer.size(); ++buffer) {
        if (!uses_buffer[buffer]) {
            continue;
        }
        buffer_bindings[buffer] = ReserveStorageBuffer(info, false);
        bindings.vertex_buffers.emplace_back(static_cast<u8>(buffer), buffer_bindings[buffer]);
    }

    IR::Block& entry{*program.blocks.front()};
    Emitter ir{entry, entry.begin()};
    const auto draw_info{[&](u32 word) { return ir.LoadWord(bindings.draw_info, word); }};

    const IR::U32 invocation{GlobalInvocation(ir)};
    const IR::U32 vertex_count{ir.UMax(draw_info(VtgDrawInfo::VERTEX_COUNT), ir.Imm32(1U))};
    const IR::U32 last_instance{ir.Sub(draw_info(VtgDrawInfo::INSTANCE_COUNT), ir.Imm32(1U))};
    // Invocations past the end of the draw repeat the last instance into unused output
    const IR::U32 instance{ir.UMin(ir.IDiv(invocation, vertex_count), last_instance)};
    const IR::U32 element{ir.Mod(invocation, vertex_count)};
    const IR::U32 base_vertex{draw_info(VtgDrawInfo::BASE_VERTEX)};
    const IR::U32 base_instance{draw_info(VtgDrawInfo::BASE_INSTANCE)};
    const IR::U32 index_size{draw_info(VtgDrawInfo::INDEX_SIZE)};
    const IR::U32 index_address{
        ir.Add(draw_info(VtgDrawInfo::INDEX_OFFSET), ir.IMul(element, index_size))};
    const IR::U32 index{ir.BitwiseAnd(ir.LoadUnaligned(bindings.index_buffer, index_address, index_size),
                                      draw_info(VtgDrawInfo::INDEX_MASK))};
    const IR::U32 vertex_id{ir.Select(ir.IEqual(index_size, ir.Imm32(0U)), element, index)};
    const IR::U32 fetch_vertex{ir.Add(vertex_id, base_vertex)};
    const IR::U32 output_base{ir.Mul(invocation, layout.stride * WORD_SIZE)};

    std::array<std::array<IR::F32, 4>, VtgDrawInfo::NUM_ATTRIBUTES> generics;
    for (size_t location = 0; location < inputs.size(); ++location) {
        const VtgVertexAttribute& attribute{inputs[location]};
        const bool is_read{info.loads.Generic(location) &&
                           attribute.type != VtgComponentType::Disabled};
        IR::U32 address;
        if (is_read) {
            const u32 words{VtgDrawInfo::ATTRIBUTES +
                            static_cast<u32>(location) * VtgDrawInfo::ATTRIBUTE_WORDS};
            const IR::U32 divisor{
                ir.UMax(draw_info(words + VtgDrawInfo::ATTRIBUTE_DIVISOR), ir.Imm32(1U))};
            const IR::U32 instanced{ir.Add(base_instance, ir.IDiv(instance, divisor))};
            const IR::U1 is_instanced{
                ir.INotEqual(draw_info(words + VtgDrawInfo::ATTRIBUTE_INSTANCED), ir.Imm32(0U))};
            const IR::U32 source{ir.Select(is_instanced, instanced, fetch_vertex)};
            address = ir.Add(draw_info(words + VtgDrawInfo::ATTRIBUTE_OFFSET),
                             ir.IMul(source, draw_info(words + VtgDrawInfo::ATTRIBUTE_STRIDE)));
        }
        const u32 binding{buffer_bindings[attribute.buffer % buffer_bindings.size()]};
        const bool is_packed{attribute.bits == 0};
        const u32 total_bits{static_cast<u32>(attribute.bits) * attribute.components};
        // Attributes fitting in a word are read once
        const bool single_read{is_read && (is_packed || total_bits <= 32)};
        IR::U32 packed;
        if (single_read) {
            packed = ir.LoadUnaligned(binding, address,
                                      ir.Imm32(is_packed ? WORD_SIZE : total_bits / 8));
        }
        for (u32 component = 0; component < 4; ++component) {
            if (!is_read || !info.loads.Generic(location, component)) {
                generics[location][component] = DefaultComponent(ir, attribute, component);
                continue;
            }
            if (component >= attribute.components) {
                generics[location][component] = DefaultComponent(ir, attribute, component);
                continue;
            }
            const u32 bits{is_packed ? attribute.packed_bits[component] : attribute.bits};
            const u32 bit_offset{is_packed ? attribute.packed_offsets[component]
                                           : component * attribute.bits};
            IR::U32 raw;
            if (single_read) {
                raw = bit_offset == 0
                          ? packed
                          : IR::U32{ir.ShiftRightLogical(packed, ir.Imm32(bit_offset))};
            } else {
                raw = ir.LoadUnaligned(binding, ir.Add(address, ir.Imm32(bit_offset / 8)),
                                       ir.Imm32(bits / 8));
            }
            generics[location][component] = DecodeComponent(ir, attribute, raw, bits);
        }
    }

    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            Emitter at{*block, IteratorTo(inst)};
            switch (inst.GetOpcode()) {
            case IR::Opcode::Prologue:
            case IR::Opcode::Epilogue:
                inst.Invalidate();
                break;
            case IR::Opcode::GetAttribute:
            case IR::Opcode::GetAttributeU32: {
                const bool is_float{inst.GetOpcode() == IR::Opcode::GetAttribute};
                const IR::Attribute attribute{inst.Arg(0).Attribute()};
                if (IR::IsGeneric(attribute)) {
                    const IR::F32 value{generics[IR::GenericAttributeIndex(attribute)]
                                                [IR::GenericAttributeElement(attribute)]};
                    inst.ReplaceUsesWith(is_float ? IR::Value{value}
                                                  : IR::Value{at.BitCast<IR::U32>(value)});
                    break;
                }
                IR::U32 value{at.Imm32(0U)};
                switch (attribute) {
                case IR::Attribute::VertexId:
                    value = vertex_id;
                    break;
                case IR::Attribute::InstanceId:
                    value = instance;
                    break;
                case IR::Attribute::BaseVertex:
                    value = base_vertex;
                    break;
                case IR::Attribute::BaseInstance:
                    value = base_instance;
                    break;
                default:
                    break;
                }
                inst.ReplaceUsesWith(is_float ? IR::Value{at.BitCast<IR::F32>(value)}
                                              : IR::Value{value});
                break;
            }
            case IR::Opcode::SetAttribute: {
                const IR::Attribute attribute{inst.Arg(0).Attribute()};
                if (const std::optional<u32> word{layout.Offset(attribute)}) {
                    at.WriteStorage(bindings.output,
                                    at.Add(output_base, at.Imm32(*word * WORD_SIZE)),
                                    at.BitCast<IR::U32>(IR::F32{inst.Arg(1)}));
                }
                inst.Invalidate();
                break;
            }
            default:
                break;
            }
        }
    }
    FinishComputeProgram(program);
    return true;
}

bool VtgGeometryToCompute(IR::Program& program, VtgTopology topology, u32 input_vertices,
                          const VtgVaryingLayout& input_layout,
                          const VtgVaryingLayout& output_layout, VtgGeometryBindings& bindings) {
    Info& info{program.info};
    if (program.stage != Stage::Geometry || IsIndexedOrUnsupported(program)) {
        return false;
    }
    if (program.invocations > 1 || program.output_vertices == 0 ||
        input_vertices > MAX_INPUT_VERTICES || input_vertices == 0) {
        return false;
    }
    if (info.storage_buffers_descriptors.size() + 4 > Info::MAX_SSBOS) {
        return false;
    }
    bindings.base = static_cast<u32>(info.storage_buffers_descriptors.size());
    bindings.draw_info = ReserveStorageBuffer(info, false);
    bindings.input = ReserveStorageBuffer(info, false);
    bindings.output = ReserveStorageBuffer(info, true);
    bindings.index_buffer = ReserveStorageBuffer(info, true);
    bindings.vertices_per_primitive = program.output_vertices;
    // Every vertex may end a strip and the last index always does
    bindings.indices_per_primitive = program.output_vertices * 2 + 1;
    bindings.output_topology = program.output_topology;

    // Emitted vertices are staged in local memory after the memory of the guest
    const u32 local_base{Common::DivCeil(program.local_memory_size, WORD_SIZE)};
    const u32 vertex_counter{local_base};
    const u32 index_counter{local_base + 1};
    const u32 staging{local_base + 2};
    program.local_memory_size = (staging + output_layout.stride) * WORD_SIZE;
    info.uses_local_memory = true;

    const u32 max_vertex{bindings.vertices_per_primitive - 1};
    const u32 max_index{bindings.indices_per_primitive - 2};
    const bool emits_points{program.output_topology == OutputTopology::PointList};

    IR::Block& entry{*program.blocks.front()};
    Emitter ir{entry, entry.begin()};
    const auto draw_info{[&](u32 word) { return ir.LoadWord(bindings.draw_info, word); }};

    const IR::U32 invocation{GlobalInvocation(ir)};
    const IR::U32 primitive_count{
        ir.UMax(draw_info(VtgDrawInfo::PRIMITIVE_COUNT), ir.Imm32(1U))};
    const IR::U32 last_instance{ir.Sub(draw_info(VtgDrawInfo::INSTANCE_COUNT), ir.Imm32(1U))};
    // Invocations past the end of the draw repeat the last instance into unused output
    const IR::U32 instance{ir.UMin(ir.IDiv(invocation, primitive_count), last_instance)};
    const IR::U32 primitive{ir.Mod(invocation, primitive_count)};
    const IR::U32 vertex_count{draw_info(VtgDrawInfo::VERTEX_COUNT)};
    const IR::U32 instance_base{ir.IMul(instance, vertex_count)};
    std::array<IR::U32, MAX_INPUT_VERTICES> input_bases;
    for (u32 vertex = 0; vertex < input_vertices; ++vertex) {
        const IR::U32 source{PrimitiveVertex(ir, topology, primitive, vertex, vertex_count)};
        input_bases[vertex] =
            ir.Mul(ir.Add(instance_base, source), input_layout.stride * WORD_SIZE);
    }
    const IR::U32 vertex_base{ir.Mul(invocation, bindings.vertices_per_primitive)};
    const IR::U32 index_base{ir.Mul(invocation, bindings.indices_per_primitive)};
    ir.WriteLocal(ir.Imm32(vertex_counter), ir.Imm32(0U));
    ir.WriteLocal(ir.Imm32(index_counter), ir.Imm32(0U));

    const auto input_base{[&](Emitter& at, const IR::Value& vertex) {
        if (vertex.IsImmediate()) {
            return input_bases[std::min(vertex.U32(), input_vertices - 1)];
        }
        IR::U32 base{input_bases[0]};
        for (u32 index = 1; index < input_vertices; ++index) {
            base = IR::U32{at.Select(at.IEqual(IR::U32{vertex}, at.Imm32(index)),
                                     input_bases[index], base)};
        }
        return base;
    }};
    const auto index_address{[&](Emitter& at, const IR::U32& count) {
        return at.Mul(at.Add(index_base, at.UMin(count, at.Imm32(max_index))), WORD_SIZE);
    }};
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            Emitter at{*block, IteratorTo(inst)};
            switch (inst.GetOpcode()) {
            case IR::Opcode::Prologue:
            case IR::Opcode::Epilogue:
                inst.Invalidate();
                break;
            case IR::Opcode::InvocationId:
                inst.ReplaceUsesWith(IR::Value{0U});
                break;
            case IR::Opcode::InvocationInfo:
                // Same value the geometry stage gets, the vertices of its input primitive
                inst.ReplaceUsesWith(IR::Value{input_vertices << 16});
                break;
            case IR::Opcode::GetAttribute:
            case IR::Opcode::GetAttributeU32: {
                const bool is_float{inst.GetOpcode() == IR::Opcode::GetAttribute};
                const IR::Attribute attribute{inst.Arg(0).Attribute()};
                IR::U32 value;
                if (attribute == IR::Attribute::PrimitiveId) {
                    value = primitive;
                } else if (const std::optional<u32> word{input_layout.Offset(attribute)}) {
                    const IR::U32 base{input_base(at, inst.Arg(1))};
                    value = at.LoadStorage(bindings.input,
                                           at.Add(base, at.Imm32(*word * WORD_SIZE)));
                } else {
                    value = at.BitCast<IR::U32>(MissingVarying(at, attribute));
                }
                inst.ReplaceUsesWith(is_float ? IR::Value{at.BitCast<IR::F32>(value)}
                                              : IR::Value{value});
                break;
            }
            case IR::Opcode::SetAttribute: {
                const IR::Attribute attribute{inst.Arg(0).Attribute()};
                if (const std::optional<u32> word{output_layout.Offset(attribute)}) {
                    at.WriteLocal(at.Imm32(staging + *word),
                                  at.BitCast<IR::U32>(IR::F32{inst.Arg(1)}));
                }
                inst.Invalidate();
                break;
            }
            case IR::Opcode::EmitVertex: {
                const IR::U32 vertices{at.LoadLocal(at.Imm32(vertex_counter))};
                const IR::U32 indices{at.LoadLocal(at.Imm32(index_counter))};
                const IR::U32 vertex{
                    at.Add(vertex_base, at.UMin(vertices, at.Imm32(max_vertex)))};
                const IR::U32 address{at.Mul(vertex, output_layout.stride * WORD_SIZE)};
                for (u32 word = 0; word < output_layout.stride; ++word) {
                    at.WriteStorage(bindings.output, at.Add(address, at.Imm32(word * WORD_SIZE)),
                                    at.LoadLocal(at.Imm32(staging + word)));
                }
                at.WriteStorage(bindings.index_buffer, index_address(at, indices), vertex);
                at.WriteLocal(at.Imm32(vertex_counter), at.Add(vertices, at.Imm32(1U)));
                at.WriteLocal(at.Imm32(index_counter), at.Add(indices, at.Imm32(1U)));
                inst.Invalidate();
                break;
            }
            case IR::Opcode::EndPrimitive: {
                if (!emits_points) {
                    const IR::U32 indices{at.LoadLocal(at.Imm32(index_counter))};
                    at.WriteStorage(bindings.index_buffer, index_address(at, indices),
                                    at.Imm32(VTG_RESTART_INDEX));
                    at.WriteLocal(at.Imm32(index_counter), at.Add(indices, at.Imm32(1U)));
                }
                inst.Invalidate();
                break;
            }
            default:
                break;
            }
        }
    }
    FinishComputeProgram(program);
    return true;
}

} // namespace Shader::Optimization
