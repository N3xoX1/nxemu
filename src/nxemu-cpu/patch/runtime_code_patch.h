// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nce/patcher.h"
#include <cstring>
#include <nxemu-module-spec/cpu.h>

class RuntimeCodePatch final : public IRuntimeCodePatch {
public:
    RuntimeCodePatch(const uint8_t* text, uint32_t size, uint64_t atomic_table = 0) : original(text, text + size) {
        patcher.PatchText(text, size, 0, size, atomic_table);
        if (atomic_table) patcher.SetAtomicTable(atomic_table);
    }
    uint64_t GetCodeSize() const override { return patcher.GetSectionSize(); }
    bool Relocate(uint64_t text_base, uint64_t patch_base) override {
        if (relocated) return false;
        auto text = original;
        Core::NCE::EntryTrampolines entries, atomic_entries;
        Core::NCE::RestartPcs restarts;
        if (!patcher.RelocateModule(text_base, patch_base, text.data(), text.size(), entries, restarts, &atomic_entries))
            return false;
        for (size_t offset = 0; offset < text.size(); offset += sizeof(u32)) {
            u32 before, after;
            std::memcpy(&before, original.data() + offset, sizeof(before));
            std::memcpy(&after, text.data() + offset, sizeof(after));
            if (before != after || atomic_entries.contains(text_base + offset)) changes.push_back({static_cast<u32>(offset), before, after});
        }
        for (const auto& [pc, target] : entries) handlers.push_back({pc, target});
        for (const auto& [pc, target] : restarts) restart_pcs.push_back({pc, target});
        for (const auto& [pc, target] : atomic_entries) atomics.push_back({pc, target});
        code.resize(GetCodeSize());
        const auto& words = patcher.GetInstructions();
        std::memcpy(code.data(), words.data(), words.size() * sizeof(u32));
        std::vector<u8>().swap(original);
        relocated = true;
        return true;
    }
    const uint8_t* GetCode() const override { return code.data(); }
    const NativeCodeWordPatch* GetTextChanges(uint32_t* count) const override {
        *count = static_cast<uint32_t>(changes.size()); return changes.data();
    }
    const NativeCodeEntry* GetPostHandlers(uint32_t* count) const override {
        *count = static_cast<uint32_t>(handlers.size()); return handlers.data();
    }
    const NativeCodeEntry* GetRestartPcs(uint32_t* count) const override {
        *count = static_cast<uint32_t>(restart_pcs.size()); return restart_pcs.data();
    }
    const NativeCodeEntry* GetAtomicHandlers(uint32_t* count) const override {
        *count = static_cast<uint32_t>(atomics.size()); return atomics.data();
    }
    void Release() override { delete this; }
private:
    Core::NCE::Patcher patcher;
    std::vector<u8> original, code;
    std::vector<NativeCodeWordPatch> changes;
    std::vector<NativeCodeEntry> handlers, restart_pcs, atomics;
    bool relocated{};
};
