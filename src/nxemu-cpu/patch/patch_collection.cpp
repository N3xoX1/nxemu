#include "patch/patch_collection.h"
#include <nxemu-cpu/cpu_settings_identifiers.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-module-spec/base.h>
#include <nxemu-module-spec/operating_system.h>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <charconv>
#include <string_view>
#include <iterator>

extern IModuleSettings * g_settings;

PatchCollection::PatchCollection(ISystemModules & modules, bool is_application) :
    m_modules(modules),
    m_is_application(is_application)
{
    std::fill(std::begin(m_module_patcher_indices), std::end(m_module_patcher_indices), -1);
#if defined(NXEMU_HAS_NCE)
    m_patchers.emplace_back();
#endif
}

void PatchCollection::PatchText(int32_t patch_index, const uint8_t * program_image, uint32_t image_size, uint32_t code_offset, uint32_t code_size)
{
#if defined(NXEMU_HAS_NCE)
    if (!m_is_application || !g_settings->GetBool(NXCpuSetting::NceEnabled))
    {
        return;
    }

    std::vector<u32> read_filter;
    const char* requested_filter = std::getenv("NXEMU_NCE_READ_INSTRUCTIONS");
    if (requested_filter) {
        std::string_view remaining{requested_filter};
        while (!remaining.empty()) {
            const size_t comma = remaining.find(',');
            const auto word = remaining.substr(0, comma);
            u32 instruction{};
            const auto result = std::from_chars(word.data(), word.data() + word.size(), instruction, 16);
            if (result.ec == std::errc{} && result.ptr == word.data() + word.size())
                read_filter.push_back(instruction);
            if (comma == std::string_view::npos) break;
            remaining.remove_prefix(comma + 1);
        }
        std::fprintf(stderr, "[NCE-READ-THUNKS] filter_words=%zu\n", read_filter.size());
    }
    Core::NCE::Patcher * patch = &m_patchers[patch_index];
    while (!patch->PatchText(program_image, image_size, code_offset, code_size
#if defined(__APPLE__)
                             , 1, g_settings->GetBool(NXCoreSetting::Has36BitAddressSpace) &&
                                  m_module_patcher_indices[0] < 0,
                                  g_settings->GetBool(NXCoreSetting::Has36BitAddressSpace),
                                  std::getenv("NXEMU_NCE_READ_THUNKS") != nullptr,
                                  std::getenv("NXEMU_NCE_ALL_READ_BASES") != nullptr &&
                                      (!requested_filter || !read_filter.empty()), read_filter,
                                  std::getenv("NXEMU_NCE_FLOAT_READ_THUNKS") != nullptr &&
                                      (!requested_filter || !read_filter.empty()),
                                  std::getenv("NXEMU_NCE_STORE_THUNKS") != nullptr &&
                                      (!requested_filter || !read_filter.empty())
                             // The table address is known after process creation, at relocation.
#endif
                             ))
    {
        patch = &m_patchers.emplace_back();
    }
#else
    (void)patch_index;
    (void)program_image;
    (void)image_size;
    (void)code_offset;
    (void)code_size;
#endif
}

void PatchCollection::Relocate(int32_t patch_index, uint64_t load_base, uint8_t * program_image, uint32_t * image_size, uint32_t code_offset, uint32_t code_size, uint64_t * segment_addr, uint32_t * segment_size)
{
#if defined(NXEMU_HAS_NCE)
    if (!m_is_application || !g_settings->GetBool(NXCpuSetting::NceEnabled))
    {
        return;
    }

    if (segment_addr != nullptr)
    {
        *segment_addr = 0;
    }
    if (segment_size != nullptr)
    {
        *segment_size = 0;
    }

    Core::NCE::Patcher & patch = m_patchers[patch_index];
    const uint32_t image_size_before_relocate = *image_size;
    uint64_t image_size64 = *image_size;
    Core::NCE::EntryTrampolines trampolines, atomics, reads;
#if defined(__APPLE__)
    if (auto* process = m_modules.OperatingSystem().CurrentProcess()) {
        patch.SetAtomicTable(process->GetPageTable().GetNceAtomicTable());
        u64 starts[2]{}, ends[2]{};
        if (g_settings->GetBool(NXCoreSetting::Has36BitAddressSpace) &&
            process->GetPageTable().GetNceWindows(starts, ends, 2) == 2)
            patch.SetLegacy36BitRegion(starts[0] + 0x8000000ULL, ends[0]);
    }
#endif
    Core::NCE::RestartPcs restart_pcs;

    const bool copied = patch.RelocateAndCopy(load_base, code_offset, code_size, program_image, &image_size64, &trampolines, &restart_pcs, &atomics, &reads);
    *image_size = (uint32_t)image_size64;

    if (IKernelProcess * process = m_modules.OperatingSystem().CurrentProcess())
    {
        for (Core::NCE::EntryTrampolines::const_iterator it = trampolines.begin(); it != trampolines.end(); ++it)
        {
            process->RegisterPostHandler(it->first, it->second);
        }
        for (const auto& [pc, target] : atomics) process->RegisterNceAtomicHandler(pc, target);
        for (const auto& [pc, target] : reads) process->RegisterNceReadHandler(pc, target);
        if (!reads.empty())
            std::fprintf(stderr, "[NCE-READ-THUNKS] registered=%zu patch_KiB=%.3f\n",
                         reads.size(), patch.GetSectionSize() / 1024.0);
        for (const auto & [interrupted_pc, restart_pc] : restart_pcs)
        {
            process->RegisterNceRestartPc(interrupted_pc, restart_pc);
        }
    }

    if (!copied)
    {
        return;
    }

    if (segment_addr != nullptr)
    {
        *segment_addr = patch.GetPatchMode() == Core::NCE::PatchMode::PreText ? 0ULL : (uint64_t)image_size_before_relocate;
    }
    if (segment_size != nullptr)
    {
        *segment_size = (uint32_t)patch.GetSectionSize();
    }
#else
    (void)patch_index;
    (void)load_base;
    (void)program_image;
    (void)image_size;
    (void)code_offset;
    (void)code_size;
    (void)segment_addr;
    (void)segment_size;
#endif
}

uint32_t PatchCollection::GetTotalPatchSize() const
{
#if defined(NXEMU_HAS_NCE)
    if (!m_is_application || !g_settings->GetBool(NXCpuSetting::NceEnabled))
    {
        return 0;
    }

    uint32_t total_size = 0;
    for (const Core::NCE::Patcher & patcher : m_patchers)
    {
        total_size += static_cast<uint32_t>(patcher.GetSectionSize());
    }
    return total_size;
#else
    return 0;
#endif
}

uint32_t PatchCollection::GetPreTextSize(int32_t patch_index) const
{
#if defined(NXEMU_HAS_NCE)
    if (!m_is_application || !g_settings->GetBool(NXCpuSetting::NceEnabled))
    {
        return 0;
    }

    if (patch_index < 0 || patch_index >= (int32_t)m_patchers.size())
    {
        return 0;
    }

    const Core::NCE::Patcher & patcher = m_patchers[patch_index];
    if (patcher.GetPatchMode() == Core::NCE::PatchMode::PreText)
    {
        return (uint32_t)patcher.GetSectionSize();
    }
#else
    (void)patch_index;
#endif
    return 0;
}

int32_t PatchCollection::GetLastIndex() const
{
#if defined(NXEMU_HAS_NCE)
    return (int32_t)(m_patchers.size()) - 1;
#else
    return 0;
#endif
}

void PatchCollection::SaveIndex(uint32_t module_index)
{
    if (module_index < std::size(m_module_patcher_indices))
    {
        m_module_patcher_indices[module_index] = GetLastIndex();
    }
}

int32_t PatchCollection::GetIndex(uint32_t module_index) const
{
    if (module_index >= std::size(m_module_patcher_indices))
    {
        return -1;
    }
    return m_module_patcher_indices[module_index];
}

void PatchCollection::Release()
{
    delete this;
}
