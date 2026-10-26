// SPDX-License-Identifier: GPL-2.0-or-later
// Frozen pre-optimization writer for wire-equivalence regression/benchmark.
std::vector<u32> LegacyWords(const GuestPipelineState& guest) {
    std::vector<u32> words;
    words.reserve(300);
    const auto& s = guest.complete;
    words.insert(words.end(), {1U, s.raw1, s.raw2, guest.enabled_vertex_streams});
    for (auto x : s.color_formats) words.push_back(x);
    words.push_back(s.alpha_test_ref);
    words.push_back(s.point_size);
    for (auto x : s.viewport_swizzles) words.push_back(x);
    words.push_back(static_cast<u32>(s.enabled_divisors));
    words.push_back(static_cast<u32>(s.enabled_divisors >> 32));
    words.push_back(s.dynamic_state.raw1);
    words.push_back(s.dynamic_state.raw2);
    for (auto x : s.attachments) words.push_back(x.raw);
    for (auto x : s.attributes) words.push_back(x.raw);
    for (auto x : s.binding_divisors) words.push_back(x);
    for (auto x : s.vertex_strides) words.push_back(x);
    for (auto x : s.xfb_state.layouts) {
        words.insert(words.end(), {x.stream, x.varying_count, x.stride});
    }
    for (const auto& stream : s.xfb_state.varyings) {
        for (const auto& x : stream) {
            words.push_back(x.attribute0.Value() | (x.attribute1.Value() << 8) |
                            (x.attribute2.Value() << 16) | (x.attribute3.Value() << 24));
        }
    }
    return words;
}
