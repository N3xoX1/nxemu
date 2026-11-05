// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>

#include "yuzu_shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "yuzu_shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "yuzu_shader_recompiler/frontend/ir/modifiers.h"

namespace Shader::Backend::SPIRV {
namespace {
Id ExtractU16(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpUConvert(ctx.U16, value);
    } else {
        return ctx.OpBitFieldUExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(16u));
    }
}

Id ExtractS16(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpSConvert(ctx.S16, value);
    } else {
        return ctx.OpBitFieldSExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(16u));
    }
}

Id ExtractU8(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int8) {
        return ctx.OpUConvert(ctx.U8, value);
    } else {
        return ctx.OpBitFieldUExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(8u));
    }
}

Id ExtractS8(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int8) {
        return ctx.OpSConvert(ctx.S8, value);
    } else {
        return ctx.OpBitFieldSExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(8u));
    }
}
} // Anonymous namespace

Id EmitConvertS16F16(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpSConvert(ctx.U32[1], ctx.OpConvertFToS(ctx.U16, value));
    } else {
        return ExtractS16(ctx, ctx.OpConvertFToS(ctx.U32[1], value));
    }
}

Id EmitConvertS16F32(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpSConvert(ctx.U32[1], ctx.OpConvertFToS(ctx.U16, value));
    } else {
        return ExtractS16(ctx, ctx.OpConvertFToS(ctx.U32[1], value));
    }
}

Id EmitConvertS16F64(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpSConvert(ctx.U32[1], ctx.OpConvertFToS(ctx.U16, value));
    } else {
        return ExtractS16(ctx, ctx.OpConvertFToS(ctx.U32[1], value));
    }
}

Id EmitConvertS32F16(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U32[1], value);
}

Id EmitConvertS32F32(EmitContext& ctx, Id value) {
    if (ctx.profile.has_broken_signed_operations) {
        return ctx.OpBitcast(ctx.U32[1], ctx.OpConvertFToS(ctx.S32[1], value));
    } else {
        return ctx.OpConvertFToS(ctx.U32[1], value);
    }
}

Id EmitConvertS32F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U32[1], value);
}

Id EmitConvertS64F16(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U64, value);
}

Id EmitConvertS64F32(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U64, value);
}

Id EmitConvertS64F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U64, value);
}

Id EmitConvertU16F16(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpUConvert(ctx.U32[1], ctx.OpConvertFToU(ctx.U16, value));
    } else {
        return ExtractU16(ctx, ctx.OpConvertFToU(ctx.U32[1], value));
    }
}

Id EmitConvertU16F32(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpUConvert(ctx.U32[1], ctx.OpConvertFToU(ctx.U16, value));
    } else {
        return ExtractU16(ctx, ctx.OpConvertFToU(ctx.U32[1], value));
    }
}

Id EmitConvertU16F64(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpUConvert(ctx.U32[1], ctx.OpConvertFToU(ctx.U16, value));
    } else {
        return ExtractU16(ctx, ctx.OpConvertFToU(ctx.U32[1], value));
    }
}

Id EmitConvertU32F16(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U32[1], value);
}

Id EmitConvertU32F32(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U32[1], value);
}

Id EmitConvertU32F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U32[1], value);
}

Id EmitConvertU64F16(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U64, value);
}

Id EmitConvertU64F32(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U64, value);
}

Id EmitConvertU64F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U64, value);
}

Id EmitConvertU64U32(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U64, value);
}

Id EmitConvertU32U64(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U32[1], value);
}

namespace {
// When the shader cannot ask the host to keep fp16 denormals, OpFConvert may flush them.
// Rebuild them with integer ops, but only when the guest instruction does not ask to flush.
bool NeedsFp16DenormalEmulation(EmitContext& ctx, IR::Inst* inst) {
    if (!ctx.emulate_fp16_denormals) {
        return false;
    }
    const IR::FmzMode fmz_mode{inst->Flags<IR::FpControl>().fmz_mode};
    return fmz_mode != IR::FmzMode::FTZ && fmz_mode != IR::FmzMode::FMZ;
}
} // Anonymous namespace

Id EmitConvertF16F32(EmitContext& ctx, IR::Inst* inst, Id value) {
    const Id result{ctx.OpFConvert(ctx.F16[1], value)};
    if (!NeedsFp16DenormalEmulation(ctx, inst)) {
        return result;
    }
    if (const IR::Value arg{inst->Arg(0)}; arg.IsImmediate()) {
        // Constants such as 0.0 are common; skip the extra code when no denormal can appear.
        const f32 magnitude{std::fabs(arg.F32())};
        if (!(magnitude > 0.0f && magnitude < 0x1p-14f)) {
            return result;
        }
    }

    // The half subnormal range is sign | round-even(abs(value) * 2^24).
    // Avoid an out-of-range float-to-unsigned conversion in the unused path,
    // including for infinities and NaNs (which make the ordered comparison false).
    const Id magnitude{ctx.OpFAbs(ctx.F32[1], value)};
    const Id half_min_normal{ctx.Const(0x1p-14f)};
    const Id is_subnormal{ctx.OpFOrdLessThan(ctx.U1, magnitude, half_min_normal)};
    const Id bounded{ctx.OpSelect(ctx.F32[1], is_subnormal, magnitude, ctx.f32_zero_value)};
    const Id mantissa{ctx.OpConvertFToU(
        ctx.U32[1], ctx.OpRoundEven(ctx.F32[1],
                                   ctx.OpFMul(ctx.F32[1], bounded, ctx.Const(0x1p24f))))};
    const Id sign{ctx.OpBitwiseAnd(
        ctx.U32[1],
        ctx.OpShiftRightLogical(ctx.U32[1], ctx.OpBitcast(ctx.U32[1], value), ctx.Const(16u)),
        ctx.Const(0x8000u))};
    const Id packed{ctx.OpBitwiseOr(ctx.U32[1], sign, mantissa)};
    const Id encoded{ctx.OpCompositeExtract(ctx.F16[1], ctx.OpBitcast(ctx.F16[2], packed), 0u)};
    return ctx.OpSelect(ctx.F16[1], is_subnormal, encoded, result);
}

Id EmitConvertF32F16(EmitContext& ctx, IR::Inst* inst, Id value) {
    const Id result{ctx.OpFConvert(ctx.F32[1], value)};
    if (!NeedsFp16DenormalEmulation(ctx, inst)) {
        return result;
    }

    // All 1024 half subnormal magnitudes are exactly representable in float32.
    const Id packed{ctx.OpBitcast(ctx.U32[1], ctx.OpCompositeConstruct(ctx.F16[2], value, value))};
    const Id is_subnormal{ctx.OpIEqual(
        ctx.U1, ctx.OpBitwiseAnd(ctx.U32[1], packed, ctx.Const(0x7c00u)), ctx.u32_zero_value)};
    const Id mantissa{ctx.OpBitwiseAnd(ctx.U32[1], packed, ctx.Const(0x3ffu))};
    const Id magnitude{ctx.OpFMul(ctx.F32[1], ctx.OpConvertUToF(ctx.F32[1], mantissa),
                                  ctx.Const(0x1p-24f))};
    const Id negative{ctx.OpINotEqual(
        ctx.U1, ctx.OpBitwiseAnd(ctx.U32[1], packed, ctx.Const(0x8000u)), ctx.u32_zero_value)};
    const Id signed_magnitude{ctx.OpSelect(ctx.F32[1], negative,
                                           ctx.OpFNegate(ctx.F32[1], magnitude), magnitude)};
    return ctx.OpSelect(ctx.F32[1], is_subnormal, signed_magnitude, result);
}

Id EmitConvertF32F64(EmitContext& ctx, Id value) {
    return ctx.OpFConvert(ctx.F32[1], value);
}

Id EmitConvertF64F32(EmitContext& ctx, Id value) {
    return ctx.OpFConvert(ctx.F64[1], value);
}

Id EmitConvertF16S8(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F16[1], ExtractS8(ctx, value));
}

Id EmitConvertF16S16(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F16[1], ExtractS16(ctx, value));
}

Id EmitConvertF16S32(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F16[1], value);
}

Id EmitConvertF16S64(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F16[1], value);
}

Id EmitConvertF16U8(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F16[1], ExtractU8(ctx, value));
}

Id EmitConvertF16U16(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F16[1], ExtractU16(ctx, value));
}

Id EmitConvertF16U32(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F16[1], value);
}

Id EmitConvertF16U64(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F16[1], value);
}

Id EmitConvertF32S8(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F32[1], ExtractS8(ctx, value));
}

Id EmitConvertF32S16(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F32[1], ExtractS16(ctx, value));
}

Id EmitConvertF32S32(EmitContext& ctx, Id value) {
    if (ctx.profile.has_broken_signed_operations) {
        value = ctx.OpBitcast(ctx.S32[1], value);
    }
    return ctx.OpConvertSToF(ctx.F32[1], value);
}

Id EmitConvertF32S64(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F32[1], value);
}

Id EmitConvertF32U8(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], ExtractU8(ctx, value));
}

Id EmitConvertF32U16(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], ExtractU16(ctx, value));
}

Id EmitConvertF32U32(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], value);
}

Id EmitConvertF32U64(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], value);
}

Id EmitConvertF64S8(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F64[1], ExtractS8(ctx, value));
}

Id EmitConvertF64S16(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F64[1], ExtractS16(ctx, value));
}

Id EmitConvertF64S32(EmitContext& ctx, Id value) {
    if (ctx.profile.has_broken_signed_operations) {
        value = ctx.OpBitcast(ctx.S32[1], value);
    }
    return ctx.OpConvertSToF(ctx.F64[1], value);
}

Id EmitConvertF64S64(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F64[1], value);
}

Id EmitConvertF64U8(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], ExtractU8(ctx, value));
}

Id EmitConvertF64U16(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], ExtractU16(ctx, value));
}

Id EmitConvertF64U32(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], value);
}

Id EmitConvertF64U64(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], value);
}

} // namespace Shader::Backend::SPIRV
