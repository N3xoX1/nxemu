// SPDX-License-Identifier: GPL-2.0-or-later
#include "recorder_fixture.h"
// Include the production exporter to exercise its private assembly path with
// synthetic records, without requiring game files or a running renderer.
#include "yuzu_video_core/shader_profile_export.cpp"

int main(int argc, char** argv) {
    Check(argc == 2);
    using namespace VideoCommon::ShaderProfile;
    const auto recording_path = std::filesystem::path{argv[1]};
    auto regs = RecorderRegisters();
    for (const auto op : {Maxwell::ComparisonOp::Always_GL, Maxwell::ComparisonOp::Never_GL,
                          Maxwell::ComparisonOp::Less_GL}) {
        regs->alpha_test_enabled = 1;
        regs->alpha_test_func = op;
        regs->alpha_test_ref = 0.5f;
        const auto guest = GuestPipelineState::Capture(*regs, Maxwell::PrimitiveTopology::Triangles,
            Tegra::Engines::Maxwell3D::EngineHint::None);
        auto old_state = guest.Project({});
        old_state.alpha_test_ref = 0x3f000000U; // A key from before inactive-alpha normalization.
        Model model;
        std::string key(416 + sizeof(old_state), '\0');
        Put(key, 40, 7, 8); // Fragment shader only, so no vertex input dependency.
        std::memcpy(key.data() + 416, &old_state, sizeof(old_state));
        model.pipelines.push_back({key, {"environment"}, false});
        model.profile["environments"]["environment"]["shader"] = "shader";
        model.profile["environments"]["environment"]["cbuf_values"] = Array();
        model.profile["shaders"]["shader"]["references"] = Array();
        model.profile["shaders"]["shader"]["references"].Append(1U);
        JsonValue recording{JsonValueType::Object};
        recording["feature_bits"] = 0U;
        recording["guest_state_word_count"] = 278U;
        recording["records_compute"] = true;
        recording["recipes"] = Array();
        for (const auto reference : {0x3f000000U, 0x3f400000U}) {
            auto words = guest.Words();
            words[12] = reference; // Old journals retained the unused register.
            JsonValue observation{JsonValueType::Object};
            observation["words"] = Array();
            for (auto word : words) observation["words"].Append(word);
            for (size_t i = 0; i < 104; ++i)
                observation["words"].Append(i == 10 ? 7U : 0U);
            recording["recipes"].Append(std::move(observation));
        }
        {
            std::ofstream stream{recording_path, std::ios::binary};
            stream << Canonical(recording);
            Check(stream.good());
        }
        ExportOptions options;
        options.recording = recording_path;
        options.cache = recording_path.parent_path() / "synthetic.nxc";
        options.context = "synthetic";
        options.title_id = 1;
        options.allow_partial = true;
        ExportReport report;
        const auto exported = Assemble(model, options, report);
        const bool active = op == Maxwell::ComparisonOp::Less_GL;
        Check(report.graphics == 1 && report.unresolved == (active ? 1U : 0U));
        Check(exported["recipes"].size() == 1);
        Check(exported["recipes"][0]["guest_words"][12].asUInt64() ==
              (active ? 0x3f000000U : 0U));
    }
    std::filesystem::remove(recording_path);
}
