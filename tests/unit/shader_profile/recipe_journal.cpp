// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile_journal.h"
#include <iostream>

using namespace VideoCommon::ShaderProfile::Journal;
void Check(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) try {
  Check(argc == 2, "Expected temporary journal path");
  const auto path = std::filesystem::u8path(argv[1]);
  std::filesystem::remove(path);
  Entry a{};
  a[0] = 63;
  a[1] = 1;
  Append(path, a);
  auto b = a;
  b[0] = 0; // A different source GPU must retain its projection inputs.
  Append(path, b);
  auto c = a;
  c[12] = 123; // Guest combination, even if the host key is shared.
  Append(path, c);
  const auto three = std::filesystem::file_size(path);
  auto saved = Read(path);
  Check(saved.entries.size() == 3 && !saved.incomplete_tail,
        "Recipe combination lost");
  Check(saved.entries[0] == a && saved.entries[1] == b && saved.entries[2] == c,
        "Journal lost first-use order");
  // An earlier recipe observed again keeps its original position.
  Append(path, a);
  Append(path, c);
  saved = Read(path);
  Check(saved.entries.size() == 3 && saved.entries[0] == a &&
        saved.entries[1] == b && saved.entries[2] == c,
        "Journal duplicate changed first-use order");
  std::filesystem::resize_file(path, three);
  // Different entries with the SAME hash must retain exact identity and order.
  std::vector<Entry> collisions;
  Detail::FirstUseIndex collision_index(collisions);
  Check(collision_index.Insert(a,0) && collision_index.Insert(b,0) &&
        collision_index.Insert(c,0),"Hash collision dropped a recipe");
  Check(!collision_index.Insert(a,0) && !collision_index.Insert(c,0) &&
        collisions==std::vector<Entry>({a,b,c}),"Collision dedup changed order");
  auto batch_path=path;batch_path+=".batch";
  const std::array entries{a,b,c};WriteMetrics metrics;
  AppendBatch(batch_path,entries,&metrics);
  // One batch is one frame, smaller than three separate appends.
  Check(Read(batch_path).entries==Read(path).entries && metrics.frames==3 && metrics.batches==1 &&
        metrics.bytes==std::filesystem::file_size(batch_path) && metrics.bytes<three,
        "Batch changed entries or counters");
  std::filesystem::remove(batch_path);
  AppendBatch(batch_path,{},nullptr);
  Check(!std::filesystem::exists(batch_path),"Empty batch created a journal");
  {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    out << "torn frame";
  }
  saved = Read(path);
  Check(saved.entries.size() == 3 && saved.incomplete_tail &&
        saved.entries[0] == a && saved.entries[1] == b && saved.entries[2] == c,
        "Crash lost committed entries or first-use order");
  std::filesystem::resize_file(path, saved.valid_bytes);
  Check(!Read(path).incomplete_tail, "Tail recovery failed");
  {
    std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
    io.seekp(Magic.size() + 10);
    io.put('x');
  }
  // A corrupt frame ends the committed prefix instead of disabling recording.
  saved = Read(path);
  Check(saved.entries.empty() && saved.incomplete_tail &&
        saved.valid_bytes == Magic.size(), "Corrupted recipe was accepted");
  std::filesystem::resize_file(path, 3);
  saved = Read(path);
  Check(saved.entries.empty() && saved.incomplete_tail && saved.valid_bytes == 0,
        "Torn journal header was accepted");
  std::filesystem::resize_file(path, 0);
  Append(path, a);
  Check(Read(path).entries == std::vector<Entry>{a}, "Empty journal did not restart");
  bool rejected = false;
  std::filesystem::remove(path);
  auto many_path=path;many_path+=".many";
  std::filesystem::remove(many_path);
  std::vector<Entry> many;
  constexpr size_t count=8192;
  many.reserve(count);
  for(size_t i=0;i<count;++i) {
    auto entry=a;entry[12]=static_cast<u32>((i*7919)%count);
    many.push_back(entry);
  }
  AppendBatch(many_path,many);
  Check(Read(many_path).entries==many,"Many-entry order/rehash mismatch");
  // The physical size limit still applies even when all frames duplicate.
  std::filesystem::resize_file(many_path,Magic.size()+MaxEntries*(BatchHeader+EntryBytes+BatchDigest)+1);
  rejected=false;
  try{Read(many_path);}catch(const std::exception&){rejected=true;}
  Check(rejected,"Oversized journal was accepted");
  std::filesystem::remove(many_path);
  {
    // Compute dispatches share the journal and its first-use order.
    Entry compute{};
    compute[0] = ComputeFlag;
    for (size_t i = 1; i <= ComputeWords; ++i) compute[i] = static_cast<u32>(i * 977);
    Check(IsCompute(compute) && !IsCompute(a), "Compute entry flag");
    AppendBatch(path, std::array{a, compute, c});
    saved = Read(path);
    Check(saved.entries == std::vector<Entry>({a, compute, c}),
          "Compute entry lost or reordered");
    // Words past the key are reserved; a frame using them is not ours.
    auto foreign = compute;
    foreign[ComputeWords + 1] = 1;
    Append(path, foreign);
    rejected = false;
    try { Read(path); } catch (const std::exception &) { rejected = true; }
    Check(rejected, "Compute entry with trailing words was accepted");
    std::filesystem::remove(path);
    // Journals of earlier formats are converted offline, never read.
    { std::ofstream out(path, std::ios::binary); out << "NXREC002"; }
    rejected = false;
    try { Read(path); } catch (const std::exception &) { rejected = true; }
    Check(rejected, "Earlier journal format was accepted");
    std::filesystem::remove(path);
  }
  std::cout << "PASS: cross-GPU combinations, restart, torn append recovery, "
               "corruption rejection, compute entries\n";
} catch (const std::exception &e) {
  std::cerr << e.what() << '\n';
  return 1;
}
