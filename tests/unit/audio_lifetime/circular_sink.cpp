// CleanUp of a circular buffer sink must not read its state as a device sink's upsampler.
#include <cstdlib>
#include <cstring>
#include "yuzu_audio_core/renderer/sink/circular_buffer_sink_info.cpp"
#include "yuzu_audio_core/renderer/sink/sink_info_base.cpp"

namespace AudioCore::Renderer {
bool PoolMapper::TryAttachBuffer(BehaviorInfo::ErrorInfo&, AddressInfo&, CpuAddr, u64) const {
    return true;
}
} // namespace AudioCore::Renderer

int main() {
    using namespace AudioCore::Renderer;
    CircularBufferSinkInfo sink;
    // After playback the positions overlap the upsampler pointer of a DeviceState.
    auto* state = reinterpret_cast<SinkInfoBase::CircularBufferState*>(sink.GetState());
    state->last_pos2 = 0x1000;
    state->current_pos = 0x2000;
    sink.CleanUp();
    if (sink.GetType() != SinkInfoBase::Type::Invalid) {
        std::abort();
    }
    return 0;
}
