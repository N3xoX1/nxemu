// Exercise the production caller as well as the enable update. In particular,
// topology changes must work with the separate StateEnable dirty group clear.
#include <array>
#include <cstdint>
#include <iostream>
#include <vector>
using u32=std::uint32_t;
namespace Maxwell {
enum class PrimitiveTopology : u32 {Points, Lines, LineLoop, LineStrip, Triangles,
    TriangleStrip, TriangleFan, Quads, QuadStrip, Polygon, LinesAdjacency,
    LineStripAdjacency, TrianglesAdjacency, TriangleStripAdjacency, Patches};
}
namespace Tegra::Engines {
struct Maxwell3D {
    struct Regs {u32 polygon_offset_point_enable{},polygon_offset_line_enable{},polygon_offset_fill_enable{1};};
};
}
using Regs=Tegra::Engines::Maxwell3D::Regs;
struct StateTracker {
    bool group_dirty{true},bias_dirty{true};
    Maxwell::PrimitiveTopology current_topology=static_cast<Maxwell::PrimitiveTopology>(~0u);
    bool TouchStateEnable(){const bool result=group_dirty;group_dirty=false;return result;}
    bool TouchDepthBiasEnable(){const bool result=bias_dirty;bias_dirty=false;return result;}
    bool ChangePrimitiveTopology(Maxwell::PrimitiveTopology new_topology);
};
#include "dynamic-state-tracker.inc"
namespace vk {
struct CommandBuffer {
    std::vector<bool>& changes;
    void SetDepthBiasEnableEXT(bool enabled){changes.push_back(enabled);}
};
}
struct Scheduler {
    std::vector<bool> changes;
    template<class F>void Record(F f){f(vk::CommandBuffer{changes});}
};
struct DrawManager {
    struct State {Maxwell::PrimitiveTopology topology{Maxwell::PrimitiveTopology::Points};} state;
    const State& GetDrawState()const{return state;}
};
struct Engine {Regs regs;DrawManager* draw_manager;};
struct Device {
    bool extended{true},extended2{true};
    bool IsExtExtendedDynamicStateSupported()const{return extended;}
    bool IsExtExtendedDynamicState2Supported()const{return extended2;}
    bool IsExtExtendedDynamicState3EnablesSupported()const{return false;}
    bool IsExtExtendedDynamicState2ExtrasSupported()const{return false;}
    bool IsExtExtendedDynamicState3Supported()const{return false;}
    bool IsExtVertexInputDynamicStateSupported()const{return false;}
};
struct Rasterizer {
    StateTracker state_tracker;Scheduler scheduler;Device device;DrawManager draws;
    Engine engine{{},&draws};Engine* maxwell3d{&engine};
    void UpdateDynamicStates();
    void UpdateDepthBiasEnable(Regs& regs);
#define NOOP(name) void name(Regs&){}
    NOOP(UpdateViewportsState) NOOP(UpdateScissorsState) NOOP(UpdateDepthBias)
    NOOP(UpdateBlendConstants) NOOP(UpdateDepthBounds) NOOP(UpdateStencilFaces)
    NOOP(UpdateLineWidth) NOOP(UpdateCullMode) NOOP(UpdateDepthCompareOp)
    NOOP(UpdateFrontFace) NOOP(UpdateStencilOp) NOOP(UpdateDepthBoundsTestEnable)
    NOOP(UpdateDepthTestEnable) NOOP(UpdateDepthWriteEnable) NOOP(UpdateStencilTestEnable)
    NOOP(UpdatePrimitiveRestartEnable) NOOP(UpdateRasterizerDiscardEnable)
    NOOP(UpdateLogicOpEnable) NOOP(UpdateDepthClampEnable) NOOP(UpdateLogicOp)
    NOOP(UpdateBlending) NOOP(UpdateVertexInput)
#undef NOOP
    void Draw(Maxwell::PrimitiveTopology topology){draws.state.topology=topology;UpdateDynamicStates();}
};
#if FIXED_MODE
#include "dynamic-state-fixed.inc"
#else
#include "dynamic-state-baseline.inc"
#endif
int tests{},failures{};
void Check(const char* label,bool ok){++tests;failures+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<label<<'\n';}
int main(){
    using T=Maxwell::PrimitiveTopology;
    Rasterizer r;r.Draw(T::Points);
    Check("initial_point_clear_disables_bias",r.scheduler.changes.size()==1&&!r.scheduler.changes.back());
    Check("initial_draw_clears_state_enable_group",!r.state_tracker.group_dirty);
    r.Draw(T::Triangles);
    Check("clean_state_group_allows_triangle_bias_update",r.scheduler.changes.back());
    auto commands=r.scheduler.changes.size();
    for(int i=0;i<10000;++i)r.Draw(T::Triangles);
    Check("unchanged_state_emits_no_extra_commands",r.scheduler.changes.size()==commands);
    r.Draw(T::Points);Check("clean_state_group_restores_point_setting",!r.scheduler.changes.back());
    r.Draw(T::TriangleStrip);Check("clean_state_group_updates_triangle_strips",r.scheduler.changes.back());
    r.Draw(T::Lines);Check("clean_state_group_restores_line_setting",!r.scheduler.changes.back());
    r.engine.regs.polygon_offset_line_enable=1;r.state_tracker.group_dirty=true;r.state_tracker.bias_dirty=true;r.Draw(T::Lines);
    Check("dirty_register_applies_without_topology_change",r.scheduler.changes.back());
    r.Draw(T::Points);Check("topology_updates_after_register_dirty_group_consumed",!r.scheduler.changes.back());
    r.Draw(T::Triangles);Check("polygon_enable_survives_clean_dirty_group",r.scheduler.changes.back());
    r.engine.regs.polygon_offset_fill_enable=0;r.state_tracker.group_dirty=true;r.state_tracker.bias_dirty=true;r.Draw(T::Triangles);
    Check("dirty_register_disables_polygon_bias",!r.scheduler.changes.back());
    Rasterizer legacy;legacy.device.extended2=false;legacy.Draw(T::Points);legacy.Draw(T::Triangles);
    Check("unsupported_dynamic_state2_does_not_emit_bias_command",legacy.scheduler.changes.empty());
    Rasterizer old;old.device.extended=false;old.Draw(T::Points);old.Draw(T::Triangles);
    Check("unsupported_dynamic_state_does_not_emit_bias_command",old.scheduler.changes.empty());
    std::cout<<"tests="<<tests<<" failures="<<failures<<'\n';return failures?1:0;
}
