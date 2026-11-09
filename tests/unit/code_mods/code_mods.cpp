// SPDX-License-Identifier: GPL-2.0-or-later
// The hash of the enabled mods' ExeFS patches and cheats (nxemu-loader core/file_sys/code_mods.h). A modded run's shader
// cache identity comes from it, so a change to the code the mods apply has to change it.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "core/file_sys/code_mods.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace {

using FileSys::HashCodeMods;

void Check(bool value, const char * message)
{
    if (!value)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
}

FileSys::VirtualFile File(const std::string & name, const std::string & text)
{
    return std::make_shared<FileSys::VectorVfsFile>(std::vector<uint8_t>(text.begin(), text.end()), name);
}

FileSys::VirtualDir Dir(const std::string & name, std::vector<FileSys::VirtualFile> files)
{
    return std::make_shared<FileSys::VectorVfsDirectory>(std::move(files), std::vector<FileSys::VirtualDir>{}, name);
}

void SetEnv(const char * name, const char * value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

} // namespace

int main()
{
    Check(HashCodeMods({}).empty(), "No folders");
    Check(HashCodeMods({Dir("exefs", {})}).empty(), "Empty folder");

    const std::string patch = HashCodeMods({Dir("exefs", {File("60fps.nxpatch", "a")})});
    // ActiveProgramContent::code_mods holds 64 characters and a terminator
    Check(patch.size() == 64, "Hash length");
    Check(patch == HashCodeMods({Dir("exefs", {File("60fps.nxpatch", "a")})}), "Stable");

    Check(patch != HashCodeMods({Dir("exefs", {File("60fps.nxpatch", "b")})}), "File contents");
    Check(patch != HashCodeMods({Dir("exefs", {File("other.nxpatch", "a")})}), "File name");
    Check(patch != HashCodeMods({Dir("cheats", {File("60fps.nxpatch", "a")})}), "Folder kind");
    Check(patch != HashCodeMods({Dir("exefs", {File("60fps.nxpatch", "a"), File("more.ips", "c")})}), "Another file");

    Check(HashCodeMods({Dir("exefs", {File("a.ips", "1"), File("b.ips", "2")})}) ==
              HashCodeMods({Dir("exefs", {File("b.ips", "2"), File("a.ips", "1")})}),
          "Listing order within a folder");

    // The order mods apply in can change what they do
    const auto first = Dir("exefs", {File("a.ips", "1")});
    const auto second = Dir("exefs", {File("b.ips", "2")});
    Check(HashCodeMods({first, second}) != HashCodeMods({second, first}), "Mod order");

    // A patch set's environment variable changes the code it applies, not the file
    const auto sets = [](const std::string & text) { return HashCodeMods({Dir("exefs", {File("60fps.nxpatch", "@nsobid-1\n" + text)})}); };
    const std::string on_by_default = "set fps60 NXEMU_TEST_CODE_MODS 1\n";
    SetEnv("NXEMU_TEST_CODE_MODS", "");
    const std::string set_default = sets(on_by_default);
    SetEnv("NXEMU_TEST_CODE_MODS", "0");
    const std::string set_off = sets(on_by_default);
    Check(set_off != set_default, "Patch set switched off");
    SetEnv("NXEMU_TEST_CODE_MODS", "1");
    Check(sets(on_by_default) == set_default, "Patch set switched to what it already was");

    // Off by default, then switched on
    const std::string off_by_default = "set fps60 NXEMU_TEST_CODE_MODS 0\n";
    SetEnv("NXEMU_TEST_CODE_MODS", "");
    const std::string default_off = sets(off_by_default);
    SetEnv("NXEMU_TEST_CODE_MODS", "1");
    Check(sets(off_by_default) != default_off, "Patch set switched on");

    // Sets with their own variable, and text that is not a set
    SetEnv("NXEMU_TEST_CODE_MODS", "");
    const std::string two_sets = "set fps60 NXEMU_TEST_CODE_MODS 1\nset cloth NXEMU_TEST_CODE_MODS_2 1\n";
    SetEnv("NXEMU_TEST_CODE_MODS_2", "");
    const std::string both_on = sets(two_sets);
    SetEnv("NXEMU_TEST_CODE_MODS_2", "0");
    Check(sets(two_sets) != both_on, "Second patch set switched off");
    const std::string commented = "# set fps60 NXEMU_TEST_CODE_MODS 1\n";
    SetEnv("NXEMU_TEST_CODE_MODS", "");
    const std::string comment_default = sets(commented);
    SetEnv("NXEMU_TEST_CODE_MODS", "0");
    Check(sets(commented) == comment_default, "A set in a comment");

    // Native code execution leaves out sets the host cannot run, so it counts when there are sets, and only then
    const auto patch_dir = Dir("exefs", {File("60fps.nxpatch", "@nsobid-1\n" + on_by_default)});
    Check(HashCodeMods({patch_dir}, true) != HashCodeMods({patch_dir}, false), "Native code with patch sets");
    const auto ips_dir = Dir("exefs", {File("a.ips", "1")});
    Check(HashCodeMods({ips_dir}, true) == HashCodeMods({ips_dir}, false), "Native code without patch sets");
    Check(HashCodeMods({}, true).empty(), "Native code without mods");
    return 0;
}
