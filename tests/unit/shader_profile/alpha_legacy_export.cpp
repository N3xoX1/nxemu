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
    // A draw that used a synthetic record exports it as is, synthetic and played; a
    // recorded record of the same key replaces it.
    for (const bool with_recorded : {false, true}) {
        regs->alpha_test_enabled = 0;
        const auto guest = GuestPipelineState::Capture(*regs, Maxwell::PrimitiveTopology::Triangles,
            Tegra::Engines::Maxwell3D::EngineHint::None);
        const auto state = guest.Project({});
        Model model;
        std::string key(416 + sizeof(state), '\0');
        Put(key, 40, 7, 8);
        std::memcpy(key.data() + 416, &state, sizeof(state));
        model.pipelines.push_back({key, {"generated"}, false, true});
        if (with_recorded) model.pipelines.push_back({key, {"recorded"}, false, false});
        for (const auto id : {"generated", "recorded"}) {
            model.profile["environments"][id]["shader"] = "shader";
            model.profile["environments"][id]["cbuf_values"] = Array();
        }
        model.profile["shaders"]["shader"]["references"] = Array();
        model.profile["shaders"]["shader"]["references"].Append(1U);
        JsonValue recording{JsonValueType::Object};
        recording["feature_bits"] = 0U;
        recording["guest_state_word_count"] = 278U;
        recording["records_compute"] = true;
        recording["recipes"] = Array();
        JsonValue observation{JsonValueType::Object};
        observation["words"] = Array();
        for (auto word : guest.Words()) observation["words"].Append(word);
        for (size_t i = 0; i < 104; ++i) observation["words"].Append(i == 10 ? 7U : 0U);
        recording["recipes"].Append(std::move(observation));
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
        Check(report.graphics == 1 && exported["recipes"].size() == 1);
        const auto& r = exported["recipes"][0];
        Check(r["environments"][0].asString() == (with_recorded ? "recorded" : "generated"));
        Check(r["synthetic"].asBool() == !with_recorded && r["played"].asBool() == !with_recorded);
    }
    {
        // Export reads the renderer's own store, at its shader ABI, and knows its synthetic records.
        const auto directory = recording_path.parent_path() / "export-store";
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        const auto store = directory / "vulkan.nxc";
        std::string env;
        const auto put = [&](u64 value, size_t bytes) { for (size_t i = 0; i < bytes; ++i) env += static_cast<char>(value >> (8 * i)); };
        constexpr u64 code_size = 88; // The 80-byte program header and one instruction.
        put(code_size, 8);
        for (int map = 0; map < 4; ++map) put(0, 8);
        for (const u64 field : {u64{0}, u64{2}, u64{0x30}, u64{0x30}, u64{0x30 + code_size - 8}, u64{1}, u64{4}}) put(field, 4);
        const std::string code(code_size, '\x01');
        env += code;
        env += code.substr(0, 80);
        VideoCommon::CacheStore::Record record;
        record.key = std::string(1400, '\0');
        Put(record.key, 40, 7, 8);
        record.environments.push_back(env);
        // A version's records: a recorded one, and one of a synthetic recipe through the generated index only.
        const std::string context(64, 'a');
        std::string recorded_id, generated_id;
        VideoCommon::CacheStore::Append(store, VideoCommon::CacheStore::VulkanShaderAbi, record, &recorded_id, context);
        auto other = env;
        other[8 + 32] = 1; // Another local memory size: another record.
        record.environments = {other};
        VideoCommon::CacheStore::Append(store, VideoCommon::CacheStore::VulkanShaderAbi, record, &generated_id, context, true);
        Check(recorded_id != generated_id);
        Check(VideoCommon::CacheStore::ReadContext(store, context) == std::set<std::string>{recorded_id});
        Check(VideoCommon::CacheStore::ReadContext(store, context, true) == std::set<std::string>{generated_id});
        ExportOptions options;
        options.cache = store;
        options.context = context;
        const auto synthetic_count = [&] {
            const auto model = ReadCache(options);
            size_t synthetic{};
            for (const auto& p : model.pipelines) synthetic += p.synthetic;
            return std::pair{model.pipelines.size(), synthetic};
        };
        Check(synthetic_count() == std::pair<size_t, size_t>{2, 1});
        // Without its generated index a synthetic record is no record of the version: never exported, nor trusted,
        // as a recorded one.
        const auto generated_index = VideoCommon::CacheStore::ContextIndexPath(store, context).parent_path() / "vulkan.generated.idx";
        Check(std::filesystem::exists(generated_index));
        std::filesystem::remove(generated_index);
        Check(VideoCommon::CacheStore::ReadContext(store, context, true).empty());
        Check(synthetic_count() == std::pair<size_t, size_t>{1, 0});
        // Without a version no record can be told recorded: every one exports as synthetic.
        options.context.clear();
        Check(synthetic_count() == std::pair<size_t, size_t>{2, 2});
        std::filesystem::remove_all(directory);
    }
    std::filesystem::remove(recording_path);
}
