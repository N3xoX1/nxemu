// Exercise the production Vulkan depth bias update across point/line/polygon draws.
#include <array>
#include <cstdint>
#include <iostream>
#include <vector>
using u32 = std::uint32_t;
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
struct StateTracker {
    bool dirty{true};
    Maxwell::PrimitiveTopology current_topology = static_cast<Maxwell::PrimitiveTopology>(~0u);
    bool TouchDepthBiasEnable() {const bool result=dirty;dirty=false;return result;}
    bool ChangePrimitiveTopology(Maxwell::PrimitiveTopology new_topology);
};
#include "depth-bias-tracker.inc"
namespace vk {
struct CommandBuffer {
    std::vector<bool>& changes;
    void SetDepthBiasEnableEXT(bool enabled) {changes.push_back(enabled);}
};
}
struct Scheduler {
    std::vector<bool> changes;
    template<class F> void Record(F f) {f(vk::CommandBuffer{changes});}
};
struct DrawManager {
    struct State {Maxwell::PrimitiveTopology topology{Maxwell::PrimitiveTopology::Points};} state;
    const State& GetDrawState() const {return state;}
};
struct Engine {DrawManager* draw_manager;};
struct Rasterizer {
    StateTracker state_tracker;
    Scheduler scheduler;
    DrawManager draw_manager;
    Engine engine{&draw_manager};
    Engine* maxwell3d{&engine};
    void UpdateDepthBiasEnable(Tegra::Engines::Maxwell3D::Regs& regs);
};
#if FIXED_MODE
#include "depth-bias-fixed.inc"
#else
#include "depth-bias-baseline.inc"
#endif
int tests{},failures{};
void Check(const char* label,bool ok) {
    ++tests;failures+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<label<<'\n';
}
int main() {
    using Topology=Maxwell::PrimitiveTopology;
    Rasterizer renderer;
    Tegra::Engines::Maxwell3D::Regs regs;
    auto draw=[&](Topology topology) {renderer.draw_manager.state.topology=topology;renderer.UpdateDepthBiasEnable(regs);};
    draw(Topology::Points);
    Check("point_clear_does_not_enable_polygon_bias",!renderer.scheduler.changes.back());
    draw(Topology::Triangles);
    Check("triangles_enable_bias_after_point_clear",renderer.scheduler.changes.back());
    const auto updates=renderer.scheduler.changes.size();
    for(int i=0;i<10000;++i) draw(Topology::Triangles);
    Check("unchanged_topology_does_not_emit_redundant_commands",renderer.scheduler.changes.size()==updates);
    draw(Topology::Lines);
    Check("lines_restore_their_own_bias_setting",!renderer.scheduler.changes.back());
    regs.polygon_offset_line_enable=1;renderer.state_tracker.dirty=true;draw(Topology::Lines);
    Check("register_changes_apply_without_topology_change",renderer.scheduler.changes.back());
    draw(Topology::Points);
    Check("points_restore_disabled_bias_after_lines",!renderer.scheduler.changes.back());
    draw(Topology::TriangleStrip);
    Check("triangle_strips_restore_enabled_polygon_bias",renderer.scheduler.changes.back());
    regs.polygon_offset_fill_enable=0;renderer.state_tracker.dirty=true;draw(Topology::TriangleStrip);
    Check("changed_channel_or_register_state_is_not_skipped",!renderer.scheduler.changes.back());
    std::cout<<"tests="<<tests<<" failures="<<failures<<'\n';return failures?1:0;
}
