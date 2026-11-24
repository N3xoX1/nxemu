// SPDX-License-Identifier: GPL-2.0-or-later
// Offline NXShader tool.
//   nxshader_tool export <title cache> <game folder> <base|-> <update|-> <version folder> <output> [previous]
//   nxshader_tool basepack <game folder> <base> <update|-> <title hex> <session file> <output>
//   nxshader_tool clean <input> <output> <title hex>
//   nxshader_tool harvest <game folder> <base> <update|-> <title hex>
//   nxshader_tool inspect <file> <title hex>
//   nxshader_tool import <file> <game folder> <title hex> [context]
//   nxshader_tool json <file> <title hex> <output.json>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include "common/json.h"
#include "video_settings.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/shader_profile.h"
#include "yuzu_video_core/shader_profile_codec.h"
#include "yuzu_video_core/shader_profile_compact.h"
#include "yuzu_video_core/shader_profile_context.h"
#include "yuzu_video_core/shader_profile_discovery.h"
#include "yuzu_video_core/shader_profile_export.h"

VideoSettings videoSettings{};
namespace Common::Log {
LogLevel g_classLevel[static_cast<uint8_t>(LogClass::Count)]{};
void FmtLogMessageImpl(LogClass, LogLevel, const char*, unsigned int, const char*, const char*, const fmt::format_args&) {}
}
void assert_fail_impl() { throw std::runtime_error("renderer assertion"); }
[[noreturn]] void unreachable_impl() { throw std::runtime_error("renderer unreachable"); }

using namespace VideoCommon;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static std::string ReadAll(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + path.string());
    return std::string((std::istreambuf_iterator<char>(file)), {});
}
static void WriteAll(const fs::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("cannot write " + path.string());
}
static double Since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}
static void Print(const ShaderProfile::ProfileInspection& r) {
    std::cout << "graphics " << r.graphics << ", compute " << r.compute << ", programs " << r.programs
              << ", environments " << r.environments << ", versions " << r.versions << ", unresolved " << r.unresolved
              << ", bytes " << r.bytes << '\n';
    // Drawn in play only when every predicted recipe was seen drawn: nothing in the file is a guess.
    if (r.synthetic == r.played)
        std::cout << "origin: drawn in play only"
                  << (r.synthetic ? " (" + std::to_string(r.played) + " predicted, all seen drawn)" : std::string()) << '\n';
    else
        std::cout << "origin: holds " << r.synthetic - r.played << " predicted recipes never seen drawn (" << r.played
                  << " more seen drawn)\n";
}

int BasePack(int argc, char** argv);

int main(int argc, char** argv) try {
    if (argc < 2) return 2;
    const std::string command = argv[1];
    if (command == "basepack") return BasePack(argc, argv);
    // clean <in> <out> <title>: what an export from play keeps of a file (no unconfirmed guesses).
    if (command == "clean" && argc == 5) {
        const auto title = std::stoull(argv[4], nullptr, 16);
        auto profile = ShaderProfile::DecodeCompact(ShaderProfile::Unpack(ReadAll(fs::u8path(argv[2]))), title);
        const auto dropped = ShaderProfile::DropUnconfirmed(profile);
        ShaderProfile::TrimToLimits(profile, profile["source_context"].asString(), ShaderProfile::ProfileLimits{});
        const auto bytes = ShaderProfile::PackCompact(profile, 19);
        ShaderProfile::Publish(fs::u8path(argv[3]), bytes);
        std::cout << "left out " << dropped << " unconfirmed predicted recipes\n";
        Print(ShaderProfile::InspectCompact(ShaderProfile::Unpack(bytes), title));
        return 0;
    }
    if (command == "export" && (argc == 8 || argc == 9)) {
        const fs::path cache = fs::u8path(argv[2]);
        ShaderProfile::ExportOptions options;
        options.cache = cache / "vulkan.nxc";
        options.recording = ShaderProfile::VersionsDirectory(cache) / fs::u8path(argv[6]) / "recipes.nxr";
        options.context = ShaderProfile::RecordingContext(options.recording);
        options.game_root = fs::u8path(argv[3]);
        // "-": found from the recording.
        if (std::string_view(argv[4]) != "-") options.base = argv[4];
        if (std::string_view(argv[5]) != "-") options.update = argv[5];
        options.automatic_sources = true;
        options.output = fs::u8path(argv[7]);
        options.index = options.output;
        options.index += ".index.json";
        options.compact = true;
        options.allow_partial = true;
        options.compression_level = 19;
        if (argc == 9) options.previous = fs::u8path(argv[8]);
        // As an export from play in the emulator: predicted recipes never drawn are left out.
        options.drop_unconfirmed = true;
        // The title folder is named after the title.
        options.title_id = std::stoull(cache.filename().string(), nullptr, 16);
        const auto start = Clock::now();
        const auto r = ShaderProfile::Export(options);
        std::cout << "export: " << r.programs << " programs, " << r.graphics << " graphics, " << r.compute
                  << " compute, inherited " << r.inherited << ", missing " << r.missing << ", left out unconfirmed "
                  << r.dropped_unconfirmed << ", left out unrepresentable " << r.dropped_unrepresentable << ", "
                  << fs::file_size(options.output)
                  << " bytes, " << Since(start) << " s\n";
        return 0;
    }
    if (command == "harvest" && argc == 6) {
        // harvest <game folder> <base> <update|-> <title>: the programs the game files hold, found
        // without a recording.
        ShaderProfile::ExportOptions options;
        options.game_root = fs::u8path(argv[2]);
        options.base = argv[3];
        if (std::string_view(argv[4]) != "-") options.update = argv[4];
        options.title_id = std::stoull(argv[5], nullptr, 16);
        const auto start = Clock::now();
        const auto r = ShaderProfile::HarvestPrograms(options);
        std::map<std::pair<int, int>, size_t> kinds;
        for (const auto& [key, program] : r.programs) ++kinds[{program.platform, program.stage}];
        std::cout << "harvest: " << r.files << " files, " << (r.read_bytes >> 20) << " MiB read, " << (r.decoded_bytes >> 20)
                  << " MiB decoded, " << r.containers << " BNSH, " << r.occurrences << " programs found (" << r.by_content
                  << " by content), " << r.programs.size() << " distinct, " << r.groups.size() << " groups, " << r.errors.size()
                  << " errors, " << Since(start) << " s\n";
        static constexpr const char* StageNames[]{"vertex A", "vertex", "tess control", "tess eval", "geometry",
                                                  "fragment", "compute", "task", "mesh"};
        for (const auto& [kind, n] : kinds)
            std::cout << "  switch " << kind.first << ' ' << StageNames[kind.second] << ": " << n << '\n';
        std::map<std::string, size_t> errors;
        for (const auto& error : r.errors) ++errors[error.substr(error.find(": ") + 2)];
        for (const auto& [error, n] : errors) std::cout << "  error x" << n << ": " << error << '\n';
        return 0;
    }
    if (command == "inspect" && argc == 4) {
        Print(ShaderProfile::Inspect(fs::u8path(argv[2]), std::stoull(argv[3], nullptr, 16)));
        return 0;
    }
    if (command == "json" && argc == 5) {
        const auto profile =
            ShaderProfile::DecodeCompact(ShaderProfile::Unpack(ReadAll(fs::u8path(argv[2]))), std::stoull(argv[3], nullptr, 16));
        WriteAll(fs::u8path(argv[4]), ShaderProfile::Canonical(profile));
        return 0;
    }
    if (command == "import" && (argc == 5 || argc == 6)) {
        const auto title = std::stoull(argv[4], nullptr, 16);
        const auto payload = ShaderProfile::Unpack(ReadAll(fs::u8path(argv[2])));
        const std::string context = argc == 6 ? argv[5] : "";
        ShaderProfile::ImportReport report;
        const auto start = Clock::now();
        const auto recipes = ShaderProfile::ReadCompact(payload, fs::u8path(argv[3]), title, nullptr, nullptr, {}, &report,
                                                        context, nullptr, nullptr, true);
        size_t graphics{}, compute{};
        for (const auto& recipe : recipes) (recipe.compute ? compute : graphics)++;
        std::cout << "import: " << recipes.size() << " recipes (" << graphics << " graphics, " << compute
                  << " compute), skipped " << report.skipped << ", missing resources " << report.missing_resources
                  << ", incompatible " << report.incompatible_resources << ", " << Since(start) << " s\n";
        return 0;
    }
    std::cerr << "usage: see nxshader_tool.cpp\n";
    return 2;
} catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
