// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "yuzu_shader_recompiler/frontend/ir/basic_block.h"
#include "yuzu_shader_recompiler/frontend/ir/opcodes.h"
#include "yuzu_shader_recompiler/frontend/ir/program.h"
#include "yuzu_shader_recompiler/frontend/ir/value.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"

namespace Shader::Optimization {
namespace {
using ResidueMask = std::uint16_t;

constexpr ResidueMask ALL_RESIDUES = 0xffff;
constexpr u32 SOURCE_STRIDE = 16;
constexpr u32 PACKED_STRIDE = 12;

[[nodiscard]] constexpr ResidueMask Residue(u32 value) {
    return static_cast<ResidueMask>(1U << (value & (SOURCE_STRIDE - 1)));
}

[[nodiscard]] ResidueMask BinaryResidues(ResidueMask lhs, ResidueMask rhs,
                                         u32 (*operation)(u32, u32)) {
    ResidueMask result{};
    for (u32 x = 0; x < SOURCE_STRIDE; ++x) {
        if ((lhs & Residue(x)) == 0) {
            continue;
        }
        for (u32 y = 0; y < SOURCE_STRIDE; ++y) {
            if ((rhs & Residue(y)) == 0) {
                continue;
            }
            result |= Residue(operation(x, y));
        }
    }
    return result;
}

[[nodiscard]] u32 Add(u32 lhs, u32 rhs) {
    return lhs + rhs;
}
[[nodiscard]] u32 Sub(u32 lhs, u32 rhs) {
    return lhs - rhs;
}
[[nodiscard]] u32 Mul(u32 lhs, u32 rhs) {
    return lhs * rhs;
}
[[nodiscard]] u32 And(u32 lhs, u32 rhs) {
    return lhs & rhs;
}
[[nodiscard]] u32 Or(u32 lhs, u32 rhs) {
    return lhs | rhs;
}
[[nodiscard]] u32 Xor(u32 lhs, u32 rhs) {
    return lhs ^ rhs;
}

class ResidueAnalyzer {
public:
    [[nodiscard]] ResidueMask Get(const IR::Value& value) {
        if (!value.IsIdentity() && value.IsImmediate()) {
            return value.Type() == IR::Type::U32 ? Residue(value.U32()) : ALL_RESIDUES;
        }
        // Bound recursion, including Identity chains, for large guest shaders.
        if (visiting.size() >= 128) {
            return ALL_RESIDUES;
        }
        const IR::Inst* const inst{value.Inst()};
        if (const auto it = cache.find(inst); it != cache.end()) {
            return it->second;
        }
        if (!visiting.insert(inst).second) {
            // Be deliberately conservative for loop-carried Phi values.
            return ALL_RESIDUES;
        }
        const ResidueMask result{Analyze(*inst)};
        visiting.erase(inst);
        cache.emplace(inst, result);
        return result;
    }

private:
    [[nodiscard]] ResidueMask Analyze(const IR::Inst& inst) {
        const auto binary = [&](u32 (*operation)(u32, u32)) {
            return BinaryResidues(Get(inst.Arg(0)), Get(inst.Arg(1)), operation);
        };
        switch (inst.GetOpcode()) {
        case IR::Opcode::Identity:
            return Get(inst.Arg(0));
        case IR::Opcode::Phi: {
            if (inst.NumArgs() == 0) {
                return ALL_RESIDUES;
            }
            ResidueMask result{};
            for (size_t i = 0; i < inst.NumArgs(); ++i) {
                result |= Get(inst.Arg(i));
            }
            return result;
        }
        case IR::Opcode::SelectU32:
            return Get(inst.Arg(1)) | Get(inst.Arg(2));
        case IR::Opcode::IAdd32:
            return binary(Add);
        case IR::Opcode::ISub32:
            return binary(Sub);
        case IR::Opcode::IMul32:
            return binary(Mul);
        case IR::Opcode::BitwiseAnd32:
            return binary(And);
        case IR::Opcode::BitwiseOr32:
            return binary(Or);
        case IR::Opcode::BitwiseXor32:
            return binary(Xor);
        case IR::Opcode::ShiftLeftLogical32: {
            const IR::Value shift{inst.Arg(1)};
            if (!shift.IsImmediate()) {
                return ALL_RESIDUES;
            }
            const u32 amount{shift.U32()};
            if (amount >= 32) {
                return ALL_RESIDUES;
            }
            if (amount >= 4) {
                return Residue(0);
            }
            ResidueMask result{};
            const ResidueMask source{Get(inst.Arg(0))};
            for (u32 value = 0; value < SOURCE_STRIDE; ++value) {
                if ((source & Residue(value)) != 0) {
                    result |= Residue(value << amount);
                }
            }
            return result;
        }
        default:
            // Unknown sources are fine when a parent operation subsequently proves alignment
            // (for example, local invocation ID multiplied by 160). Returning all low bits keeps
            // this analysis safe and intentionally conservative.
            return ALL_RESIDUES;
        }
    }

    std::unordered_map<const IR::Inst*, ResidueMask> cache;
    std::unordered_set<const IR::Inst*> visiting;
};

// Build direct use lists once instead of rescanning the shader for each vector load.
class SharedLoadUses {
public:
    explicit SharedLoadUses(const IR::Program& program) {
        for (const IR::Block* const block : program.blocks) {
            for (const IR::Inst& user : *block) {
                for (size_t index = 0; index < user.NumArgs(); ++index) {
                    const IR::Value arg{user.Arg(index)};
                    if (arg.IsIdentity() || !arg.IsImmediate()) {
                        uses[arg.Inst()].push_back({&user, index});
                    }
                }
            }
        }
    }

    [[nodiscard]] bool OnlyUsesXYZ(const IR::Inst* load) const {
        std::vector<const IR::Inst*> pending{load};
        std::unordered_set<const IR::Inst*> visited;
        while (!pending.empty()) {
            const IR::Inst* const value{pending.back()};
            pending.pop_back();
            if (!visited.insert(value).second) {
                return false;
            }
            const auto it = uses.find(value);
            const size_t observed_uses{it == uses.end() ? 0 : it->second.size()};
            // Also reject uses outside program.blocks (including associated/pseudo uses).
            if (observed_uses != static_cast<size_t>(value->UseCount())) {
                return false;
            }
            if (it == uses.end()) {
                continue;
            }
            for (const auto& [user, index] : it->second) {
                if (user->GetOpcode() == IR::Opcode::Identity && index == 0) {
                    pending.push_back(user);
                    continue;
                }
                if (user->GetOpcode() != IR::Opcode::CompositeExtractU32x4 || index != 0) {
                    return false;
                }
                const IR::Value component{user->Arg(1)};
                if (!component.IsImmediate() || component.U32() >= 3) {
                    return false;
                }
            }
        }
        return true;
    }

private:
    struct Use {
        const IR::Inst* user;
        size_t index;
    };
    std::unordered_map<const IR::Inst*, std::vector<Use>> uses;
};

[[nodiscard]] bool AccessFitsPackedLane(ResidueMask residues, u32 access_size) {
    if (residues == 0) {
        return false;
    }
    for (u32 residue = 0; residue < SOURCE_STRIDE; ++residue) {
        if ((residues & Residue(residue)) == 0) {
            continue;
        }
        // The explicit backend rounds vector addresses down to their natural alignment.
        // Require that alignment before switching to the scalar packed representation.
        if (residue % access_size != 0 || residue + access_size > PACKED_STRIDE) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsSafeAccess(const IR::Inst& inst, ResidueAnalyzer& residues,
                                const SharedLoadUses& uses) {
    const ResidueMask offset_residues{residues.Get(inst.Arg(0))};
    switch (inst.GetOpcode()) {
    case IR::Opcode::LoadSharedU8:
    case IR::Opcode::LoadSharedS8:
    case IR::Opcode::WriteSharedU8:
        return AccessFitsPackedLane(offset_residues, 1);
    case IR::Opcode::LoadSharedU16:
    case IR::Opcode::LoadSharedS16:
    case IR::Opcode::WriteSharedU16:
        return AccessFitsPackedLane(offset_residues, 2);
    case IR::Opcode::LoadSharedU32:
    case IR::Opcode::WriteSharedU32:
    case IR::Opcode::SharedAtomicIAdd32:
    case IR::Opcode::SharedAtomicSMin32:
    case IR::Opcode::SharedAtomicUMin32:
    case IR::Opcode::SharedAtomicSMax32:
    case IR::Opcode::SharedAtomicUMax32:
    case IR::Opcode::SharedAtomicInc32:
    case IR::Opcode::SharedAtomicDec32:
    case IR::Opcode::SharedAtomicAnd32:
    case IR::Opcode::SharedAtomicOr32:
    case IR::Opcode::SharedAtomicXor32:
    case IR::Opcode::SharedAtomicExchange32:
        return AccessFitsPackedLane(offset_residues, 4);
    case IR::Opcode::LoadSharedU64:
    case IR::Opcode::WriteSharedU64:
    case IR::Opcode::SharedAtomicExchange32x2:
        return AccessFitsPackedLane(offset_residues, 8);
    case IR::Opcode::SharedAtomicExchange64:
        // Packing intentionally disables explicit workgroup layout. On profiles with native
        // shared int64 atomics that would turn this operation into the backend's non-atomic
        // 2x32 fallback, changing guest semantics. Fail closed instead.
        return false;
    case IR::Opcode::LoadSharedU128:
        // A 16-byte guest load spans XYZ + a four-byte padding lane in the pattern this pass
        // handles. We may drop W only when the IR proves it is never observed.
        return offset_residues == Residue(0) && uses.OnlyUsesXYZ(&inst);
    case IR::Opcode::WriteSharedU128:
        // Dropping a written W component would change observable shared-memory state.
        return false;
    default:
        return false;
    }
}

[[nodiscard]] bool IsSharedAccess(IR::Opcode opcode) {
    switch (opcode) {
    case IR::Opcode::LoadSharedU8:
    case IR::Opcode::LoadSharedS8:
    case IR::Opcode::LoadSharedU16:
    case IR::Opcode::LoadSharedS16:
    case IR::Opcode::LoadSharedU32:
    case IR::Opcode::LoadSharedU64:
    case IR::Opcode::LoadSharedU128:
    case IR::Opcode::WriteSharedU8:
    case IR::Opcode::WriteSharedU16:
    case IR::Opcode::WriteSharedU32:
    case IR::Opcode::WriteSharedU64:
    case IR::Opcode::WriteSharedU128:
    case IR::Opcode::SharedAtomicIAdd32:
    case IR::Opcode::SharedAtomicSMin32:
    case IR::Opcode::SharedAtomicUMin32:
    case IR::Opcode::SharedAtomicSMax32:
    case IR::Opcode::SharedAtomicUMax32:
    case IR::Opcode::SharedAtomicInc32:
    case IR::Opcode::SharedAtomicDec32:
    case IR::Opcode::SharedAtomicAnd32:
    case IR::Opcode::SharedAtomicOr32:
    case IR::Opcode::SharedAtomicXor32:
    case IR::Opcode::SharedAtomicExchange32:
    case IR::Opcode::SharedAtomicExchange64:
    case IR::Opcode::SharedAtomicExchange32x2:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] u32 PackedSize(u32 source_size) {
    const u32 full_lanes{source_size / SOURCE_STRIDE};
    const u32 tail{source_size % SOURCE_STRIDE};
    const u32 bytes{full_lanes * PACKED_STRIDE + std::min(tail, PACKED_STRIDE)};
    // The SPIR-V backend allocates whole u32 words, including any partial tail.
    return (bytes + 3U) & ~3U;
}
} // Anonymous namespace

bool TryPackSharedMemory16To12(IR::Program& program, u32 max_shared_memory) {
    if (program.stage != Stage::Compute || program.shared_memory_size <= max_shared_memory ||
        program.shared_memory_16_to_12_packing) {
        return false;
    }
    const u32 packed_size{PackedSize(program.shared_memory_size)};
    if (packed_size >= program.shared_memory_size || packed_size > max_shared_memory) {
        return false;
    }

    ResidueAnalyzer residues;
    const SharedLoadUses uses{program};
    for (const IR::Block* const block : program.blocks) {
        for (const IR::Inst& inst : *block) {
            if (IsSharedAccess(inst.GetOpcode()) && !IsSafeAccess(inst, residues, uses)) {
                return false;
            }
        }
    }

    program.shared_memory_size = packed_size;
    program.shared_memory_16_to_12_packing = true;
    return true;
}

} // namespace Shader::Optimization
