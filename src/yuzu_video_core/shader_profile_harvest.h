// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_common/common_types.h"
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Finds the shader programs a game ships, from its files alone, for Switch 1 and Switch 2.
// Two ways, used together:
// - Containers: BNSH files (alone, in BFSHA archives or inside any other data) list each
//   program with its stage and the programs drawn together (a variation). One reader serves
//   every NVN version: the layout is the same, the control section's version tells the platform.
// - Content: a Switch 1 graphics program starts with NVN's code header (0x12345678, then its
//   program header 0x30 bytes later), so programs of containers we do not know are found too.
namespace VideoCommon::ShaderProfile::Harvest
{
// Portable stages (docs/nxshader-format.md).
enum Stage : u8
{
    VertexA, VertexB, TessControl, TessEval, Geometry, Fragment, Compute, Task, Mesh
};
inline constexpr u8 Switch1 = 1, Switch2 = 2;
struct Program
{
    u8 platform{};
    u8 stage{};
    // Switch 1: the bytes the GPU runs from the program's start address, as the emulator reads
    // them (program header included for graphics) through the self branch that ends them.
    // Switch 2: the whole code area.
    std::string_view code;
    std::string_view control; // NVN control section; empty when found by content
    // Switch 1 from a BNSH: the program's own constants (constant bank 1), which the NVN control
    // section places after the code (offset at +0x700, code area size at +0x704).
    std::string_view constants;
    size_t offset{};          // of code in the scanned bytes
};
struct Scan
{
    std::vector<Program> programs;
    // Programs drawn together: one BNSH variation, as indices into programs.
    std::vector<std::vector<u32>> groups;
    u64 containers{}, by_content{};
    std::vector<std::string> errors;
};
// Adds the programs found in these bytes to `out`.
void ScanBytes(std::string_view data, Scan & out);
// Bytes from a program's start through its self branch, as the emulator sizes it; 0 when
// there is none in the first MiB.
size_t ProgramSize(std::string_view code);
// Whether these 80 bytes read as a Maxwell program header; its stage, else 0xff.
u8 HeaderStage(std::string_view header);
} // namespace VideoCommon::ShaderProfile::Harvest
