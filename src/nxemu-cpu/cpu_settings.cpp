#include "cpu_settings.h"
#include "cpu_enum_strings.h"
#include <nxemu-cpu/cpu_settings_identifiers.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/module_setting_table.h>
#include <common/json.h>
#include <common/json_util.h>
#include <cstdlib>
#include <cstring>
#include <nxemu-module-spec/base.h>
#include <yuzu_common/logging/log.h>
#include <yuzu_common/yuzu_assert.h>

extern IModuleSettings * g_settings;

CpuSettings cpuSettings{};

namespace
{
enum class SettingType
{
    Boolean,
    CpuBackend,
    CpuAccuracy,
};

class CpuSetting
{
public:
    CpuSetting(const char * id, const char * section, const char * key, bool * val, bool defaultValue, bool debugSetting = false);
    CpuSetting(const char * id, const char * section, const char * key, CpuBackend * val, CpuBackend defaultValue, bool debugSetting = false);
    CpuSetting(const char * id, const char * section, const char * key, CpuAccuracy * val, CpuAccuracy defaultValue, bool debugSetting = false);
    CpuSetting(const char * id, const char * section, const char * key, CpuBackend * val, const char * fallbackId);
    CpuSetting(const char * id, const char * section, const char * key, CpuAccuracy * val, const char * fallbackId);

    const char * identifier;
    const char * json_section;
    const char * json_key;
    SettingType settingType;
    bool debugSetting;
    const char * fallback{nullptr};
    bool gameValue{false};
    union
    {
        bool boolValue;
        CpuBackend cpuBackend;
        CpuAccuracy cpuAccuracy;
    } defaults;
    union
    {
        bool * boolValue;
        CpuBackend * cpuBackend;
        CpuAccuracy * cpuAccuracy;
    } value;
};

static CpuSetting settings[] = {
#if defined(NXEMU_HAS_NCE)
    {NXCpuSetting::SystemCpuBackend, "cpu", "cpu_backend", &cpuSettings.global_cpu_backend, CpuBackend::Nce, true},
#else
    {NXCpuSetting::SystemCpuBackend, "cpu", "cpu_backend", &cpuSettings.global_cpu_backend, CpuBackend::Dynarmic, true},
#endif
    {NXCpuSetting::SystemCpuAccuracy, "cpu", "cpu_accuracy", &cpuSettings.global_cpu_accuracy, CpuAccuracy::Auto, true},
    {NXCpuSetting::CpuDebugMode, "cpu", "cpu_debug_mode", &cpuSettings.cpu_debug_mode, false},
    {NXCpuSetting::CpuoptPageTables, "cpu", "cpuopt_page_tables", &cpuSettings.cpuopt_page_tables, true},
    {NXCpuSetting::CpuoptBlockLinking, "cpu", "cpuopt_block_linking", &cpuSettings.cpuopt_block_linking, true},
    {NXCpuSetting::CpuoptReturnStackBuffer, "cpu", "cpuopt_return_stack_buffer", &cpuSettings.cpuopt_return_stack_buffer, true},
    {NXCpuSetting::CpuoptFastDispatcher, "cpu", "cpuopt_fast_dispatcher", &cpuSettings.cpuopt_fast_dispatcher, true},
    {NXCpuSetting::CpuoptContextElimination, "cpu", "cpuopt_context_elimination", &cpuSettings.cpuopt_context_elimination, true},
    {NXCpuSetting::CpuoptConstProp, "cpu", "cpuopt_const_prop", &cpuSettings.cpuopt_const_prop, true},
    {NXCpuSetting::CpuoptMiscIr, "cpu", "cpuopt_misc_ir", &cpuSettings.cpuopt_misc_ir, true},
    {NXCpuSetting::CpuoptReduceMisalignChecks, "cpu", "cpuopt_reduce_misalign_checks", &cpuSettings.cpuopt_reduce_misalign_checks, true},
    {NXCpuSetting::CpuoptFastmem, "cpu", "cpuopt_fastmem", &cpuSettings.cpuopt_fastmem, true},
    {NXCpuSetting::CpuoptFastmemExclusives, "cpu", "cpuopt_fastmem_exclusives", &cpuSettings.cpuopt_fastmem_exclusives, true},
    {NXCpuSetting::CpuoptRecompileExclusives, "cpu", "cpuopt_recompile_exclusives", &cpuSettings.cpuopt_recompile_exclusives, true},
    {NXCpuSetting::CpuoptIgnoreMemoryAborts, "cpu", "cpuopt_ignore_memory_aborts", &cpuSettings.cpuopt_ignore_memory_aborts, true},
    {NXCpuSetting::CpuoptUnsafeUnfuseFma, "cpu", "cpuopt_unsafe_unfuse_fma", &cpuSettings.cpuopt_unsafe_unfuse_fma, true},
    {NXCpuSetting::CpuoptUnsafeReduceFpError, "cpu", "cpuopt_unsafe_reduce_fp_error", &cpuSettings.cpuopt_unsafe_reduce_fp_error, true},
    {NXCpuSetting::CpuoptUnsafeIgnoreStandardFpcr, "cpu", "cpuopt_unsafe_ignore_standard_fpcr", &cpuSettings.cpuopt_unsafe_ignore_standard_fpcr, true},
    {NXCpuSetting::CpuoptUnsafeInaccurateNan, "cpu", "cpuopt_unsafe_inaccurate_nan", &cpuSettings.cpuopt_unsafe_inaccurate_nan, true},
    {NXCpuSetting::CpuoptUnsafeFastmemCheck, "cpu", "cpuopt_unsafe_fastmem_check", &cpuSettings.cpuopt_unsafe_fastmem_check, true},
    {NXCpuSetting::CpuoptUnsafeIgnoreGlobalMonitor, "cpu", "cpuopt_unsafe_ignore_global_monitor", &cpuSettings.cpuopt_unsafe_ignore_global_monitor, true},
    {NXCpuSetting::GameCpuBackend, "cpu", "cpu_backend", &cpuSettings.cpu_backend, NXCpuSetting::SystemCpuBackend},
    {NXCpuSetting::GameCpuAccuracy, "cpu", "cpu_accuracy", &cpuSettings.cpu_accuracy, NXCpuSetting::SystemCpuAccuracy},
};

CpuSetting::CpuSetting(const char * id, const char * section, const char * key, bool * val, bool defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::Boolean),
    debugSetting(debugSetting)
{
    defaults.boolValue = defaultValue;
    value.boolValue = val;
}

CpuSetting::CpuSetting(const char * id, const char * section, const char * key, CpuBackend * val, CpuBackend defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::CpuBackend),
    debugSetting(debugSetting)
{
    defaults.cpuBackend = defaultValue;
    value.cpuBackend = val;
}

CpuSetting::CpuSetting(const char * id, const char * section, const char * key, CpuAccuracy * val, CpuAccuracy defaultValue, bool debugSetting) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::CpuAccuracy),
    debugSetting(debugSetting)
{
    defaults.cpuAccuracy = defaultValue;
    value.cpuAccuracy = val;
}

CpuSetting::CpuSetting(const char * id, const char * section, const char * key, CpuBackend * val, const char * fallbackId) :
    CpuSetting(id, section, key, val, CpuBackend{})
{
    fallback = fallbackId;
}

CpuSetting::CpuSetting(const char * id, const char * section, const char * key, CpuAccuracy * val, const char * fallbackId) :
    CpuSetting(id, section, key, val, CpuAccuracy{})
{
    fallback = fallbackId;
}

} // namespace

bool IsFastmemEnabled()
{
    if (cpuSettings.cpu_debug_mode)
    {
        return cpuSettings.cpuopt_fastmem;
    }
    return true;
}

#if defined(NXEMU_HAS_NCE) && defined(__APPLE__)
// XNU keeps x18 across exceptions for processes whose main executable declares an SDK older
// than 13.0, and flags them with bit 48 of TPIDR_EL0. Guest code uses x18 as a scratch register.
static bool HostPreservesX18()
{
    uint64_t tpidr_el0;
    asm volatile("mrs %0, tpidr_el0" : "=r"(tpidr_el0));
    return ((tpidr_el0 >> 48) & 1) != 0;
}
#endif

void UpdateNceEnabled()
{
    const bool is_64bit = g_settings->GetBool(NXCoreSetting::Is64BitProgram);
    bool supported_space = g_settings->GetBool(NXCoreSetting::Has39BitAddressSpace);
#if defined(__APPLE__)
    // Experimental: the 36-bit code region is relocated above Darwin hard page zero.
    const char* enable_36bit = std::getenv("NXEMU_NCE_36BIT");
    supported_space |= g_settings->GetBool(NXCoreSetting::Has36BitAddressSpace) &&
                       enable_36bit && std::strcmp(enable_36bit, "1") == 0;
#endif
#if defined(NXEMU_HAS_NCE)
    bool is_nce = g_settings->GetInt(NXCpuSetting::GameCpuBackend) == static_cast<int32_t>(CpuBackend::Nce);
#if defined(__APPLE__)
    if (is_nce && !HostPreservesX18())
    {
        LOG_WARNING(Common, "The host does not preserve x18, unable to natively execute code");
        is_nce = false;
    }
#endif
#else
    const bool is_nce = false;
#endif
    if (is_nce && !IsFastmemEnabled())
    {
        LOG_WARNING(Common, "Fastmem is required to natively execute code in a performant manner, falling back to Dynarmic");
    }
    if (is_nce && is_64bit && !supported_space)
    {
        LOG_WARNING(Common, "Program address space is not supported by NCE, falling back to Dynarmic");
    }
    g_settings->SetBool(NXCpuSetting::NceEnabled, IsFastmemEnabled() && is_nce && is_64bit && supported_space);
}

bool AffectsNceEnabled(const char * setting)
{
    return strcmp(setting, NXCpuSetting::GameCpuBackend) == 0 ||
           strcmp(setting, NXCpuSetting::CpuDebugMode) == 0 ||
           strcmp(setting, NXCpuSetting::CpuoptFastmem) == 0 ||
           strcmp(setting, NXCoreSetting::Has39BitAddressSpace) == 0 ||
           strcmp(setting, NXCoreSetting::Has36BitAddressSpace) == 0 ||
           strcmp(setting, NXCoreSetting::Is64BitProgram) == 0;
}

namespace
{
void UseAvailableBackend(CpuBackend & backend)
{
#if !defined(NXEMU_HAS_NCE)
    if (backend == CpuBackend::Nce)
    {
        LOG_WARNING(Common, "NCE is unavailable on this host; using Dynarmic");
        backend = CpuBackend::Dynarmic;
    }
#else
    (void)backend;
#endif
}

StoreKind StoreKindOf(const CpuSetting & cpuSetting)
{
    return cpuSetting.settingType == SettingType::Boolean ? StoreKind::Bool : StoreKind::Int;
}

bool IsPersisted(const CpuSetting & /*cpuSetting*/)
{
    return true;
}

JsonValue JsonSettingValue(const CpuSetting & cpuSetting, const JsonValue & root)
{
    const JsonValue & section = root[cpuSetting.json_section];
    return section.isObject() ? section[cpuSetting.json_key] : JsonValue();
}

void SetJsonSettingValue(const CpuSetting & cpuSetting, JsonValue & root, const JsonValue & value)
{
    root[cpuSetting.json_section][cpuSetting.json_key] = value;
}

void InheritFallback(CpuSetting & cpuSetting, const CpuSetting & fallbackSetting)
{
    cpuSetting.defaults = fallbackSetting.defaults;
    cpuSetting.debugSetting = fallbackSetting.debugSetting;
}

void LoadStoreSetting(const CpuSetting & cpuSetting, const char * setting)
{
    switch (cpuSetting.settingType)
    {
    case SettingType::Boolean:
        *cpuSetting.value.boolValue = g_settings->GetBool(setting);
        break;
    case SettingType::CpuBackend:
        *cpuSetting.value.cpuBackend = static_cast<CpuBackend>(g_settings->GetInt(setting));
        break;
    case SettingType::CpuAccuracy:
        *cpuSetting.value.cpuAccuracy = static_cast<CpuAccuracy>(g_settings->GetInt(setting));
        break;
    default:
        UNIMPLEMENTED();
    }
}

void ApplyDefault(const CpuSetting & cpuSetting)
{
    switch (cpuSetting.settingType)
    {
    case SettingType::Boolean:
        *cpuSetting.value.boolValue = cpuSetting.defaults.boolValue;
        break;
    case SettingType::CpuBackend:
        *cpuSetting.value.cpuBackend = cpuSetting.defaults.cpuBackend;
        break;
    case SettingType::CpuAccuracy:
        *cpuSetting.value.cpuAccuracy = cpuSetting.defaults.cpuAccuracy;
        break;
    default:
        UNIMPLEMENTED();
    }
}

void ReadJsonSetting(const CpuSetting & cpuSetting, const JsonValue & root)
{
    const JsonValue value = JsonSettingValue(cpuSetting, root);
    switch (cpuSetting.settingType)
    {
    case SettingType::Boolean:
        if (value.isBool())
        {
            *cpuSetting.value.boolValue = value.asBool();
        }
        break;
    case SettingType::CpuBackend:
        if (value.isString())
        {
            *cpuSetting.value.cpuBackend = CpuBackendFromString(value.asString());
            UseAvailableBackend(*cpuSetting.value.cpuBackend);
        }
        break;
    case SettingType::CpuAccuracy:
        if (value.isString())
        {
            *cpuSetting.value.cpuAccuracy = CpuAccuracyFromString(value.asString());
        }
        break;
    default:
        UNIMPLEMENTED();
    }
}

void StoreSetting(const CpuSetting & cpuSetting)
{
    switch (cpuSetting.settingType)
    {
    case SettingType::Boolean:
        g_settings->SetDefaultBool(cpuSetting.identifier, cpuSetting.defaults.boolValue);
        g_settings->SetBool(cpuSetting.identifier, *cpuSetting.value.boolValue);
        break;
    case SettingType::CpuBackend:
        g_settings->SetDefaultInt(cpuSetting.identifier, static_cast<int32_t>(cpuSetting.defaults.cpuBackend));
        g_settings->SetInt(cpuSetting.identifier, static_cast<int32_t>(*cpuSetting.value.cpuBackend));
        break;
    case SettingType::CpuAccuracy:
        g_settings->SetDefaultInt(cpuSetting.identifier, static_cast<int32_t>(cpuSetting.defaults.cpuAccuracy));
        g_settings->SetInt(cpuSetting.identifier, static_cast<int32_t>(*cpuSetting.value.cpuAccuracy));
        break;
    default:
        UNIMPLEMENTED();
    }
}

void WriteJsonSetting(const CpuSetting & cpuSetting, JsonValue & root, bool always)
{
    switch (cpuSetting.settingType)
    {
    case SettingType::Boolean:
        if (always || *cpuSetting.value.boolValue != cpuSetting.defaults.boolValue)
        {
            SetJsonSettingValue(cpuSetting, root, *cpuSetting.value.boolValue);
        }
        break;
    case SettingType::CpuBackend:
        if (always || *cpuSetting.value.cpuBackend != cpuSetting.defaults.cpuBackend)
        {
            SetJsonSettingValue(cpuSetting, root, CpuBackendToString(*cpuSetting.value.cpuBackend));
        }
        break;
    case SettingType::CpuAccuracy:
        if (always || *cpuSetting.value.cpuAccuracy != cpuSetting.defaults.cpuAccuracy)
        {
            SetJsonSettingValue(cpuSetting, root, CpuAccuracyToString(*cpuSetting.value.cpuAccuracy));
        }
        break;
    default:
        UNIMPLEMENTED();
    }
}
} // namespace

void CpuSettingChanged(const char * setting, void * /*userData*/)
{
    ModuleSettingTable::SettingChanged(settings, setting);
    if (AffectsNceEnabled(setting))
    {
        UpdateNceEnabled();
    }
}

void NceInputChanged(const char * setting, void * /*userData*/)
{
    if (AffectsNceEnabled(setting))
    {
        UpdateNceEnabled();
    }
}

namespace
{
void GameSettingsChanged(const char * /*setting*/, void * /*userData*/)
{
    ModuleSettingTable::LoadGameSettings(settings, "nxemu-cpu");
    UpdateNceEnabled();
}
} // namespace

void SetupCpuSetting(void)
{
    ModuleSettingTable::SetupSettings(settings, "nxemu-cpu", CpuSettingChanged, GameSettingsChanged);

    g_settings->RegisterCallback(NXCoreSetting::Has39BitAddressSpace, NceInputChanged, nullptr);
    g_settings->RegisterCallback(NXCoreSetting::Has36BitAddressSpace, NceInputChanged, nullptr);
    g_settings->RegisterCallback(NXCoreSetting::Is64BitProgram, NceInputChanged, nullptr);

    g_settings->SetDefaultBool(NXCpuSetting::NceEnabled, false);
    UpdateNceEnabled();
}

void CleanupCpuSetting(void)
{
    ModuleSettingTable::CleanupSettings(settings, CpuSettingChanged, GameSettingsChanged);
    g_settings->UnregisterCallback(NXCoreSetting::Has39BitAddressSpace, NceInputChanged, nullptr);
    g_settings->UnregisterCallback(NXCoreSetting::Has36BitAddressSpace, NceInputChanged, nullptr);
    g_settings->UnregisterCallback(NXCoreSetting::Is64BitProgram, NceInputChanged, nullptr);
}

void SaveCpuSettings(void)
{
    ModuleSettingTable::SaveSettings(settings, "nxemu-cpu");
}
