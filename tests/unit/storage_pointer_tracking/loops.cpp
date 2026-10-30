// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "yuzu_shader_recompiler/frontend/ir/ir_emitter.h"
#include "yuzu_shader_recompiler/frontend/ir/program.h"
#include "yuzu_shader_recompiler/host_translate_info.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"

namespace IR = Shader::IR;
struct Fixture {
    Shader::ObjectPool<IR::Inst> pool{128};
    IR::Block block{pool};
    IR::IREmitter ir{block};
    IR::Program program;
    Fixture() { program.post_order_blocks.push_back(&block); }
    IR::U32 Base(u32 offset = 0x650) {
        return ir.GetCbuf(ir.Imm32(0), ir.Imm32(offset));
    }
    IR::Inst& Phi() {
        block.AppendNewInst(IR::Opcode::Phi, {});
        auto& phi = block.Instructions().back();
        phi.SetFlags(IR::Type::U32);
        return phi;
    }
    IR::U32 Value(IR::Inst& inst) { return IR::U32{IR::Value{&inst}}; }
    IR::U32 Identity(IR::Inst& inst) {
        block.AppendNewInst(IR::Opcode::Identity, {IR::Value{&inst}});
        return Value(block.Instructions().back());
    }
    void Incoming(IR::Inst& phi, const IR::U32& value) {
        phi.AddPhiOperand(&block, value);
    }
    bool Run(const IR::U32& low) {
        const auto address = ir.PackUint2x32(ir.CompositeConstruct(low, Base(0x654)));
        (void)ir.GlobalAtomicIAdd(address, ir.Imm32(1));
        Shader::HostTranslateInfo host{};
        host.min_ssbo_alignment = 16;
        Shader::Optimization::GlobalMemoryToStorageBufferPass(program, host);
        unsigned storage{}, global{};
        for (const auto& inst : block.Instructions()) {
            storage += inst.GetOpcode() == IR::Opcode::StorageAtomicIAdd32;
            global += inst.GetOpcode() == IR::Opcode::GlobalAtomicIAdd32;
        }
        if (storage == 1 && global == 0) {
            const auto& descriptors = program.info.storage_buffers_descriptors;
            return descriptors.size() == 1 && descriptors[0].cbuf_index == 0 &&
                   descriptors[0].cbuf_offset == 0x650 && descriptors[0].is_written;
        }
        return false;
    }
};

int main() {
    unsigned failures{};
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
    };
    { Fixture f; check(f.Run(f.Base()), "direct base remains supported"); }
    for (bool identity : {false, true}) {
        Fixture f;
        const auto base = f.Base();
        auto& phi = f.Phi();
        f.Incoming(phi, base);
        f.Incoming(phi, identity ? f.Identity(phi) : f.Value(phi));
        check(f.Run(f.Value(phi)), "unchanged loop pointer retains its descriptor");
    }
    {
        Fixture f;
        const auto base = f.Base();
        auto& loop = f.Phi();
        f.Incoming(loop, base);
        f.Incoming(loop, f.Identity(loop));
        auto& outer = f.Phi();
        f.Incoming(outer, base);
        f.Incoming(outer, f.Value(loop));
        check(f.Run(f.Value(outer)), "outer merge agrees with invariant loop base");
    }
    {
        Fixture f; auto& phi = f.Phi();
        f.Incoming(phi, f.Value(phi));
        check(!f.Run(f.Value(phi)), "unanchored loop is rejected");
    }
    {
        Fixture f; const auto base = f.Base(); const auto other = f.Base(0x660);
        auto& phi = f.Phi();
        f.Incoming(phi, base); f.Incoming(phi, other); f.Incoming(phi, f.Value(phi));
        check(!f.Run(f.Value(phi)), "different descriptors remain rejected");
    }
    {
        Fixture f; const auto base = f.Base(); auto& phi = f.Phi();
        f.Incoming(phi, base);
        f.Incoming(phi, f.ir.IAdd(f.Value(phi), f.ir.Imm32(4)));
        check(!f.Run(f.Value(phi)), "changed loop pointer remains rejected");
    }
    {
        Fixture f; const auto base = f.Base(); auto& a = f.Phi(); auto& b = f.Phi();
        f.Incoming(a, base); f.Incoming(a, f.Value(b));
        f.Incoming(b, f.Value(a));
        check(!f.Run(f.Value(a)), "mutual recursion remains conservative");
    }
    {
        Fixture f; const auto base = f.Base();
        const auto address = f.ir.PackUint2x32(f.ir.CompositeConstruct(base, f.Base(0x654)));
        const auto loaded = f.ir.LoadGlobal32(address);
        auto& phi = f.Phi();
        f.Incoming(phi, base); f.Incoming(phi, loaded); f.Incoming(phi, f.Value(phi));
        check(!f.Run(f.Value(phi)), "pointer loaded from memory is not its source descriptor");
    }
    return failures ? 1 : 0;
}
