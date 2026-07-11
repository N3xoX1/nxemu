#include "patch/patch_collection.h"

PatchCollection::PatchCollection(ISystemModules & modules, bool is_application) :
    m_modules(modules),
    m_is_application(is_application)
{
    std::fill(std::begin(m_module_patcher_indices), std::end(m_module_patcher_indices), -1);
#if defined(ARCHITECTURE_arm64) || defined(__aarch64__)
    m_patchers.emplace_back();
#endif
}

void PatchCollection::PatchText(int32_t patch_index, const uint8_t * program_image, uint32_t image_size, uint32_t code_offset, uint32_t code_size)
{
#if defined(ARCHITECTURE_arm64) || defined(__aarch64__)
    if (!g_settings->GetBool(NXCpuSetting::NceEnabled))
    {
        return;
    }

    Core::NCE::Patcher * patch = &m_patchers[patch_index];
    while (!patch->PatchText(program_image, image_size, code_offset, code_size))
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
#if defined(ARCHITECTURE_arm64) || defined(__aarch64__)
    if (!g_settings->GetBool(NXCpuSetting::NceEnabled))
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

    if (!patch.RelocateAndCopy(load_base, code_offset, code_size, program_image, image_size, &m_entry_trampolines))
    {
        RegisterPostTrampolines();
        return;
    }

    RegisterPostTrampolines();

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
#if defined(ARCHITECTURE_arm64) || defined(__aarch64__)
    if (!g_settings->GetBool(NXCpuSetting::NceEnabled))
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
#if defined(ARCHITECTURE_arm64) || defined(__aarch64__)
    if (!g_settings->GetBool(NXCpuSetting::NceEnabled))
    {
        return 0;
    }

    if (patch_index < 0 || patch_index >= m_patchers.size())
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
#if defined(ARCHITECTURE_arm64) || defined(__aarch64__)
    return (int32_t)(m_patchers.size()) - 1;
#else
    return 0;
#endif
}

void PatchCollection::SaveIndex(uint32_t module_index)
{
    if (module_index < (sizeof(m_module_patcher_indices) / sizeof(m_module_patcher_indices[0])))
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
