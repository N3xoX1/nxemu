void Harness::Baseline()
{
    SCOPE_EXIT
    {
        method_sink.clear();
    };
    const auto control = shadow_state.shadow_ram_control;
    if (control == Regs::ShadowRamControl::Track ||
        control == Regs::ShadowRamControl::TrackWithFilter)
    {

        for (auto [method, value] : method_sink)
        {
            shadow_state.reg_array[method] = value;
            ProcessDirtyRegisters(method, value);
        }
        return;
    }
    if (control == Regs::ShadowRamControl::Replay)
    {
        for (auto [method, value] : method_sink)
        {
            ProcessDirtyRegisters(method, shadow_state.reg_array[method]);
        }
        return;
    }
    for (auto [method, value] : method_sink)
    {
        ProcessDirtyRegisters(method, value);
    }
}
void Harness::ProcessDirtyRegisters(u32 method, u32 argument)
{
    if (regs.reg_array[method] == argument)
    {
        return;
    }
    regs.reg_array[method] = argument;

    for (const auto & table : dirty.tables)
    {
        dirty.flags[table[method]] = true;
    }
}