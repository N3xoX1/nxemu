#pragma once
#include <nxemu-module-spec/cpu.h>

#if defined(ARCHITECTURE_arm64) || defined(__aarch64__)
#undef NOMINMAX
#include "nce/patcher.h"
#endif

class PatchCollection final :
    public IPatchCollection
{
public:
    PatchCollection(ISystemModules & modules, bool is_application);

    void PatchText(int32_t patch_index, const uint8_t * program_image, uint32_t image_size, uint32_t code_offset, uint32_t code_size) override;
    void Relocate(int32_t patch_index, uint64_t load_base, uint8_t * program_image, uint32_t * image_size, uint32_t code_offset, uint32_t code_size, uint64_t * segment_addr, uint32_t * segment_size) override;
    uint32_t GetTotalPatchSize() const override;
    uint32_t GetPreTextSize(int32_t patch_index) const override;
    int32_t GetLastIndex() const override;
    void SaveIndex(uint32_t module_index) override;
    int32_t GetIndex(uint32_t module_index) const override;
    void Release() override;

private:
    ISystemModules & m_modules;
    bool m_is_application;
    int32_t m_module_patcher_indices[13];
#if defined(ARCHITECTURE_arm64) || defined(__aarch64__)
    std::vector<Core::NCE::Patcher> m_patchers;
#endif
};
