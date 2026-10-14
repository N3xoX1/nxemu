void Harness::Probe()
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
            ProcessCandidate(method, value);
        }
        return;
    }
    if (control == Regs::ShadowRamControl::Replay)
    {
        for (auto [method, value] : method_sink)
        {
            ProcessCandidate(method, shadow_state.reg_array[method]);
        }
        return;
    }
    for (auto [method, value] : method_sink)
    {
        ProcessCandidate(method, value);
    }
}
void Harness::ProcessCandidate(u32 method, u32 argument)
{
    if (regs.reg_array[method] == argument)
    {
        return;
    }
    regs.reg_array[method] = argument;

    static_assert(std::tuple_size_v<DirtyState::Tables> == 2);
    const auto first = dirty.tables[0][method];
    const auto second = dirty.tables[1][method];
    if (first < 64 && second < 64) {
        dirty.flags |= DirtyState::Flags{(u64{1} << first) | (u64{1} << second)};
    } else {
        dirty.flags[first] = true;
        if (first != second) {
            dirty.flags[second] = true;
        }
    }
}