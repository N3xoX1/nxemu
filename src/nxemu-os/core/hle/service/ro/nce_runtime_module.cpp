// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/hle/service/ro/nce_runtime_module.h"
#include "core/hle/service/ro/ro_results.h"
#include "core/hle/kernel/k_page_table_base.h"
#include <nxemu-cpu/cpu_settings_identifiers.h>

extern IModuleSettings* g_settings;

#if defined(__APPLE__) && (defined(_M_ARM64) || defined(ARCHITECTURE_arm64))
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "yuzu_common/alignment.h"
#include "yuzu_common/scope_exit.h"

namespace Service::RO {
namespace {
constexpr uint64_t HostPage = 0x4000;

bool Executable(uint64_t begin, uint64_t size) {
    const auto end = begin + size;
    while (begin < end) {
        mach_vm_address_t region = begin;
        mach_vm_size_t length{};
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object{};
        const auto result = mach_vm_region(mach_task_self(), &region, &length,
            VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object);
        if (object) mach_port_deallocate(mach_task_self(), object);
        if (result != KERN_SUCCESS || region > begin ||
            (info.protection & (VM_PROT_READ | VM_PROT_EXECUTE)) != (VM_PROT_READ | VM_PROT_EXECUTE))
            return false;
        begin = region + length;
    }
    return true;
}
}

Result NceRuntimeModule::Prepare(std::unique_ptr<NceRuntimeModule>& out, Core::System& system,
    Kernel::KProcess& process, uint64_t base, uint64_t text_size, uint64_t total_size) {
    uint64_t starts[2], ends[2];
    if (!g_settings->GetBool(NXCpuSetting::NceEnabled) || !process.IsApplication() ||
        process.GetPageTable().GetNceWindows(starts, ends, 2) == 0)
        R_SUCCEED();
    auto module = std::unique_ptr<NceRuntimeModule>(new NceRuntimeModule(process, base, text_size));
    R_TRY(module->PrepareImpl(system, total_size));
    out = std::move(module);
    R_SUCCEED();
}

Result NceRuntimeModule::PrepareImpl(Core::System& system, uint64_t total_size) {
    auto& table = process.GetKPageTable().GetBasePageTable();
    reservations.reserve(3);
    const auto reserve = [&](uint64_t address, uint64_t size) -> Result {
        if (size) {
            R_TRY(table.ReserveNativeCode(address, size));
            reservations.push_back({address, size});
        }
        R_SUCCEED();
    };
    // Own whole host pages. An unrelated future mapping must not share their subpages.
    const auto begin = Common::AlignDown(base, HostPage);
    const auto end = Common::AlignUp(base + total_size, HostPage);
    R_TRY(reserve(begin, base - begin));
    R_TRY(reserve(base + total_size, end - base - total_size));
    R_TRY(table.MaterializeNativeCode(backing, base, total_size));

    std::vector<uint8_t> text(text_size);
    process.GetCoreMemory().ReadBlock(base, text.data(), text_size);
    patch = system.GetSystemModules().Cpu().CreateRuntimeCodePatch(text.data(), static_cast<uint32_t>(text_size),
        reinterpret_cast<uint64_t>(system.DeviceMemory().buffer.GetNceAtomicTable()));
    R_UNLESS(patch != nullptr, ResultInternalError);
    patch_size = patch->GetCodeSize();

    // Reserve a separate patch island before/after the module, then search outward.
    // Relocation checks the exact reachability of every branch before changing any text.
    const uint64_t candidates[] = {begin >= patch_size + HostPage ? begin - patch_size - HostPage : 0,
                                   end + HostPage};
    for (uint64_t distance = 0; distance < (1ULL << 27); distance += 0x200000) {
        for (size_t side = 0; side < 2; ++side) {
            const uint64_t address = side == 0
                ? (candidates[0] >= distance ? candidates[0] - distance : 0)
                : candidates[1] + distance;
            if (!address || R_FAILED(table.ReserveNativeCode(address, patch_size))) continue;
            if (!patch->Relocate(base, address)) {
                R_ASSERT(table.ReleaseNativeCode(address, patch_size));
                continue;
            }
            patch_base = address;
            reservations.push_back({address, patch_size});
            break;
        }
        if (patch_base) break;
    }
    R_UNLESS(patch_base != 0, ResultOutOfAddressSpace);

    mach_port_t entry{};
    memory_object_size_t size = patch_size;
    R_UNLESS(mach_make_memory_entry_64(mach_task_self(), &size, 0,
        MAP_MEM_NAMED_CREATE | VM_PROT_ALL, &entry, MACH_PORT_NULL) == KERN_SUCCESS, ResultInternalError);
    SCOPE_EXIT { mach_port_deallocate(mach_task_self(), entry); };
    mach_vm_address_t writer{};
    R_UNLESS(mach_vm_map(mach_task_self(), &writer, patch_size, 0, VM_FLAGS_ANYWHERE, entry, 0,
        FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_ALL, VM_INHERIT_NONE) == KERN_SUCCESS,
        ResultInternalError);
    SCOPE_EXIT { mach_vm_deallocate(mach_task_self(), writer, patch_size); };
    std::memcpy(reinterpret_cast<void*>(writer), patch->GetCode(), patch_size);
    sys_dcache_flush(reinterpret_cast<void*>(writer), patch_size);
    mach_vm_address_t destination = patch_base;
    R_UNLESS(mach_vm_map(mach_task_self(), &destination, patch_size, 0,
        VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, entry, 0, FALSE,
        VM_PROT_READ | VM_PROT_EXECUTE, VM_PROT_ALL, VM_INHERIT_NONE) == KERN_SUCCESS,
        ResultInternalError);
    mapped = true;
    sys_icache_invalidate(reinterpret_cast<void*>(patch_base), patch_size);

    uint32_t count;
    const auto* changes = patch->GetTextChanges(&count);
    text_changed = true;
    for (uint32_t i = 0; i < count; ++i)
        process.GetCoreMemory().Write32(base + changes[i].offset, changes[i].replacement);
    R_SUCCEED();
}

void NceRuntimeModule::SynchronizeText() {
    auto& memory = process.GetCoreMemory();
    for (uint64_t offset = 0; offset < text_size; offset += Kernel::PageSize)
        sys_dcache_flush(memory.GetPointer(base + offset), Kernel::PageSize);
    if (Executable(base, text_size)) {
        sys_icache_invalidate(reinterpret_cast<void*>(base), text_size);
    } else {
        // A failed preparation can leave only some host pages executable. Do not perform
        // cache maintenance on inaccessible VAs while rolling back a rejected mapping.
        for (uint64_t page = Common::AlignDown(base, HostPage); page < base + text_size; page += HostPage)
            if (Executable(page, HostPage)) sys_icache_invalidate(reinterpret_cast<void*>(page), HostPage);
    }
}

Result NceRuntimeModule::Publish() {
    // Check actual host permissions, including fragment boundaries. An inaccessible native
    // code page must never be reported as a successfully loaded executable module.
    if (!Executable(base, text_size)) {
        LOG_ERROR(Service_LDR, "NCE: NRO text at {:#x}, size {:#x} has incompatible host backing", base, text_size);
        R_THROW(ResultOutOfAddressSpace);
    }
    SynchronizeText();
    uint32_t handler_count, restart_count, atomic_count;
    const auto* handlers = patch->GetPostHandlers(&handler_count);
    const auto* restarts = patch->GetRestartPcs(&restart_count);
    const auto* atomics = patch->GetAtomicHandlers(&atomic_count);
    process.RuntimeCodeMetadata().Install(patch_base, {handlers, handler_count}, {restarts, restart_count},
                                          {atomics, atomic_count});
    published = true;
    // Publication and activation are seq_cst: either an in-flight activation sees this
    // snapshot, or this check sees activation and installs the new module's branches.
    process.PatchRuntimeAtomics();
    LOG_INFO(Service_LDR, "NCE: patched NRO text {:#x} ({:#x} bytes), trampolines {:#x} ({:#x} bytes)",
             base, text_size, patch_base, patch_size);
    R_SUCCEED();
}

NceRuntimeModule::~NceRuntimeModule() {
    if (published) process.RuntimeCodeMetadata().Remove(patch_base);
    if (text_changed) {
        uint32_t count;
        const auto* changes = patch->GetTextChanges(&count);
        for (uint32_t i = 0; i < count; ++i)
            process.GetCoreMemory().Write32(base + changes[i].offset, changes[i].original);
        SynchronizeText();
    }
    if (backing) {
        backing.reset();
        SynchronizeText();
    }
    if (mapped) {
        // Restore the NCE window's reservation, not an unreserved hole in host VA.
        mach_vm_address_t address = patch_base;
        const auto result = mach_vm_map(mach_task_self(), &address, patch_size, 0,
            VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, MEMORY_OBJECT_NULL, 0, FALSE,
            VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_NONE);
        ASSERT(result == KERN_SUCCESS);
    }
    auto& table = process.GetKPageTable().GetBasePageTable();
    for (auto it = reservations.rbegin(); it != reservations.rend(); ++it)
        R_ASSERT(table.ReleaseNativeCode(it->address, it->size));
    if (patch) patch->Release();
}
} // namespace Service::RO
#else
namespace Service::RO {
Result NceRuntimeModule::Prepare(std::unique_ptr<NceRuntimeModule>&, Core::System&,
    Kernel::KProcess&, uint64_t, uint64_t, uint64_t) { R_SUCCEED(); }
Result NceRuntimeModule::Publish() { R_SUCCEED(); }
NceRuntimeModule::~NceRuntimeModule() = default;
}
#endif
