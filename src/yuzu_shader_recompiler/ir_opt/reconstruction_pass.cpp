// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_shader_recompiler/ir_opt/reconstruction_pass.h"
#include "yuzu_shader_recompiler/frontend/ir/ir_emitter.h"
#include "yuzu_shader_recompiler/frontend/ir/modifiers.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace Shader::Optimization
{
namespace
{
using Op = IR::Opcode;
using Key = std::pair<u32, u32>;
struct Pixel
{
    unsigned axis{}; // 1=X, 2=Y
    bool half{}, integer{};
};

bool FloatConstant(IR::Value value, float expected)
{
    value = value.Resolve();
    return value.IsImmediate() && value.Type() == IR::Type::F32 && value.F32() == expected;
}

class Coordinates
{
public:
    Pixel Get(IR::Value value)
    {
        value = value.Resolve();
        if (value.IsImmediate()) return {};
        auto * inst = value.Inst();
        if (auto it = cache.find(inst); it != cache.end()) return it->second;
        if (depth >= 64) return {};
        cache.emplace(inst, Pixel{}); // Cycles/unsupported expressions are not inferred.
        ++depth;
        const Pixel result = Infer(*inst);
        --depth;
        cache[inst] = result;
        return result;
    }

private:
    Pixel Infer(const IR::Inst & inst)
    {
        switch (inst.GetOpcode())
        {
        case Op::GetAttribute:
            if (inst.Arg(0).Attribute() == IR::Attribute::PositionX) return {1};
            if (inst.Arg(0).Attribute() == IR::Attribute::PositionY) return {2};
            return {};
        case Op::BitCastU32F32:
        case Op::BitCastF32U32:
        case Op::ConvertS32F32:
        case Op::ConvertU32F32:
        case Op::ConvertF32S32:
        case Op::ConvertF32U32:
            return Get(inst.Arg(0));
        case Op::FPClamp32:
            if (!inst.Arg(1).IsImmediate() || !inst.Arg(2).IsImmediate()) return {};
            return Get(inst.Arg(0));
        case Op::FPTrunc32:
        case Op::FPFloor32:
        {
            auto pixel = Get(inst.Arg(0));
            pixel.integer = true;
            return pixel;
        }
        case Op::FPMul32:
            for (unsigned i = 0; i < 2; ++i)
            {
                auto pixel = Get(inst.Arg(i));
                if (pixel.axis && FloatConstant(inst.Arg(1 - i), 0.5f))
                {
                    pixel.half = true;
                    pixel.integer = false;
                    return pixel;
                }
                if (pixel.axis && (FloatConstant(inst.Arg(1 - i), 2.0f) ||
                                   FloatConstant(inst.Arg(1 - i), 1.0f))) return pixel;
            }
            return {};
        case Op::FPAdd32:
            for (unsigned i = 0; i < 2; ++i)
            {
                const auto offset = inst.Arg(1 - i).Resolve();
                const auto pixel = Get(inst.Arg(i));
                if (pixel.axis && offset.IsImmediate() && offset.Type() == IR::Type::F32 &&
                    std::isfinite(offset.F32())) return pixel;
            }
            return {};
        case Op::SelectU32:
        {
            // Maxwell's float->integer conversion protects NaN with select(0, value).
            const auto condition = inst.Arg(0).Resolve();
            const auto zero = inst.Arg(1).Resolve();
            if (condition.IsImmediate() || zero.Type() != IR::Type::U32 ||
                !zero.IsImmediate() || zero.U32() != 0 ||
                condition.Inst()->GetOpcode() != Op::FPIsNan32) return {};
            const auto converted = Get(inst.Arg(2));
            return Get(condition.Inst()->Arg(0)).axis == converted.axis ? converted : Pixel{};
        }
        default: return {};
        }
    }
    std::unordered_map<const IR::Inst *, Pixel> cache;
    unsigned depth{};
};

std::optional<Key> ConstantBuffer(IR::Value value)
{
    value = value.Resolve();
    if (value.IsImmediate()) return {};
    const auto & inst = *value.Inst();
    if (inst.GetOpcode() != Op::GetCbufF32 || !inst.Arg(0).IsImmediate() ||
        !inst.Arg(1).IsImmediate()) return {};
    return Key{inst.Arg(0).U32(), inst.Arg(1).U32()};
}

// Slice only coordinate expressions. Sampling results (e.g. motion vectors)
// are opaque here: their color calculations do not define pixel dimensions.
bool CoordinateSlice(IR::Value value, std::unordered_set<const IR::Inst *> & slice,
                     unsigned depth = 0)
{
    value = value.Resolve();
    if (value.IsImmediate()) return true;
    const auto * inst = value.Inst();
    if (inst->GetOpcode() == Op::ImageSampleImplicitLod || !slice.insert(inst).second) return true;
    if (depth >= 128) return false;
    for (size_t i = 0; i < inst->NumArgs(); ++i)
    {
        if (!CoordinateSlice(inst->Arg(i), slice, depth + 1)) return false;
    }
    return true;
}
} // namespace

ReconstructionSteps FindReconstructionSteps(const IR::Program & program)
{
    if (program.stage != Stage::Fragment) return {};
    Coordinates coordinates;
    std::map<Key, unsigned> coefficients;
    std::unordered_set<const IR::Inst *> slice;
    std::unordered_set<const IR::Inst *> parity;
    std::set<u32> textures, outputs;
    bool half_x{}, y_step{};
    unsigned samples{};
    for (auto * block : program.blocks)
    {
        for (auto & inst : *block)
        {
            const auto op = inst.GetOpcode();
            if (op == Op::GetAttribute && inst.Arg(0).Attribute() != IR::Attribute::PositionX &&
                inst.Arg(0).Attribute() != IR::Attribute::PositionY) return {};
            // This recognizer deliberately excludes geometry/material shaders,
            // depth exports, storage writes, discard and unrelated texture operations.
            if (inst.MayHaveSideEffects() && op != Op::Prologue && op != Op::Epilogue &&
                op != Op::SetFragColor && op != Op::ConditionRef) return {};
            if (op == Op::SetFragColor) outputs.insert(inst.Arg(0).U32());
            // Accept only the sampling operation whose coordinate/LOD semantics
            // we handle. Reject depth, gather, query, atomic and future image ops.
            if (op != Op::ImageSampleImplicitLod &&
                IR::NameOf(op).find("Image") != std::string_view::npos) return {};
            if (op == Op::ImageSampleImplicitLod)
            {
                const auto info = inst.Flags<IR::TextureInstInfo>();
                if (info.type != TextureType::Color2D || info.is_depth ||
                    !inst.Arg(2).IsEmpty() || !inst.Arg(3).IsEmpty()) return {};
                textures.insert(info.descriptor_index);
                ++samples;
                if (!CoordinateSlice(inst.Arg(1), slice)) return {};
            }
            if (op == Op::BitwiseAnd32)
            {
                for (unsigned i = 0; i < 2; ++i)
                {
                    const auto mask = inst.Arg(1 - i).Resolve();
                    const auto pixel = coordinates.Get(inst.Arg(i));
                    if (mask.IsImmediate() && mask.Type() == IR::Type::U32 && mask.U32() == 1 &&
                        pixel.axis == 1 && pixel.integer && !pixel.half) parity.insert(&inst);
                }
            }
            if (op != Op::FPMul32 && op != Op::FPFma32) continue;
            for (unsigned i = 0; i < 2; ++i)
            {
                const auto pixel = coordinates.Get(inst.Arg(i));
                const auto coefficient = ConstantBuffer(inst.Arg(1 - i));
                if (!pixel.axis || !coefficient) continue;
                // Only products that really feed normalized image coordinates count.
                coefficients[*coefficient] |= pixel.axis;
                half_x |= pixel.axis == 1 && pixel.half && pixel.integer;
                y_step |= pixel.axis == 2;
            }
        }
    }
    // Two-dimensional half-rate reconstruction with auxiliary output and at
    // least two inputs. Simple pixelation, parity color patterns and ordinary
    // fullscreen sampling must stay on the generic path.
    if (!half_x || !y_step || parity.empty() || textures.size() < 2 ||
        outputs.size() < 2 || samples < 2 || coefficients.empty()) return {};

    std::unordered_set<const IR::Inst *> comparisons;
    for (auto * block : program.blocks)
    {
        for (auto & inst : *block)
        {
            if (inst.GetOpcode() != Op::IEqual && inst.GetOpcode() != Op::INotEqual) continue;
            for (size_t i = 0; i < 2; ++i)
            {
                const auto arg = inst.Arg(i).Resolve();
                if (!arg.IsImmediate() && parity.contains(arg.Inst())) comparisons.insert(&inst);
            }
        }
    }
    if (comparisons.empty()) return {};
    std::unordered_map<const IR::Inst *, std::vector<const IR::Inst *>> users;
    for (auto * block : program.blocks)
    {
        for (auto & inst : *block)
        {
            if (inst.GetOpcode() == Op::Identity) continue;
            for (size_t i = 0; i < inst.NumArgs(); ++i)
            {
                const auto arg = inst.Arg(i).Resolve();
                if (!arg.IsImmediate()) users[arg.Inst()].push_back(&inst);
            }
        }
    }
    std::vector<const IR::Inst *> pending(comparisons.begin(), comparisons.end());
    std::unordered_set<const IR::Inst *> visited;
    bool controls_branch{};
    while (!pending.empty())
    {
        const auto * predicate = pending.back();
        pending.pop_back();
        if (!visited.insert(predicate).second) continue;
        for (const auto * user : users[predicate])
        {
            switch (user->GetOpcode())
            {
            case Op::ConditionRef: controls_branch = true; break;
            case Op::LogicalAnd:
            case Op::LogicalOr:
            case Op::LogicalNot:
            case Op::LogicalXor:
                pending.push_back(user);
                break;
            case Op::Phi:
                if (user->Type() != IR::Type::U1) return {};
                pending.push_back(user);
                break;
            default: return {};
            }
        }
    }
    if (!controls_branch) return {};
    ReconstructionSteps result;
    result.textures.assign(textures.begin(), textures.end());
    for (auto * block : program.blocks)
    {
        for (auto & inst : *block)
        {
            if (inst.GetOpcode() == Op::Identity) continue;
            // A parity bit must be used as a branch predicate, not as a color or
            // dither weight. A coefficient reused as a color weight is ambiguous.
            for (size_t i = 0; i < inst.NumArgs(); ++i)
            {
                const auto arg = inst.Arg(i).Resolve();
                if (arg.IsImmediate()) continue;
                if (parity.contains(arg.Inst()) && !comparisons.contains(&inst)) return {};
                if (const auto cb = ConstantBuffer(arg); cb && coefficients.contains(*cb) &&
                                                         !slice.contains(&inst)) return {};
            }
            if (const auto cb = ConstantBuffer(IR::Value{&inst}); cb && coefficients.contains(*cb))
            {
                result.loads.push_back(&inst);
            }
        }
    }
    return result;
}

IR::U1 ScaleReconstructionSteps(IR::Program & program, const ReconstructionSteps & steps)
{
    IR::Block & entry = *program.blocks.front();
    auto insertion = std::find_if(entry.begin(), entry.end(),
                                  [](const auto & inst) { return inst.GetOpcode() == Op::Prologue; });
    if (insertion != entry.end()) ++insertion;
    IR::IREmitter ir{entry, insertion};
    IR::U1 all_scaled = ir.Imm1(true);
    for (u32 texture : steps.textures)
    {
        all_scaled = ir.LogicalAnd(all_scaled, ir.IsTextureScaled(ir.Imm32(texture)));
    }
    // Normalized reads also occur with native-size lookup/history textures.
    // Lift the entire reconstruction only when all its input images are scaled.
    const IR::F32 factor{ir.Select(all_scaled, ir.ResolutionDownFactor(), ir.Imm32(1.0f))};
    std::map<Key, IR::F32> scaled;
    for (auto * load : steps.loads)
    {
        const Key key{load->Arg(0).U32(), load->Arg(1).U32()};
        if (!scaled.contains(key))
        {
            scaled.emplace(key, ir.FPMul(ir.GetFloatCbuf(ir.Imm32(key.first),
                                                         ir.Imm32(key.second)),
                                         factor));
        }
        load->ReplaceUsesWith(scaled.at(key));
    }
    // Normalized origins/clamps are intentionally preserved: their half-texel
    // convention cannot be inferred from an arbitrary uniform bound.
    return all_scaled;
}
} // namespace Shader::Optimization
