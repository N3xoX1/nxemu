#pragma once

#include <nxemu-module-spec/cpu.h>

struct CpuSettings
{
    CpuBackend cpu_backend;
    CpuAccuracy cpu_accuracy;
    bool cpu_debug_mode;

    bool cpuopt_page_tables;
    bool cpuopt_block_linking;
    bool cpuopt_return_stack_buffer;
    bool cpuopt_fast_dispatcher;
    bool cpuopt_context_elimination;
    bool cpuopt_const_prop;
    bool cpuopt_misc_ir;
    bool cpuopt_reduce_misalign_checks;
    bool cpuopt_fastmem;
    bool cpuopt_fastmem_exclusives;
    bool cpuopt_recompile_exclusives;
    bool cpuopt_ignore_memory_aborts;

    bool cpuopt_unsafe_unfuse_fma;
    bool cpuopt_unsafe_reduce_fp_error;
    bool cpuopt_unsafe_ignore_standard_fpcr;
    bool cpuopt_unsafe_inaccurate_nan;
    bool cpuopt_unsafe_fastmem_check;
    bool cpuopt_unsafe_ignore_global_monitor;
};

extern CpuSettings cpuSettings;

bool IsFastmemEnabled(void);

void SetupCpuSetting(void);
void CleanupCpuSetting(void);
void SaveCpuSettings(void);
