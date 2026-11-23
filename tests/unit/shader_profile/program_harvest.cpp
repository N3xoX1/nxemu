// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile_harvest.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace VideoCommon::ShaderProfile::Harvest;
namespace {
void Require(bool value, const char* what) {
    if (!value) throw std::runtime_error(std::string("program harvest: ") + what);
}
void Put(std::string& bytes, size_t offset, uint64_t value, size_t size) {
    for (size_t i = 0; i < size; ++i) bytes[offset + i] = static_cast<char>(value >> (8 * i));
}
constexpr uint64_t SelfBranch = 0xE2400FFFFF87000FULL;
constexpr size_t ProgramBytes = 0x50 + 0x10 + 8; // header, two instructions, the self branch

// A Switch 1 code area: NVN's code header, then the program header (stage 1 vertex .. 5 fragment)
// or, for compute (stage 0), the instructions straight away.
void Code(std::string& bytes, size_t at, uint32_t stage, uint64_t instruction) {
    size_t program = at;
    if (stage) {
        Put(bytes, at, 0x12345678, 4);
        program += 0x30;
        Put(bytes, program, (stage == 5 ? 2 : 1) | (3 << 5) | (stage << 10), 4);
        program += 0x50;
    }
    Put(bytes, program, instruction, 8);
    Put(bytes, program + 8, instruction + 1, 8);
    Put(bytes, program + 16, SelfBranch, 8);
}
// One BNSH: variation 0 draws a vertex and a fragment program, variation 1 is a compute program.
std::string Bnsh(uint32_t control_version, uint32_t vertex_header_stage = 1) {
    std::string b(0x1000, '\0');
    std::memcpy(b.data(), "BNSH\0\0\0\0", 8);
    Put(b, 8, 0x00020102, 4); // version 2.1.2.0, as Switch 1 games ship it
    Put(b, 0xc, 0xfeff, 2);
    Put(b, 0x1c, b.size(), 4);
    std::memcpy(b.data() + 0x60, "grsc", 4);
    Put(b, 0x7c, 2, 4);
    Put(b, 0x80, 0xc0, 8);
    Put(b, 0xc0 + 0x10, 0x200, 8);
    Put(b, 0x100 + 0x10, 0x280, 8);
    Put(b, 0x200 + 0x08, 0x300, 8); // vertex
    Put(b, 0x200 + 0x28, 0x340, 8); // fragment
    Put(b, 0x280 + 0x30, 0x380, 8); // compute
    const size_t records[]{0x300, 0x340, 0x380}, controls[]{0x400, 0x440, 0x480}, codes[]{0x800, 0x900, 0xa00};
    const uint32_t stages[]{vertex_header_stage, 5, 0};
    for (size_t i = 0; i < 3; ++i) {
        Put(b, records[i] + 0x08, controls[i], 8);
        Put(b, records[i] + 0x10, codes[i], 8);
        Put(b, records[i] + 0x18, 0x100, 4);
        Put(b, records[i] + 0x1c, 0x20, 4);
        Put(b, controls[i], 0x98761234, 4);
        Put(b, controls[i] + 4, control_version, 4);
        Code(b, codes[i], stages[i], 0x1000 * (i + 1));
    }
    return b;
}
// The BNSH inside other data, then a graphics program of no known container.
std::string Wrapped(const std::string& bnsh) {
    std::string data(0x40, '\x5a');
    data += bnsh;
    const size_t loose = data.size();
    data.resize(loose + 0x200, '\0');
    Code(data, loose, 1, 0x9000);
    return data;
}
} // namespace
int main() try {
    {
        Scan scan;
        ScanBytes(Wrapped(Bnsh(1)), scan);
        Require(scan.errors.empty() && scan.containers == 1 && scan.by_content == 1 && scan.programs.size() == 4, "Switch 1 scan");
        const auto& v = scan.programs[0];
        const auto& f = scan.programs[1];
        const auto& c = scan.programs[2];
        const auto& loose = scan.programs[3];
        Require(v.platform == Switch1 && v.stage == VertexB && f.stage == Fragment && c.stage == Compute && loose.stage == VertexB,
                "stages");
        Require(v.code.size() == ProgramBytes && f.code.size() == ProgramBytes && c.code.size() == 0x18 && loose.code.size() == ProgramBytes,
                "programs run from their header through the self branch");
        Require(v.offset == 0x40 + 0x800 + 0x30 && c.offset == 0x40 + 0xa00 && loose.offset == 0x40 + 0x1000 + 0x30, "offsets");
        Require(!v.control.empty() && loose.control.empty(), "control sections");
        Require(scan.groups.size() == 2 && scan.groups[0] == std::vector<uint32_t>{0, 1} && scan.groups[1] == std::vector<uint32_t>{2},
                "variations give the programs drawn together");
    }
    {
        // Switch 2: the control version names the platform; the code area is kept whole.
        Scan scan;
        ScanBytes(Bnsh(0xc9), scan);
        Require(scan.errors.empty() && scan.programs.size() == 3 && scan.programs[0].platform == Switch2 &&
                    scan.programs[0].code.size() == 0x100 && scan.by_content == 0,
                "Switch 2 BNSH");
    }
    {
        // A BNSH that does not read back is reported, gives no variation, and its graphics programs
        // are still found by content.
        for (const auto& bad : {Bnsh(7), Bnsh(1, 5)}) {
            Scan scan;
            ScanBytes(bad, scan);
            Require(scan.errors.size() == 1 && scan.containers == 0 && scan.groups.empty() && scan.by_content == 2 &&
                        scan.programs.size() == 2,
                    "unreadable BNSH");
        }
        auto truncated = Bnsh(1);
        truncated.resize(0x900);
        Scan scan;
        ScanBytes(truncated, scan);
        Require(scan.containers == 0 && scan.by_content == 1, "BNSH larger than the data");
    }
    {
        std::string none(0x200, '\0');
        Require(ProgramSize(none) == 0, "no self branch");
        Put(none, 0x48, SelfBranch, 8);
        Require(ProgramSize(none) == 0x50, "self branch ends the program");
        Require(HeaderStage(none.substr(0, 0x50)) == 0xff, "zero header is no program");
    }
    std::cout << "BNSH variations, Switch 1 and 2 programs, content scan and bad containers checked\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
