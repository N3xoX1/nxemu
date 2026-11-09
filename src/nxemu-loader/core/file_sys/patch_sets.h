// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/file_sys/vfs/vfs_types.h"

namespace FileSys {

// Patch sets (.nxpatch in a mod's exefs folder): code patches for the module with the given build id, each of which may
// replace one instruction with several:
//
//   @nsobid-<build id>                                  the module's build id (trailing zeros may be left out)
//   @version <text>                                     optional: the game version, shown in logs and the
//                                                       add-ons list; the build id alone decides what applies
//   set <name> <env> <default>                          a patch set, on when <default> is 1; the environment
//                                                       variable <env> (0 or 1) overrides it
//   site <set> <module> <offset> <original> <word>...   at module offset <offset>, whose instruction must be
//                                                       <original>, run the words instead of it
//
// A word is an instruction (hex) encoded as if it were at the site, or @page:x<n>: adrp x<n> to the patch page.
// Only the last word may branch. A site of one word is patched in place; a site of more words branches to a cave
// the loader places in the cave pages after the module, holding the words and a branch back to the instruction
// after the site, so patches never name cave addresses and never share one.
//
// The patch page is the page after the module's .bss, read-write, for values the emulator writes for patches to
// read. At every present the frame step (nxemu-os core/frame_step.h) writes at its start: +0 the step in 30 Hz
// frames, +4 the step in seconds, +8 the step in 60 Hz frames (floats), +12 1 while a video plays, else 0 (a 32-bit
// integer). Without a writer (page_written) the loader leaves out sets that read it. The cave pages follow it in the
// same read-write region: Horizon's rtld takes every executable region for a module. With native code execution the
// host runs the caves, so they start on a host page (cave_alignment) after that region, unmapped for the guest and
// executable for the host, as the native code patcher's section is. Nothing rewrites a cave as the patcher rewrites
// .text, so a set is left out when a cave word is an exception, a system instruction or register other than a hint,
// a barrier, NZCV, FPCR or FPSR, or an exclusive: wider than what the patcher rewrites, to stay on the safe side.
//
// A set is applied whole or not at all: it is left out when a site does not hold its original instruction (another
// game version, an IPS or IPSwitch patch changed it), when an earlier set already patched one of its sites, or when
// a word cannot run from a cave. "spin" lines and comments (#) are ignored.
//
// Limits, each logged when hit: MaxFileSize per file, MaxSitesPerFile, MaxWordsPerSite, MaxCaveBytes per module.
class PatchSets
{
public:
    static constexpr uint32_t PageSize = 0x1000;
    static constexpr uint64_t MaxFileSize = 1 << 20;
    static constexpr size_t MaxSitesPerFile = 4096;
    static constexpr size_t MaxWordsPerSite = 64;
    static constexpr uint32_t MaxCaveBytes = 1 << 20;

    // What the emulator provides for patch sets
    struct Support
    {
        // Something in this build writes the patch page; without it, sets that read the page are left out
        bool page_written{};
        // The host runs the caves (native code execution): leave out sets with a cave word it cannot run there
        bool native_code{};
        // The caves start, and end, on this boundary after the patch page: the host page with native code execution
        uint32_t cave_alignment{PageSize};
    };

    // Reads the patch set files of the enabled mods for the module with this build id (hex, trailing zeros
    // optional). Deterministic: the layout pass and the load pass see the same reservation.
    PatchSets(const std::vector<VirtualFile> & files, const std::string & build_id, const std::string & module_name, Support support);

    // The @version of a patch set file, or empty when it has none (the add-ons list)
    [[nodiscard]] static std::string FileVersion(const VirtualFile & file);

    // Each set a patch set file names and whether its default and environment variable leave it on, a line of
    // "<name>=<0|1>" each, for any build id. Empty when the file has none.
    [[nodiscard]] static std::string FileSetSwitches(const VirtualFile & file);

    // True when no set that is on has a site for this module
    [[nodiscard]] bool Empty() const;

    // Bytes to reserve after the module's image: the patch page then the cave pages; 0 when the sets that are on
    // patch in place only and do not read the patch page. The patch page must then start on cave_alignment.
    [[nodiscard]] uint32_t ReservedSize() const;

    // Where the reservation goes after a module image that ends at image_end (module-relative, on a guest page):
    // the patch page, and how much the data segment and the image grow. Without a reservation nothing moves or grows.
    struct Layout
    {
        uint32_t page_offset{};
        uint32_t data_growth{};
        uint32_t image_growth{};
    };
    [[nodiscard]] Layout Reserve(uint32_t image_end) const;

    // The cave pages, from the patch page: where they start and their size (0 without caves)
    [[nodiscard]] uint32_t CaveOffset() const;
    [[nodiscard]] uint32_t CaveSize() const;

    // Logs what reading the files left out (malformed lines, sets that read an unwritten patch page). Kept apart
    // from the constructor, which both loader passes run, so the load pass reports them once.
    void LogProblems() const;

    // Patches the module image (module-relative: offset 0 is the start of .text). page_offset is the patch page,
    // followed by the cave pages. Each set is encoded and checked whole before any of it is written, so a set whose
    // site does not hold its original instruction, or whose word cannot run from a cave, changes nothing. Returns
    // whether an applied set reads the patch page. report: log each set's result (the load pass).
    bool Apply(std::span<uint8_t> image, uint32_t page_offset, bool report = true);

private:
    struct Word
    {
        uint32_t value{};
        int page_reg{-1}; // >= 0: adrp x<page_reg>, patch page
    };
    struct Site
    {
        size_t set{};
        uint32_t offset{};
        uint32_t original{};
        std::vector<Word> words;
        int line{};
    };
    struct Set
    {
        std::string name;
        std::string file; // file name, then " (<version>)" when the file has one
        bool enabled{};
        std::vector<Site> sites;
    };

    void Parse(const VirtualFile & file, const std::string & build_id, const std::string & module_name);
    // The words of a multi-word site encoded for a cave at cave, then the branch back after the site; nullopt when
    // a word's target is out of its range from there
    static std::optional<std::vector<uint32_t>> CaveWords(const Site & site, uint32_t cave, uint32_t page_offset);

    std::vector<Set> m_sets;
    std::vector<std::string> m_problems;
    uint32_t m_cave_alignment{PageSize};
    bool m_native_code{};
    uint32_t m_cave_bytes{};
    bool m_uses_page{};
};

} // namespace FileSys
