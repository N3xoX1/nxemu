#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <vector>
#include "yuzu_audio_core/renderer/command/effect/biquad_filter.cpp"

int main() {
    using namespace AudioCore::Renderer;
    const auto check = [](bool condition) { if (!condition) std::abort(); };
    for (float value : {std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity()}) {
        for (bool inplace : {false, true}) {
            for (bool feedback : {false, true}) {
                std::array<s32, 4> input{10, -20, 30, -40}, output{};
                VoiceState::BiquadFilterState state{1, 2, 3, 4};
                std::array<f32, 3> b{1, 0, 0};
                std::array<f32, 2> a{};
                if (feedback) a[1] = value; else b[0] = value;
                auto out = inplace ? std::span<s32>{input} : std::span<s32>{output};
                ApplyBiquadFilterFloat2(out, input, b, a, state, 4);
                check(std::equal(out.begin(), out.end(), input.begin()));
                check(state.s0 == 0 && state.s1 == 0 && state.s2 == 0 && state.s3 == 0);
            }
        }
    }
    for (bool native : {false, true}) {
        std::array<s32, 4> input{10, -20, 30, -40}, output{};
        VoiceState::BiquadFilterState state{};
        state.s0 = std::bit_cast<s64>(std::numeric_limits<double>::quiet_NaN());
        std::array<f32, 3> b{1, 0, 0}; std::array<f32, 2> a{};
        std::array<s16, 3> bi{16384, 0, 0}; std::array<s16, 2> ai{};
        if (native) ApplyBiquadFilterFloat2(output, input, b, a, state, 4);
        else ApplyBiquadFilterFloat(output, input, bi, ai, state, 4);
        check(output == input);
    }
    std::vector<s32> input(400, 100), output(400);
    std::array<f32, 3> b{std::numeric_limits<float>::max(), 0, 0};
    std::array<f32, 2> a{std::numeric_limits<float>::max(), 0};
    VoiceState::BiquadFilterState state{};
    ApplyBiquadFilterFloat2(output, input, b, a, state, 400);
    check(std::isfinite(std::bit_cast<double>(state.s2)));
}
