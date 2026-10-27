// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <vector>
#include "core/hle/result.h"
#include <nxemu-module-spec/cpu.h>

namespace Core { class System; }
namespace Kernel { class KProcess; class NativeCodeBacking; }

namespace Service::RO {
class NceRuntimeModule {
public:
    static Result Prepare(std::unique_ptr<NceRuntimeModule>& out, Core::System& system,
                          Kernel::KProcess& process, uint64_t base, uint64_t text_size,
                          uint64_t total_size);
    Result Publish();
    ~NceRuntimeModule();
private:
    explicit NceRuntimeModule(Kernel::KProcess& process, uint64_t base, uint64_t text_size)
        : process(process), base(base), text_size(text_size) {}
    Result PrepareImpl(Core::System& system, uint64_t total_size);
    void SynchronizeText();
    Kernel::KProcess& process;
    uint64_t base{}, text_size{}, patch_base{}, patch_size{};
    struct Range { uint64_t address, size; };
    std::vector<Range> reservations;
    IRuntimeCodePatch* patch{};
    std::unique_ptr<Kernel::NativeCodeBacking> backing;
    bool text_changed{}, mapped{}, published{};
};
} // namespace Service::RO
