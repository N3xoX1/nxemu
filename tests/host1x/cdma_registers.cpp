#include <cstdio>
#include <initializer_list>
#include "yuzu_video_core/cdma_pusher.h"
#include "probe-devices.h"

static unsigned failures;
static Tegra::ChCommandHeader Word(u32 value) {
    Tegra::ChCommandHeader word{};
    word.raw = value;
    return word;
}
static void Run(const char* name, std::initializer_list<u32> words,
                std::initializer_list<Tegra::Host1x::Call> expected, bool split = false) {
    Tegra::Host1x::Host1x host;
    Tegra::CDmaPusher pusher{host};
    Tegra::ChCommandHeaderList buffer;
    for (auto word : words) {
        buffer.push_back(Word(word));
        if (split) { pusher.ProcessEntries(std::move(buffer)); buffer.clear(); }
    }
    pusher.ProcessEntries(std::move(buffer));
    const bool ok = host.calls == std::vector<Tegra::Host1x::Call>{expected};
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}
int main() {
    constexpr u32 nvdec = 0xf0 << 6;
    constexpr u32 vic = 0x5d << 6;
    constexpr u32 control = 1 << 6;
    Run("NVDEC SetClass mask", {0x00100003 | nvdec, 0xc0, 123}, {{0, 0xc0, 123}});
    Run("NVDEC sparse mask", {nvdec, 0x300f0006, 0xc0, 124}, {{0, 0xc0, 124}});
    Run("NVDEC split mask", {0x00100003 | nvdec, 0xc1, 125}, {{0, 0xc1, 125}}, true);
    Run("Host1x sparse mask", {control, 0x304e0005, 7, 9}, {{2, 0x4e, 7}, {2, 0x50, 9}});
    Run("NVDEC incrementing", {nvdec, 0x10100002, 0xc2, 126}, {{0, 0xc2, 126}});
    Run("NVDEC nonincrementing", {nvdec, 0x401000c3, 0x20110002, 127, 128}, {{0, 0xc3, 127}, {0, 0xc3, 128}});
    Run("VIC SetClass mask", {0x00100003 | vic, 0xc0, 129}, {{1, 0xc0, 129}});
    return failures ? 1 : 0;
}
