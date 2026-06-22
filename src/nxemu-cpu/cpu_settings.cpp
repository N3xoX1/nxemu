#include "cpu_settings.h"
#include "cpu_enum_strings.h"
#include <nxemu-cpu/cpu_settings_identifiers.h>
#include <nxemu-loader/loader_settings_identifiers.h>
#include <common/json.h>
#include <common/json_util.h>
#include <cstring>
#include <map>
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
    CpuSetting(const char * id, const char * section, const char * key, bool * val, bool defaultValue);
    CpuSetting(const char * id, const char * section, const char * key, CpuBackend * val, CpuBackend defaultValue);
    CpuSetting(const char * id, const char * section, const char * key, CpuAccuracy * val, CpuAccuracy defaultValue);

    const char * identifier;
    const char * json_section;
    const char * json_key;
    SettingType settingType;
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
#ifdef HAS_NCE
    {NXCpuSetting::CpuBackend, "cpu", "cpu_backend", &cpuSettings.cpu_backend, CpuBackend::Nce},
#else
    {NXCpuSetting::CpuBackend, "cpu", "cpu_backend", &cpuSettings.cpu_backend, CpuBackend::Dynarmic},
#endif
    {NXCpuSetting::CpuAccuracy, "cpu", "cpu_accuracy", &cpuSettings.cpu_accuracy, CpuAccuracy::Auto},
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
};

CpuSetting::CpuSetting(const char * id, const char * section, const char * key, bool * val, bool defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::Boolean)
{
    defaults.boolValue = defaultValue;
    value.boolValue = val;
}

CpuSetting::CpuSetting(const char * id, const char * section, const char * key, CpuBackend * val, CpuBackend defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::CpuBackend)
{
    defaults.cpuBackend = defaultValue;
    value.cpuBackend = val;
}

CpuSetting::CpuSetting(const char * id, const char * section, const char * key, CpuAccuracy * val, CpuAccuracy defaultValue) :
    identifier(id),
    json_section(section),
    json_key(key),
    settingType(SettingType::CpuAccuracy)
{
    defaults.cpuAccuracy = defaultValue;
    value.cpuAccuracy = val;
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

void UpdateNceEnabled()
{
    const bool has_39bit = g_settings->GetBool(NXLoaderSetting::Has39BitAddressSpace);
    const bool is_nce = g_settings->GetInt(NXCpuSetting::CpuBackend) == static_cast<int32_t>(CpuBackend::Nce);
    if (is_nce && !IsFastmemEnabled())
    {
        LOG_WARNING(Common, "Fastmem is required to natively execute code in a performant manner, falling back to Dynarmic");
    }
    if (is_nce && !has_39bit)
    {
        LOG_WARNING(Common, "Program does not utilize 39-bit address space, unable to natively execute code");
    }
    g_settings->SetBool(NXCpuSetting::NceEnabled, IsFastmemEnabled() && is_nce && has_39bit);
}

bool AffectsNceEnabled(const char * setting)
{
    return strcmp(setting, NXCpuSetting::CpuBackend) == 0 ||
           strcmp(setting, NXCpuSetting::CpuDebugMode) == 0 ||
           strcmp(setting, NXCpuSetting::CpuoptFastmem) == 0 ||
           strcmp(setting, NXLoaderSetting::Has39BitAddressSpace) == 0;
}

void CpuSettingChanged(const char * setting, void * /*userData*/)
{
    for (const CpuSetting & cpuSetting : settings)
    {
        if (strcmp(cpuSetting.identifier, setting) != 0)
        {
            continue;
        }
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

void SetupCpuSetting(void)
{
    for (const CpuSetting & cpuSetting : settings)
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

    JsonValue root;
    JsonReader reader;
    const std::string json = g_settings->GetSectionSettings("nxemu-cpu");

    if (!json.empty() && reader.Parse(json.data(), json.data() + json.size(), root))
    {
        for (const CpuSetting & cpuSetting : settings)
        {
            JsonValue section = root[cpuSetting.json_section];
            if (!section.isObject())
            {
                continue;
            }
            JsonValue value = section[cpuSetting.json_key];
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
    }

    for (const CpuSetting & cpuSetting : settings)
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
        g_settings->RegisterCallback(cpuSetting.identifier, CpuSettingChanged, nullptr);
    }

    g_settings->SetDefaultBool(NXLoaderSetting::Has39BitAddressSpace, false);
    g_settings->SetBool(NXLoaderSetting::Has39BitAddressSpace, false);
    g_settings->RegisterCallback(NXLoaderSetting::Has39BitAddressSpace, NceInputChanged, nullptr);

    g_settings->SetDefaultBool(NXCpuSetting::NceEnabled, false);
    UpdateNceEnabled();
}

void SaveCpuSettings(void)
{
    typedef std::map<std::string, JsonValue> SectionMap;
    SectionMap sections;

    for (const CpuSetting & cpuSetting : settings)
    {
        switch (cpuSetting.settingType)
        {
        case SettingType::Boolean:
            if (*cpuSetting.value.boolValue != cpuSetting.defaults.boolValue)
            {
                sections[cpuSetting.json_section][cpuSetting.json_key] = *cpuSetting.value.boolValue;
            }
            break;
        case SettingType::CpuBackend:
            if (*cpuSetting.value.cpuBackend != cpuSetting.defaults.cpuBackend)
            {
                sections[cpuSetting.json_section][cpuSetting.json_key] =
                    CpuBackendToString(*cpuSetting.value.cpuBackend);
            }
            break;
        case SettingType::CpuAccuracy:
            if (*cpuSetting.value.cpuAccuracy != cpuSetting.defaults.cpuAccuracy)
            {
                sections[cpuSetting.json_section][cpuSetting.json_key] =
                    CpuAccuracyToString(*cpuSetting.value.cpuAccuracy);
            }
            break;
        default:
            UNIMPLEMENTED();
        }
    }

    JsonValue root;
    for (SectionMap::const_iterator it = sections.begin(); it != sections.end(); ++it)
    {
        if (it->second.size() > 0)
        {
            root[it->first] = it->second;
        }
    }
    g_settings->SetSectionSettings("nxemu-cpu", root.isNull() ? "" : JsonStyledWriter().write(root));
}
