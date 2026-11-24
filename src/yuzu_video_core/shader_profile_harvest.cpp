// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile_harvest.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace VideoCommon::ShaderProfile::Harvest
{
namespace
{
constexpr u64 SelfBranchA = 0xE2400FFFFF87000FULL, SelfBranchB = 0xE2400FFFFF07000FULL;
constexpr size_t MaxProgram = 0x100000;
constexpr u32 CodeHeaderMagic = 0x12345678;   // NVN code header, before a Switch 1 graphics program
constexpr size_t CodeHeaderSize = 0x30;
constexpr u32 ControlMagic = 0x98761234;      // NVN control section
constexpr u32 Nvn1Control = 1, Nvn2Control = 0xc9;
constexpr size_t BnshMinimum = 0x98;
constexpr std::string_view BnshMagic{"BNSH\0\0\0\0", 8};

struct Bad : std::runtime_error
{
    using std::runtime_error::runtime_error;
};
template <typename T>
T Load(std::string_view data, u64 offset)
{
    if (offset > data.size() || sizeof(T) > data.size() - offset) throw Bad("read past the end");
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}
std::string_view Sub(std::string_view data, u64 offset, u64 size)
{
    if (offset > data.size() || size > data.size() - offset) throw Bad("range past the end");
    return data.substr(static_cast<size_t>(offset), static_cast<size_t>(size));
}

// A Switch 1 program cut from a code area: graphics programs start after the code header.
std::string_view Switch1Program(std::string_view code, bool graphics)
{
    if (graphics)
    {
        if (code.size() < CodeHeaderSize + 0x50 || Load<u32>(code, 0) != CodeHeaderMagic) throw Bad("no NVN code header");
        code.remove_prefix(CodeHeaderSize);
    }
    const auto size = ProgramSize(code);
    if (size == 0 || (graphics && size <= 0x50)) throw Bad("no self branch");
    return code.substr(0, size);
}

// One BNSH file. The layout is the same for every NVN version: +0x60 'grsc', +0x7c variation
// count, +0x80 the variation table (0x40 bytes each, the program at +0x10). A program has six
// stage records from +0x08 (vertex, tessellation control and evaluation, geometry, fragment,
// compute) and, in NVN2's 1.4.2 layout with program type 2, task and mesh at +0x40 and +0x48. A
// stage record has its control section at +0x08, its code at +0x10, then the code and control
// sizes as u32 at +0x18 and +0x1c. Offsets count from the BNSH start.
void Bnsh(std::string_view bnsh, size_t base, Scan & out)
{
    if (Sub(bnsh, 0x60, 4) != "grsc" || Load<u16>(bnsh, 0xc) != 0xfeff) throw Bad("not a little-endian BNSH");
    const std::array<u8, 4> version{static_cast<u8>(bnsh[8]), static_cast<u8>(bnsh[9]), static_cast<u8>(bnsh[10]), static_cast<u8>(bnsh[11])};
    const bool task_mesh = version == std::array<u8, 4>{1, 4, 2, 0};
    const auto count = Load<u32>(bnsh, 0x7c);
    const auto table = Load<u64>(bnsh, 0x80);
    Sub(bnsh, table, u64{count} * 0x40);
    std::vector<Program> found;
    std::vector<std::vector<u32>> groups;
    for (u32 variation = 0; variation < count; ++variation)
    {
        const auto program = Load<u64>(bnsh, table + u64{variation} * 0x40 + 0x10);
        if (program == 0) continue;
        std::vector<std::pair<u64, u8>> slots{{0x08, VertexB}, {0x10, TessControl}, {0x18, TessEval},
                                              {0x20, Geometry}, {0x28, Fragment}, {0x30, Compute}};
        if (task_mesh && Load<u64>(bnsh, program) == 2)
        {
            slots.emplace_back(0x40, Task);
            slots.emplace_back(0x48, Mesh);
        }
        std::vector<u32> group;
        for (const auto [slot, stage] : slots)
        {
            const auto record = Load<u64>(bnsh, program + slot);
            if (record == 0) continue;
            const auto control_offset = Load<u64>(bnsh, record + 0x08), code_offset = Load<u64>(bnsh, record + 0x10);
            const auto code_size = Load<u32>(bnsh, record + 0x18), control_size = Load<u32>(bnsh, record + 0x1c);
            const auto control = Sub(bnsh, control_offset, control_size);
            auto code = Sub(bnsh, code_offset, code_size);
            if (control.size() < 8 || Load<u32>(control, 0) != ControlMagic) throw Bad("stage without an NVN control section");
            const auto control_version = Load<u32>(control, 4);
            Program p{};
            p.stage = stage;
            p.control = control;
            if (control_version == Nvn1Control)
            {
                if (stage >= Task) throw Bad("Switch 1 task or mesh stage");
                const bool graphics = stage != Compute;
                const auto cut = Switch1Program(code, graphics);
                if (graphics && HeaderStage(cut.substr(0, 0x50)) != stage) throw Bad("stage disagrees with the program header");
                p.platform = Switch1;
                p.offset = base + code_offset + static_cast<size_t>(cut.data() - code.data());
                p.code = cut;
                if (control.size() >= 0x708)
                    if (const auto at = Load<u32>(control, 0x700), end = Load<u32>(control, 0x704); at <= end && end <= code.size())
                        p.constants = code.substr(at, end - at);
            }
            else if (control_version == Nvn2Control)
            {
                p.platform = Switch2;
                p.offset = base + code_offset;
                p.code = code;
            }
            else
                throw Bad("unknown NVN control version");
            group.push_back(static_cast<u32>(out.programs.size() + found.size()));
            found.push_back(p);
        }
        if (!group.empty()) groups.push_back(std::move(group));
    }
    // A BNSH is kept whole or not at all.
    out.programs.insert(out.programs.end(), found.begin(), found.end());
    for (auto & group : groups) out.groups.push_back(std::move(group));
    ++out.containers;
}

size_t Find(std::string_view data, std::string_view what, size_t from)
{
    while (from < data.size())
    {
        const void * hit = std::memchr(data.data() + from, what[0], data.size() - from);
        if (!hit) return std::string_view::npos;
        const auto at = static_cast<size_t>(static_cast<const char *>(hit) - data.data());
        if (data.substr(at, what.size()) == what) return at;
        from = at + 1;
    }
    return std::string_view::npos;
}
} // namespace

size_t ProgramSize(std::string_view code)
{
    const auto limit = std::min(code.size(), MaxProgram) & ~size_t{7};
    for (size_t at = 0; at < limit; at += 8)
    {
        u64 word;
        std::memcpy(&word, code.data() + at, 8);
        if (word == SelfBranchA || word == SelfBranchB) return at + 8;
    }
    return 0;
}

u8 HeaderStage(std::string_view header)
{
    if (header.size() < 0x50) return 0xff;
    u32 word;
    std::memcpy(&word, header.data(), 4);
    const u32 type = word & 31, version = (word >> 5) & 31, stage = (word >> 10) & 15;
    if (version != 3 || stage < 1 || stage > 5 || type != (stage == 5 ? 2U : 1U)) return 0xff;
    constexpr std::array<u8, 6> stages{0xff, VertexB, TessControl, TessEval, Geometry, Fragment};
    return stages[stage];
}

void ScanBytes(std::string_view data, Scan & out)
{
    // Containers first; their ranges are not searched by content again.
    std::vector<std::pair<size_t, size_t>> parsed;
    for (size_t at = Find(data, BnshMagic, 0); at != std::string_view::npos; at = Find(data, BnshMagic, at))
    {
        const auto size = at + 0x20 <= data.size() ? size_t{Load<u32>(data, at + 0x1c)} : 0;
        if (size < BnshMinimum || size > data.size() - at)
        {
            at += 8;
            continue;
        }
        try
        {
            Bnsh(data.substr(at, size), at, out);
            parsed.emplace_back(at, at + size);
            at += size;
        }
        catch (const Bad & error)
        {
            out.errors.push_back("BNSH at " + std::to_string(at) + ": " + error.what());
            at += 8;
        }
    }
    // Switch 1 graphics programs anywhere else, by their code header.
    constexpr char magic[4]{0x78, 0x56, 0x34, 0x12};
    size_t range = 0;
    for (size_t at = Find(data, {magic, 4}, 0); at != std::string_view::npos; at = Find(data, {magic, 4}, at + 4))
    {
        if (at % 4) continue;
        while (range < parsed.size() && parsed[range].second <= at) ++range;
        if (range < parsed.size() && parsed[range].first <= at) continue;
        const auto code = data.substr(at);
        if (code.size() < CodeHeaderSize + 0x50) break;
        const auto stage = HeaderStage(code.substr(CodeHeaderSize, 0x50));
        if (stage == 0xff) continue;
        try
        {
            const auto cut = Switch1Program(code, true);
            out.programs.push_back({Switch1, stage, cut, {}, {}, at + CodeHeaderSize});
            ++out.by_content;
        }
        catch (const Bad &) {}
    }
}
} // namespace VideoCommon::ShaderProfile::Harvest
