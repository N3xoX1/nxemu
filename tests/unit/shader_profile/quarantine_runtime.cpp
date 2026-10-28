// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/renderer_vulkan/startup_pipeline_variants.h"
#include "yuzu_video_core/shader_profile_import_status.h"
#include "yuzu_video_core/shader_profile_location.h"
#include "yuzu_video_core/shader_profile_runtime.h"
#include "yuzu_video_core/shader_profile_sph.h"

#include <iostream>
#include <stdexcept>
#include <thread>
using namespace VideoCommon::ShaderProfile;
void Check(bool value) {
  if (!value)
    throw std::runtime_error("quarantine/runtime/import safety regression");
}
JsonValue Parse(std::string_view text) {
  JsonValue result;
  JsonReader reader;
  Check(reader.Parse(text.data(), text.data() + text.size(), result));
  return result;
}
int main(int argc, char **argv) try {
  Check(argc == 2);
  const std::filesystem::path path = argv[1];
  Check(ProfileFilename(0x100152000022000ULL)=="0100152000022000.nxshader");
  auto sidecar=path.parent_path()/ProfileFilename(0x100152000022000ULL);
  Check(FindGameProfile(path.parent_path()/"game.dnsp",0x100152000022000ULL).empty());
  {std::ofstream out(sidecar);out<<"metadata fixture";}
  Check(FindGameProfile(path.parent_path()/"game.dnsp",0x100152000022000ULL)==sidecar);
  Check(FindGameProfile(path.parent_path()/"game.dnsp",1).empty());
  Check(FindGameProfile({},0x100152000022000ULL).empty());
  const auto cache=path.parent_path()/"startup-cache"/"0100152000022000";
  std::filesystem::create_directories(cache);
  Check(FindStartupProfile(cache,path.parent_path()/"game.dnsp",0x100152000022000ULL)==sidecar);
  const auto shared=cache.parent_path()/sidecar.filename();
  {std::ofstream out(shared);out<<"shared fixture";}
  Check(FindStartupProfile(cache,path.parent_path()/"game.dnsp",0x100152000022000ULL)==shared);
  const auto local=cache/sidecar.filename();
  {std::ofstream out(local);out<<"local fixture";}
  Check(FindStartupProfile(cache,path.parent_path()/"game.dnsp",0x100152000022000ULL)==local);
  Check(FindStartupProfile(cache,{},0x100152000022000ULL)==local);
  Check(FindStartupProfile(cache,{},1).empty());
  Check(FindStartupProfile(cache,{},0).empty());
  const auto profiles=ProfileDirectory(path.parent_path()/"startup-data");
  Check(profiles.filename()=="nxshader");
  std::filesystem::create_directories(profiles);
  const auto durable=profiles/sidecar.filename();
  {std::ofstream out(durable);out<<"durable fixture";}
  Check(FindStartupProfile(cache,{},0x100152000022000ULL,profiles)==durable);
  std::filesystem::remove_all(cache.parent_path());
  Check(FindStartupProfile(cache,{},0x100152000022000ULL,profiles)==durable);
  std::filesystem::remove_all(profiles.parent_path());
  std::filesystem::create_directories(cache);
  {std::ofstream out(shared);out<<"shared fixture";}
  std::filesystem::remove(local);
  std::filesystem::create_directory(local); // A directory is never imported as a file.
  Check(FindStartupProfile(cache,{},0x100152000022000ULL)==shared);
  std::filesystem::remove_all(cache.parent_path());
  std::filesystem::remove(sidecar);
  Runtime::QuarantinedKeys<int> graphics, compute;
  Vulkan::StartupPipelineVariants<int, int> variants;
  const auto equal = [](int a, int b) { return a == b; };
  variants.Observe(7, 1, equal);
  variants.Observe(7, 2, equal);
  variants.Observe(9, 3, equal);
  Check(variants.ConsumeSafe([](int key, int) { Check(key == 9); },
                             [&](int key) { graphics.Reject(key); }) == 1);
  graphics.Requested(7);
  graphics.Requested(7);
  graphics.Requested(9);
  Check(graphics.total == 1 && graphics.requests == 2 &&
        graphics.requested.size() == 1);
  Runtime::Report report(path);
  Check(!report.Begin(7, false, true));
  report.active = true;
  auto event = report.Begin(7, false, true);
  {
    Runtime::Context context(event);
    Runtime::Span compiler(Runtime::current, Runtime::Work::Compiler);
    // A native build queued to another thread must retain its event explicitly.
    const auto captured = Runtime::current;
    std::thread native([captured] {
      Check(!Runtime::current);
      Runtime::Span timer(captured, Runtime::Work::Native);
      Runtime::NativeResult(captured, true);
    });
    native.join();
  }
  Check(!Runtime::current);
  report.Frame();
  report.Frame();
  Check(report.Flush(graphics, compute));
  std::ifstream in(path, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(in)), {});
  in.close();
  const auto json = Parse(bytes);
  Check(json["graphics"]["redemanded_unique"].asUInt64() == 1);
  const auto &compilation = json["compilations"][0U];
  Check(compilation["compiler"]["calls"].asUInt64() == 1 &&
        compilation["native"]["calls"].asUInt64() == 1);
  Check(compilation["native_successes"].asUInt64() == 1 &&
        compilation["native_failures"].asUInt64() == 0);
  Check(json["frames"].size() == 1);
  std::filesystem::remove(path);
  Shader::ProgramHeader header{};
  auto changed = header;
  changed.common0.shader_type.Assign(3);
  Check(CanonicalGuestHeader(header) != CanonicalGuestHeader(changed));
  changed = header;
  changed.raw[14] = 0x80000000;
  Check(CanonicalGuestHeader(header) != CanonicalGuestHeader(changed));
  Check(CanonicalGuestHeader(header) ==
        CanonicalGuestHeader(Shader::ProgramHeader{}));
  // Publication cannot certify partial or cancelled native work.
  Check(CanPublishImport(true, "stamp", false, 3, 3, 0));
  Check(!CanPublishImport(false, "stamp", false, 3, 3, 0));
  Check(!CanPublishImport(true, "", false, 3, 3, 0));
  Check(!CanPublishImport(true, "stamp", true, 3, 3, 0));
  Check(!CanPublishImport(true, "stamp", false, 2, 3, 0));
  Check(!CanPublishImport(true, "stamp", false, 3, 3, 1));
  const auto marker = Parse(
      R"({"schema":2,"stamp":"stamp","status":"ready-compatible-subset","native_completed":3,"native_queued":3,"native_failures":0})");
  Check(ReusableImportMarker(marker, "stamp"));
  Check(!ReusableImportMarker(marker, "other"));
  for (const char *field :
       {"native_queued", "native_completed", "native_failures"}) {
    auto partial = marker;
    partial.removeMember(field);
    Check(!ReusableImportMarker(partial, "stamp"));
    partial = marker;
    partial[field] = "3";
    Check(!ReusableImportMarker(partial, "stamp"));
    partial = marker;
    partial[field] = -1;
    Check(!ReusableImportMarker(partial, "stamp"));
  }
  auto failed = marker;
  failed["native_failures"] = 1U;
  Check(!ReusableImportMarker(failed, "stamp"));
  auto partial = marker;
  partial["native_completed"] = 2U;
  Check(!ReusableImportMarker(partial, "stamp"));
  std::cout << "PASS: quarantine membership, async timing, wire header and "
               "import completion safety\n";
} catch (const std::exception &e) {
  std::cerr << e.what() << '\n';
  return 1;
}
