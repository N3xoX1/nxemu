/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <functional>
#include <vector>

#include <mcl/stdint.hpp>
#include <xbyak/xbyak.h>

namespace Dynarmic::Backend::X64 {

using RegList = std::vector<Xbyak::Reg64>;

class BlockOfCode;

class Callback {
public:
    virtual ~Callback();

    void EmitCall(BlockOfCode& code) const {
        EmitCall(code, [](RegList) {});
    }

    virtual void EmitCall(BlockOfCode& code, std::function<void(RegList)> fn) const = 0;
    virtual void EmitCallWithReturnPointer(BlockOfCode& code, std::function<void(Xbyak::Reg64, RegList)> fn) const = 0;
};

class SimpleCallback final : public Callback {
public:
    template<typename Function>
    SimpleCallback(Function fn)
            : fn(reinterpret_cast<void (*)()>(fn)) {}

    using Callback::EmitCall;

    void EmitCall(BlockOfCode& code, std::function<void(RegList)> fn) const override;
    void EmitCallWithReturnPointer(BlockOfCode& code, std::function<void(Xbyak::Reg64, RegList)> fn) const override;

private:
    void (*fn)();
};

class ArgCallback final : public Callback {
public:
    template<typename Function>
    ArgCallback(Function fn, u64 arg)
            : fn(reinterpret_cast<void (*)()>(fn)), arg(arg) {}

    using Callback::EmitCall;

    void EmitCall(BlockOfCode& code, std::function<void(RegList)> fn) const override;
    void EmitCallWithReturnPointer(BlockOfCode& code, std::function<void(Xbyak::Reg64, RegList)> fn) const override;

    u64 Arg() const { return arg; }

    /// The same function, with its first argument loaded from the JIT state (r15) at the given
    /// offset when the call runs. The emitted code then does not depend on one core's objects.
    ArgCallback WithArgFromState(size_t state_offset) const {
        ArgCallback result = *this;
        result.arg = state_offset;
        result.arg_in_state = true;
        return result;
    }

private:
    void LoadArg(BlockOfCode& code, Xbyak::Reg64 reg) const;

    void (*fn)();
    u64 arg;
    bool arg_in_state = false;
};

}  // namespace Dynarmic::Backend::X64
