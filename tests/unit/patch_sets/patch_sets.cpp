// SPDX-License-Identifier: GPL-2.0-or-later
// Patch sets (nxemu-loader core/file_sys/patch_sets.h) applied to synthetic modules: in-place and cave sites, every
// PC-relative form a cave word may take, whole-set rejection, set order and switches. The decoders below are written
// from the Arm ARM, apart from the production encoders, so a shared mistake does not pass.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/file_sys/patch_sets.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace {

using FileSys::PatchSets;

constexpr uint32_t Nop = 0xD503201F;
constexpr uint32_t AddX0 = 0x91000400; // add x0, x0, #1
constexpr uint32_t MovW1 = 0x52800041; // mov w1, #2

void Check(bool value, const char * message)
{
    if (!value)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
}

void Check(bool value, const std::string & message)
{
    Check(value, message.c_str());
}

void SetEnv(const char * name, const char * value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

int64_t Sext(uint64_t value, int bits)
{
    const uint64_t sign = uint64_t{1} << (bits - 1);
    return static_cast<int64_t>((value ^ sign) - sign);
}

// Targets of the encodings the loader writes, decoded at pc
uint64_t TargetB(uint32_t w, uint64_t pc)
{
    Check((w & 0x7C000000) == 0x14000000, "b or bl");
    return pc + Sext(w & 0x3FFFFFF, 26) * 4;
}
uint64_t TargetImm19(uint32_t w, uint64_t pc) // b.cond, cbz, ldr literal
{
    return pc + Sext((w >> 5) & 0x7FFFF, 19) * 4;
}
uint64_t TargetTb(uint32_t w, uint64_t pc)
{
    return pc + Sext((w >> 5) & 0x3FFF, 14) * 4;
}
uint64_t TargetAdr(uint32_t w, uint64_t pc)
{
    const int64_t imm = Sext(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3), 21);
    return (w & 0x80000000) != 0 ? (pc & ~uint64_t{0xFFF}) + imm * 0x1000 : pc + imm;
}

struct Module
{
    std::vector<uint8_t> image;
    uint32_t page{};

    Module(uint32_t page_offset, uint32_t reserved) : image(page_offset + reserved), page(page_offset)
    {
        for (uint32_t offset = 0; offset < page_offset; offset += 4)
        {
            Set(offset, Nop);
        }
    }
    uint32_t At(uint32_t offset) const
    {
        uint32_t word;
        std::memcpy(&word, image.data() + offset, 4);
        return word;
    }
    void Set(uint32_t offset, uint32_t word)
    {
        std::memcpy(image.data() + offset, &word, 4);
    }
};

FileSys::VirtualFile File(const std::string & text)
{
    return std::make_shared<FileSys::VectorVfsFile>(std::vector<uint8_t>(text.begin(), text.end()), "test.nxpatch");
}

PatchSets Make(const std::string & text, bool page_written = true, bool native_code = false)
{
    return PatchSets({File(text)}, "AB12", "main",
                     {.page_written = page_written, .native_code = native_code, .cave_alignment = native_code ? 0x4000U : PatchSets::PageSize});
}

std::string Hex(uint32_t value)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%08x", value);
    return text;
}

// The cave a patched site branches to
uint32_t CaveOf(const Module & module, uint32_t site)
{
    return static_cast<uint32_t>(TargetB(module.At(site), site));
}

void InPlaceAndCave()
{
    const PatchSets probe = Make("@nsobid-ab12\nset s ENV_UNUSED 1\nsite s main 100 d503201f 52800041\n"
                                 "site s main 200 d503201f 91000400 52800041\n");
    Check(probe.ReservedSize() == PatchSets::PageSize * 2, "one cave reserves the patch page and a cave page");
    PatchSets sets = probe;
    Module module(0x10000, sets.ReservedSize());
    Check(!sets.Apply(module.image, module.page), "no set reads the patch page");
    Check(module.At(0x100) == MovW1, "one word replaces the instruction in place");
    const uint32_t cave = CaveOf(module, 0x200);
    Check(cave == module.page + PatchSets::PageSize, "the first cave starts after the patch page");
    Check(module.At(cave) == AddX0 && module.At(cave + 4) == MovW1, "the cave holds the words");
    Check(TargetB(module.At(cave + 8), cave + 8) == 0x204, "the cave branches back after the site");
    std::puts("PASS: in place and cave sites");
}

void Relocation()
{
    // Encoded at the site 0x300: adr x1 0x320, adrp x2 page 0x1000, ldr x3 literal 0x340, then b.al 0x380. The
    // other sites end in the branches only a last word may be: b.eq, cbz, tbz and bl.
    PatchSets sets = Make("@nsobid-ab12\nset r ENV_UNUSED 1\n"
                          "site r main 300 d503201f 10000101 b0000002 58000203 5400040e\n"
                          "site r main 400 d503201f d503201f 54000800\n"
                          "site r main 500 d503201f d503201f b4000a05\n"
                          "site r main 600 d503201f d503201f 36180e06\n"
                          "site r main 700 d503201f d503201f 94000010\n");
    Module module(0x10000, sets.ReservedSize());
    sets.Apply(module.image, module.page);

    uint32_t cave = CaveOf(module, 0x300);
    Check(TargetAdr(module.At(cave), cave) == 0x320 && (module.At(cave) & 0x9F00001F) == 0x10000001, "adr keeps its target and register");
    Check(TargetAdr(module.At(cave + 4), cave + 4) == 0x1000 && (module.At(cave + 4) & 0x9F00001F) == 0x90000002,
          "adrp keeps its page and register");
    Check(TargetImm19(module.At(cave + 8), cave + 8) == 0x340 && (module.At(cave + 8) & 0xFF00001F) == 0x58000003,
          "ldr literal keeps its target and register");
    Check((module.At(cave + 12) & 0xFC000000) == 0x14000000 && TargetB(module.At(cave + 12), cave + 12) == 0x380,
          "b.al becomes a branch to its target");
    Check(TargetB(module.At(cave + 16), cave + 16) == 0x304, "after b.al the cave branches back");

    cave = CaveOf(module, 0x700);
    Check((module.At(cave + 4) & 0xFC000000) == 0x94000000 && TargetB(module.At(cave + 4), cave + 4) == 0x740, "bl keeps its target");
    Check(TargetB(module.At(cave + 8), cave + 8) == 0x704, "the callee returns to the branch back");

    // b.eq 0x400 + 0x100: b.ne over a long branch to the target, then the branch back
    cave = CaveOf(module, 0x400);
    Check(module.At(cave + 4) == 0x54000041, "b.eq becomes b.ne to skip the next word");
    Check(TargetB(module.At(cave + 8), cave + 8) == 0x500, "then a branch to the b.eq target");
    Check(TargetB(module.At(cave + 12), cave + 12) == 0x404, "then the branch back");

    cave = CaveOf(module, 0x500);
    Check(module.At(cave + 4) == 0xB5000045, "cbz x5 becomes cbnz x5 to skip the next word");
    Check(TargetB(module.At(cave + 8), cave + 8) == 0x500 + 0x140, "then a branch to the cbz target");

    cave = CaveOf(module, 0x600);
    Check(module.At(cave + 4) == 0x37180046, "tbz w6, #3 becomes tbnz w6, #3 to skip the next word");
    Check(TargetB(module.At(cave + 8), cave + 8) == 0x600 + 0x1C0, "then a branch to the tbz target");
    Check(TargetTb(module.At(cave + 4), cave + 4) == cave + 12, "the skip lands on the branch back");
    std::puts("PASS: PC-relative words in caves");
}

void UnreachableWordLeavesSetOut()
{
    // 3 MiB of code puts the caves beyond the 1 MiB reach of adr and ldr literal at the start
    for (const char * word : {"10000101", "58000203"})
    {
        PatchSets sets = Make(std::string("@nsobid-ab12\nset far ENV_UNUSED 1\nsite far main 100 d503201f 52800041\n"
                                          "site far main 200 d503201f 91000400 ") + word + "\n"
                              "set next ENV_UNUSED 1\nsite next main 300 d503201f 91000400\n");
        Module module(0x300000, sets.ReservedSize());
        const std::vector<uint8_t> cave(module.image.begin() + module.page, module.image.end());
        sets.Apply(module.image, module.page);
        Check(module.At(0x100) == Nop && module.At(0x200) == Nop, "a set with an unreachable cave word changes no site");
        Check(std::equal(cave.begin(), cave.end(), module.image.begin() + module.page), "nor any cave");
        Check(module.At(0x300) == AddX0, "the next set still applies");
    }
    std::puts("PASS: a word that cannot run from a cave leaves its set out whole");
}

void LargeModule()
{
    // 144 MiB: the caves are beyond branch reach of the start, in-place sites are not affected
    PatchSets sets = Make("@nsobid-ab12\nset inplace ENV_UNUSED 1\nsite inplace main 100 d503201f 91000400\n"
                          "set cave ENV_UNUSED 1\nsite cave main 200 d503201f 91000400 52800041\n");
    Module module(144u << 20, sets.ReservedSize());
    sets.Apply(module.image, module.page);
    Check(module.At(0x100) == AddX0, "an in-place site applies whatever the module size");
    Check(module.At(0x200) == Nop, "a cave out of branch reach leaves its set out");
    std::puts("PASS: large modules");
}

void SetsAndSwitches()
{
    SetEnv("NXEMU_TEST_SET_OFF", "0");
    SetEnv("NXEMU_TEST_SET_ON", "1");
    PatchSets sets = Make("@nsobid-AB12000000\n"
                          "set first ENV_UNUSED 1\nsite first main 100 d503201f 91000400\n"
                          "set second ENV_UNUSED 1\nsite second main 100 d503201f 52800041\nsite second main 104 d503201f 52800041\n"
                          "set wrong ENV_UNUSED 1\nsite wrong main 108 d503201f 52800041\nsite wrong main 10c 12345678 52800041\n"
                          "set off NXEMU_TEST_SET_OFF 1\nsite off main 110 d503201f 52800041\n"
                          "set on NXEMU_TEST_SET_ON 0\nsite on main 114 d503201f 52800041\n"
                          "set other ENV_UNUSED 1\nsite other sdk 118 d503201f 52800041\n"
                          "spin main 120 130\n");
    Check(sets.ReservedSize() == 0, "in-place sets reserve nothing");
    Module module(0x10000, 0);
    sets.Apply(module.image, module.page);
    Check(module.At(0x100) == AddX0, "a site stays with the first set that patched it");
    Check(module.At(0x104) == Nop, "a set sharing a site with an earlier set is left out whole");
    Check(module.At(0x108) == Nop, "a set with an unexpected original instruction changes nothing");
    Check(module.At(0x110) == Nop && module.At(0x114) == MovW1, "the environment overrides a set's default");
    Check(module.At(0x118) == Nop, "sites of other modules are ignored");
    std::puts("PASS: set order, original instructions and switches");
}

void PatchPage()
{
    const std::string text = "@nsobid-ab12\nset page ENV_UNUSED 1\nsite page main 100 d503201f @page:x9\n"
                             "site page main 200 d503201f @page:x8 bd400900\n";
    PatchSets sets = Make(text);
    Module module(0x10000, sets.ReservedSize());
    Check(sets.Apply(module.image, module.page), "a set reading the patch page reports it");
    Check(TargetAdr(module.At(0x100), 0x100) == module.page && (module.At(0x100) & 0x9F00001F) == 0x90000009,
          "@page in place is adrp x9 to the patch page");
    const uint32_t cave = CaveOf(module, 0x200);
    Check(TargetAdr(module.At(cave), cave) == module.page && (module.At(cave) & 0x1F) == 8, "@page in a cave is adrp to the patch page");

    Check(Make(text, false).Empty(), "without a page writer, sets that read the page are left out");
    std::puts("PASS: patch page");
}

void NativeCode()
{
    // Native code execution: the caves start and end on the host page (16 KiB here) after the patch page
    const std::string text = "@nsobid-ab12\nset cave ENV_UNUSED 1\nsite cave main 200 d503201f 91000400 52800041\n";
    PatchSets sets = Make(text, true, true);
    Check(sets.CaveOffset() == 0x4000 && sets.CaveSize() == 0x4000, "the caves take whole host pages after the patch page");
    Check(sets.ReservedSize() == 0x8000, "the reservation covers the patch page's host page and the caves");
    Module module(0x10000, sets.ReservedSize());
    sets.Apply(module.image, module.page);
    Check(CaveOf(module, 0x200) == module.page + 0x4000, "the first cave starts on the host page after the patch page");
    Module unaligned(0x11000, sets.ReservedSize());
    Check(!sets.Apply(unaligned.image, unaligned.page) && unaligned.At(0x200) == Nop, "a patch page off the host page patches nothing");

    // The loader's layout after an image ending off the host page (0x11000): the patch page moves to the host page,
    // the data segment takes the patch page's host page and the image the caves too
    const PatchSets::Layout caves = sets.Reserve(0x11000);
    Check(caves.page_offset == 0x14000 && caves.data_growth == 0x7000 && caves.image_growth == 0xB000,
          "with native code execution the caves stay out of the data segment");
    const PatchSets::Layout jit = Make(text).Reserve(0x11000);
    Check(jit.page_offset == 0x11000 && jit.data_growth == 0x2000 && jit.image_growth == 0x2000, "in the JIT the caves are data");
    const PatchSets::Layout page = Make("@nsobid-ab12\nset p ENV_UNUSED 1\nsite p main 100 d503201f @page:x9\n", true, true).Reserve(0x11000);
    Check(page.page_offset == 0x14000 && page.data_growth == 0x3000 + 0x4000 && page.image_growth == page.data_growth,
          "a patch page without caves is all data");
    // In place only: no reservation, so nothing moves or grows and the sets apply wherever the image ends
    const std::string in_place = "@nsobid-ab12\nset s ENV_UNUSED 1\nsite s main 200 d503201f 52800041\n";
    PatchSets in_place_sets = Make(in_place, true, true);
    const PatchSets::Layout none = in_place_sets.Reserve(0x11000);
    Check(none.page_offset == 0x11000 && none.data_growth == 0 && none.image_growth == 0, "in-place sets reserve nothing");
    Module in_place_module(0x11000, 0);
    in_place_sets.Apply(in_place_module.image, in_place_module.page);
    Check(in_place_module.At(0x200) == 0x52800041, "in-place sets apply after an image ending off the host page");

    // Words the native code patcher rewrites in .text cannot run from a cave; in place the patcher still sees them
    const char * refused[] = {"d4000001", "d53bd040", "d53be020", "c85f7c20", "c8007c20", "d50b7520", "d4200000"};
    for (const char * word : refused)
    {
        const std::string cave = std::string("@nsobid-ab12\nset s ENV_UNUSED 1\nsite s main 200 d503201f 91000400 ") + word + "\n";
        Check(Make(cave, true, true).Empty(), std::string("cave word ") + word + " leaves its set out with native code execution");
        Check(!Make(cave).Empty(), std::string("cave word ") + word + " runs from a cave in the JIT");
        Check(!Make(std::string("@nsobid-ab12\nset s ENV_UNUSED 1\nsite s main 200 d503201f ") + word + "\n", true, true).Empty(),
              std::string("word ") + word + " in place applies with native code execution");
    }
    // Hints, barriers and the NZCV, FPCR and FPSR registers run anywhere
    const char * allowed[] = {"d503201f", "d5033bbf", "d5033fdf", "d53b4200", "d51b4200", "d53b4400", "d53b4420", "d51b4400", "c8dffc20"};
    for (const char * word : allowed)
    {
        Check(!Make(std::string("@nsobid-ab12\nset s ENV_UNUSED 1\nsite s main 200 d503201f 91000400 ") + word + "\n", true, true).Empty(),
              std::string("cave word ") + word + " runs from a cave with native code execution");
    }
    std::puts("PASS: native code execution");
}

void Limits()
{
    std::string words;
    for (size_t i = 0; i < PatchSets::MaxWordsPerSite; i++)
    {
        words += " d503201f";
    }
    Check(!Make("@nsobid-ab12\nset s ENV_UNUSED 1\nsite s main 100 d503201f" + words + "\n").Empty(), "a site may hold MaxWordsPerSite words");
    Check(Make("@nsobid-ab12\nset s ENV_UNUSED 1\nsite s main 100 d503201f" + words + " d503201f\n").Empty(),
          "a site of more words leaves its set out");

    std::string sites = "@nsobid-ab12\nset a ENV_UNUSED 1\n";
    for (size_t i = 0; i < PatchSets::MaxSitesPerFile; i++)
    {
        sites += "site a main 100 d503201f 52800041\n";
    }
    PatchSets many = Make(sites + "set b ENV_UNUSED 1\nsite b main 200 d503201f 52800041\n");
    Module module(0x10000, 0);
    many.Apply(module.image, module.page);
    Check(module.At(0x200) == Nop, "sites past MaxSitesPerFile are ignored");

    std::string large = "@nsobid-ab12\nset s ENV_UNUSED 1\nsite s main 100 d503201f 52800041\n";
    large.append(PatchSets::MaxFileSize, '#');
    Check(Make(large).Empty(), "a file over MaxFileSize is ignored");
    std::puts("PASS: limits");
}

void Version()
{
    Check(PatchSets::FileVersion(File("# comment\n@nsobid-ab12\n@version 1.4.3\nset s ENV_UNUSED 1\n")) == "1.4.3", "@version is read");
    Check(PatchSets::FileVersion(File("@nsobid-ab12\nset s ENV_UNUSED 1\n@version 9.9.9\n")).empty(), "@version after the sets is not the file's");
    Check(!Make("@version 1.4.3\n@nsobid-ab12\nset s ENV_UNUSED 1\nsite s main 100 d503201f 52800041\n").Empty(),
          "@version before @nsobid is accepted");
    std::puts("PASS: @version");
}

void Rejected()
{
    Check(Make("@nsobid-cd34\nset s ENV_UNUSED 1\nsite s main 100 d503201f 52800041\n").Empty(), "another build id applies nothing");
    Check(Make("set s ENV_UNUSED 1\nsite s main 100 d503201f 52800041\n").Empty(), "no build id applies nothing");
    Check(Make("@nsobid-ab12\nset s ENV_UNUSED 1\nsite s main 100 d503201f 14000010 52800041\n").Empty(),
          "a branch before the last word leaves the set out");
    Check(Make("@nsobid-ab12\nset s ENV_UNUSED 1\nsite s main 102 d503201f 52800041\n").Empty(), "an unaligned site leaves the set out");
    Check(Make("@nsobid-ab12\nset s ENV_UNUSED 1\nsite s main 100 d503201f @page:x31\n").Empty(), "@page takes x0 to x30");
    std::puts("PASS: malformed sets");
}

} // namespace

int main()
{
    InPlaceAndCave();
    Relocation();
    UnreachableWordLeavesSetOut();
    LargeModule();
    SetsAndSwitches();
    PatchPage();
    NativeCode();
    Limits();
    Version();
    Rejected();
    return 0;
}
