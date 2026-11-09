// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>
#include <unordered_map>

#include "core/file_sys/patch_sets.h"
#include "core/file_sys/vfs/vfs.h"
#include "yuzu_common/logging/log.h"

namespace FileSys {

namespace {

// A64 encodings the loader moves into caves (Arm ARM C4.1)
bool IsB(uint32_t w) { return (w & 0x7C000000) == 0x14000000; } // b, bl
bool IsBCond(uint32_t w) { return (w & 0xFF000010) == 0x54000000; }
bool IsCb(uint32_t w) { return (w & 0x7E000000) == 0x34000000; } // cbz, cbnz
bool IsTb(uint32_t w) { return (w & 0x7E000000) == 0x36000000; } // tbz, tbnz
bool IsBrReg(uint32_t w) { return (w & 0xFF9FFC1F) == 0xD61F0000; } // br, blr, ret
bool IsAdr(uint32_t w) { return (w & 0x1F000000) == 0x10000000; } // adr, adrp
bool IsLdrLiteral(uint32_t w) { return (w & 0x3B000000) == 0x18000000; }

bool IsBranch(uint32_t w)
{
    return IsB(w) || IsBCond(w) || IsCb(w) || IsTb(w) || IsBrReg(w);
}

bool IsConditional(uint32_t w)
{
    return (IsBCond(w) && (w & 0xE) != 0xE) || IsCb(w) || IsTb(w);
}

int64_t SignExtend(uint64_t value, int bits)
{
    const uint64_t sign = uint64_t{1} << (bits - 1);
    return static_cast<int64_t>((value ^ sign) - sign);
}

bool Fits(int64_t value, int bits)
{
    return value >= -(int64_t{1} << (bits - 1)) && value < (int64_t{1} << (bits - 1));
}

std::optional<uint32_t> EncodeB(uint64_t from, uint64_t to, bool link = false)
{
    const int64_t delta = static_cast<int64_t>(to - from);
    if ((delta & 3) != 0 || !Fits(delta >> 2, 26))
    {
        return std::nullopt;
    }
    return (link ? 0x94000000U : 0x14000000U) | (static_cast<uint32_t>(delta >> 2) & 0x3FFFFFF);
}

std::optional<uint32_t> EncodeAdrp(uint64_t pc, uint64_t target, uint32_t rd)
{
    const int64_t pages = static_cast<int64_t>((target >> 12) - (pc >> 12));
    if (!Fits(pages, 21))
    {
        return std::nullopt;
    }
    const uint32_t imm = static_cast<uint32_t>(pages) & 0x1FFFFF;
    return 0x90000000U | (imm & 3) << 29 | (imm >> 2) << 5 | rd;
}

// The branch target of a b, b.cond, cbz or tbz encoded at pc
uint64_t BranchTarget(uint32_t w, uint64_t pc)
{
    if (IsB(w))
    {
        return pc + (SignExtend(w & 0x3FFFFFF, 26) << 2);
    }
    if (IsTb(w))
    {
        return pc + (SignExtend((w >> 5) & 0x3FFF, 14) << 2);
    }
    return pc + (SignExtend((w >> 5) & 0x7FFFF, 19) << 2);
}

// The word encoded at from re-encoded at to, so it reaches the same target; nullopt when out of range. Words that
// are not PC-relative are returned as they are.
std::optional<uint32_t> Relocate(uint32_t w, uint64_t from, uint64_t to)
{
    if (IsB(w))
    {
        return EncodeB(to, BranchTarget(w, from), (w & 0x80000000) != 0);
    }
    if (IsAdr(w))
    {
        const int64_t imm = SignExtend(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3), 21);
        if ((w & 0x80000000) != 0)
        {
            return EncodeAdrp(to, (from & ~uint64_t{0xFFF}) + (imm << 12), w & 0x1F);
        }
        const int64_t delta = static_cast<int64_t>(from + imm - to);
        if (!Fits(delta, 21))
        {
            return std::nullopt;
        }
        const uint32_t value = static_cast<uint32_t>(delta) & 0x1FFFFF;
        return (w & 0x9F00001F) | (value & 3) << 29 | (value >> 2) << 5;
    }
    if (IsLdrLiteral(w))
    {
        const int64_t delta = static_cast<int64_t>(from + (SignExtend((w >> 5) & 0x7FFFF, 19) << 2) - to);
        if (!Fits(delta >> 2, 19))
        {
            return std::nullopt;
        }
        return (w & 0xFF00001F) | (static_cast<uint32_t>(delta >> 2) & 0x7FFFF) << 5;
    }
    return w;
}

// A conditional branch inverted to skip the next word (b.cond / cbz / tbz to pc + 8)
uint32_t InvertToSkipNext(uint32_t w)
{
    if (IsBCond(w))
    {
        return (w & 0xFF000010) | ((w & 0xF) ^ 1) | 2 << 5;
    }
    if (IsTb(w))
    {
        return ((w & 0xFFF8001F) ^ 0x01000000) | 2 << 5;
    }
    return ((w & 0xFF00001F) ^ 0x01000000) | 2 << 5;
}

// Why the host cannot run this word from a cave with native code execution, or nullptr. The native code patcher
// rewrites these in .text (nxemu-cpu nce/patcher.cpp); a cave is not in .text.
const char * NativeCodeProblem(uint32_t w)
{
    if ((w & 0xFF000000) == 0xD4000000)
    {
        return "an exception (svc, brk)";
    }
    if ((w & 0xFFC00000) == 0xD5000000)
    {
        // System instructions: hints and barriers run, and the NZCV, FPCR and FPSR registers (mrs and msr)
        const uint32_t reg = (w >> 5) & 0xFFFF;
        const bool move = (w & 0xFFD00000) == 0xD5100000; // mrs, msr (register)
        if ((w & 0xFFFFF01F) == 0xD503201F || (w & 0xFFFFF01F) == 0xD503301F ||
            (move && (reg == 0xDA10 || reg == 0xDA20 || reg == 0xDA21)))
        {
            return nullptr;
        }
        return "a system instruction or register";
    }
    if ((w & 0x3F800000) == 0x08000000)
    {
        return "an exclusive load or store";
    }
    return nullptr;
}

uint32_t AlignUp(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

// The cave bytes a site of these words takes: the words, the branch back and, for a conditional last word, the
// long branch it becomes
uint32_t CaveBytes(const std::vector<uint32_t> & words)
{
    return static_cast<uint32_t>(words.size() + 1 + (IsConditional(words.back()) ? 1 : 0)) * 4;
}

bool ParseHex(const std::string & text, uint32_t & out)
{
    if (text.empty() || text.size() > 8)
    {
        return false;
    }
    char * end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 16);
    if (end == nullptr || *end != '\0')
    {
        return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

std::string LowerHex(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

uint32_t ReadWord(std::span<const uint8_t> image, uint32_t offset)
{
    uint32_t word;
    std::memcpy(&word, image.data() + offset, sizeof(word));
    return word;
}

void WriteWord(std::span<uint8_t> image, uint32_t offset, uint32_t word)
{
    std::memcpy(image.data() + offset, &word, sizeof(word));
}

// On when the default is 1, unless the environment variable says otherwise
bool SetSwitch(const std::string & env, const std::string & default_value)
{
    if (const char * value = std::getenv(env.c_str()); value != nullptr && *value != '\0')
    {
        return std::strcmp(value, "0") != 0;
    }
    return default_value == "1";
}

} // namespace

PatchSets::PatchSets(const std::vector<VirtualFile> & files, const std::string & build_id, const std::string & module_name, Support support) :
    m_cave_alignment(std::max(support.cave_alignment, PageSize)),
    m_native_code(support.native_code)
{
    for (const VirtualFile & file : files)
    {
        Parse(file, build_id, module_name);
    }
    for (Set & set : m_sets)
    {
        if (!set.enabled || set.sites.empty())
        {
            continue;
        }
        bool reads_page = false;
        uint32_t cave_bytes = 0;
        std::string native;
        for (const Site & site : set.sites)
        {
            std::vector<uint32_t> values;
            for (const Word & word : site.words)
            {
                values.push_back(word.value);
                reads_page |= word.page_reg >= 0;
                const char * problem = word.page_reg < 0 && site.words.size() > 1 ? NativeCodeProblem(word.value) : nullptr;
                if (support.native_code && problem != nullptr && native.empty())
                {
                    native = fmt::format("the cave word {:08x} at module offset {:#x} ({}:{}) is {}, which native code execution cannot run "
                                         "from a cave",
                                         word.value, site.offset, set.file, site.line, problem);
                }
            }
            if (values.size() > 1)
            {
                cave_bytes += CaveBytes(values);
            }
        }
        const std::string missing = reads_page && !support.page_written ? "it reads the patch page, which this build does not write"
                                    : !native.empty()                   ? native
                                    : m_cave_bytes + uint64_t{cave_bytes} > MaxCaveBytes ? "the module's caves would pass the limit"
                                                                                         : "";
        if (!missing.empty())
        {
            m_problems.push_back(fmt::format("Patch set {} of {} left out: {}", set.name, set.file, missing));
            set.enabled = false;
            continue;
        }
        m_uses_page |= reads_page;
        m_cave_bytes += cave_bytes;
    }
}

std::string PatchSets::FileVersion(const VirtualFile & file)
{
    if (file == nullptr || file->GetSize() > MaxFileSize)
    {
        return {};
    }
    const std::vector<uint8_t> bytes = file->ReadAllBytes();
    std::istringstream text(std::string(bytes.begin(), bytes.end()));
    for (std::string line; std::getline(text, line);)
    {
        std::istringstream tokens(line.substr(0, line.find('#')));
        std::string directive, version;
        tokens >> directive >> version;
        if (directive == "@version")
        {
            return version;
        }
        if (directive == "set" || directive == "site")
        {
            break;
        }
    }
    return {};
}

std::string PatchSets::FileSetSwitches(const VirtualFile & file)
{
    if (file == nullptr || file->GetSize() > MaxFileSize)
    {
        return {};
    }
    const std::vector<uint8_t> bytes = file->ReadAllBytes();
    std::istringstream text(std::string(bytes.begin(), bytes.end()));
    std::string switches;
    for (std::string line; std::getline(text, line);)
    {
        std::istringstream tokens(line.substr(0, line.find('#')));
        std::string directive, name, env, default_value;
        tokens >> directive >> name >> env >> default_value;
        if (directive == "set" && !default_value.empty())
        {
            switches += name + (SetSwitch(env, default_value) ? "=1\n" : "=0\n");
        }
    }
    return switches;
}

void PatchSets::Parse(const VirtualFile & file, const std::string & build_id, const std::string & module_name)
{
    if (file == nullptr)
    {
        return;
    }
    std::string name = file->GetName();
    if (file->GetSize() > MaxFileSize)
    {
        m_problems.push_back(fmt::format("Patch sets {}: larger than {} bytes; file ignored", name, MaxFileSize));
        return;
    }
    const std::vector<uint8_t> bytes = file->ReadAllBytes();
    std::istringstream text(std::string(bytes.begin(), bytes.end()));
    const std::string want_id = LowerHex(build_id).append(64, '0').substr(0, 64);

    bool matches = false;
    std::unordered_map<std::string, size_t> sets;
    size_t site_lines = 0;
    int line_number = 0;
    for (std::string line; std::getline(text, line);)
    {
        ++line_number;
        line = line.substr(0, line.find('#'));
        std::istringstream tokens(line);
        std::vector<std::string> parts;
        for (std::string part; tokens >> part;)
        {
            parts.push_back(part);
        }
        if (parts.empty())
        {
            continue;
        }
        if (parts[0] == "@version" && parts.size() == 2)
        {
            // Names the sets that follow in logs
            name = fmt::format("{} ({})", file->GetName(), parts[1]);
            continue;
        }
        if (parts[0].starts_with("@nsobid-"))
        {
            matches = LowerHex(parts[0].substr(8)).append(64, '0').substr(0, 64) == want_id;
            if (!matches)
            {
                return;
            }
            continue;
        }
        if (!matches)
        {
            m_problems.push_back(fmt::format("Patch sets {}:{}: no @nsobid line before the sets; file ignored", name, line_number));
            return;
        }
        if (parts[0] == "set")
        {
            if (parts.size() != 4 || sets.contains(parts[1]))
            {
                m_problems.push_back(fmt::format("Patch sets {}:{}: expected \"set <name> <env> <default>\" with a new name", name, line_number));
                continue;
            }
            sets.emplace(parts[1], m_sets.size());
            m_sets.push_back({.name = parts[1], .file = name, .enabled = SetSwitch(parts[2], parts[3])});
        }
        else if (parts[0] == "site")
        {
            if (++site_lines > MaxSitesPerFile)
            {
                m_problems.push_back(fmt::format("Patch sets {}:{}: more than {} sites; the rest of the file is ignored", name, line_number, MaxSitesPerFile));
                return;
            }
            const auto set = parts.size() >= 6 ? sets.find(parts[1]) : sets.end();
            if (set == sets.end())
            {
                m_problems.push_back(fmt::format("Patch sets {}:{}: expected \"site <set> <module> <offset> <original> <word>...\" of a set named before", name, line_number));
                continue;
            }
            if (LowerHex(parts[2]) != LowerHex(module_name))
            {
                continue;
            }
            Site site{.set = set->second, .line = line_number};
            bool valid = ParseHex(parts[3], site.offset) && (site.offset & 3) == 0 && ParseHex(parts[4], site.original) &&
                         parts.size() - 5 <= MaxWordsPerSite;
            for (size_t i = 5; valid && i < parts.size(); i++)
            {
                Word word;
                if (parts[i].starts_with("@page:x"))
                {
                    word.page_reg = std::atoi(parts[i].c_str() + 7);
                    valid = word.page_reg >= 0 && word.page_reg <= 30 && parts[i].size() <= 9;
                    word.value = 0x90000000U | static_cast<uint32_t>(word.page_reg);
                }
                else
                {
                    valid = ParseHex(parts[i], word.value);
                }
                // Only the last word may leave the cave
                valid = valid && (i + 1 == parts.size() || !IsBranch(word.value));
                site.words.push_back(word);
            }
            if (!valid)
            {
                m_problems.push_back(fmt::format("Patch sets {}:{}: invalid site, set {} left out", name, line_number, parts[1]));
                m_sets[set->second].enabled = false;
                continue;
            }
            m_sets[set->second].sites.push_back(std::move(site));
        }
        else if (parts[0] != "spin")
        {
            m_problems.push_back(fmt::format("Patch sets {}:{}: unknown line \"{}\" ignored", name, line_number, parts[0]));
        }
    }
}

bool PatchSets::Empty() const
{
    return std::none_of(m_sets.begin(), m_sets.end(), [](const Set & set) { return set.enabled && !set.sites.empty(); });
}

uint32_t PatchSets::ReservedSize() const
{
    if (!m_uses_page && m_cave_bytes == 0)
    {
        return 0;
    }
    return CaveOffset() + CaveSize();
}

PatchSets::Layout PatchSets::Reserve(uint32_t image_end) const
{
    const uint32_t reserved = ReservedSize();
    if (reserved == 0)
    {
        return {image_end, 0, 0};
    }
    // The patch page starts on the cave alignment, so the caves after it do. With native code execution the caves
    // stay out of the data segment, so the guest never maps them, as with the padding after the image: once they
    // are read-write data, the kernel refuses to hide them.
    const uint32_t page_offset = AlignUp(image_end, m_cave_alignment);
    const uint32_t padding = page_offset - image_end;
    return {page_offset, padding + (m_native_code ? CaveOffset() : reserved), padding + reserved};
}

uint32_t PatchSets::CaveOffset() const
{
    return AlignUp(PageSize, m_cave_alignment);
}

uint32_t PatchSets::CaveSize() const
{
    return AlignUp(m_cave_bytes, m_cave_alignment);
}

std::optional<std::vector<uint32_t>> PatchSets::CaveWords(const Site & site, uint32_t cave, uint32_t page_offset)
{
    // The words are encoded as if each were at the site; each is encoded again for where it lands
    std::vector<uint32_t> out;
    uint32_t pc = cave;
    for (size_t i = 0; i < site.words.size(); i++)
    {
        const Word & word = site.words[i];
        const bool last = i + 1 == site.words.size();
        std::optional<uint32_t> value;
        if (word.page_reg >= 0)
        {
            value = EncodeAdrp(pc, page_offset, word.page_reg);
        }
        else if (last && IsBCond(word.value) && !IsConditional(word.value))
        {
            // b.al and b.nv are always taken
            value = EncodeB(pc, BranchTarget(word.value, site.offset));
        }
        else if (last && IsConditional(word.value))
        {
            // Out of a cave a conditional branch may not reach its target: skip a long branch to it instead
            out.push_back(InvertToSkipNext(word.value));
            pc += 4;
            value = EncodeB(pc, BranchTarget(word.value, site.offset));
        }
        else
        {
            value = Relocate(word.value, site.offset, pc);
        }
        if (!value)
        {
            return std::nullopt;
        }
        out.push_back(*value);
        pc += 4;
    }
    const std::optional<uint32_t> back = EncodeB(pc, site.offset + 4);
    if (!back)
    {
        return std::nullopt;
    }
    out.push_back(*back);
    return out;
}

void PatchSets::LogProblems() const
{
    for (const std::string & problem : m_problems)
    {
        LOG_WARNING(Loader, "{}", problem);
    }
}

bool PatchSets::Apply(std::span<uint8_t> image, uint32_t page_offset, bool report)
{
    const uint32_t reserved = ReservedSize();
    if (page_offset + static_cast<uint64_t>(reserved) > image.size() || (reserved != 0 && page_offset % m_cave_alignment != 0))
    {
        return false;
    }
    const uint32_t cave_end = page_offset + reserved;
    uint32_t cave = page_offset + CaveOffset();
    std::unordered_map<uint32_t, const Set *> claimed;
    bool page_used = false;

    for (const Set & set : m_sets)
    {
        if (set.sites.empty())
        {
            // Its sites are for other modules
            continue;
        }
        if (!set.enabled)
        {
            if (report)
            {
                LOG_INFO(Loader, "Patch set {} of {} is off", set.name, set.file);
            }
            continue;
        }

        // Every word is checked and encoded before any is written, so a set applies whole or not at all
        std::vector<std::pair<uint32_t, uint32_t>> writes;
        std::vector<uint32_t> sites;
        std::string problem;
        uint32_t set_cave = cave;
        size_t in_caves = 0;
        bool set_page = false;
        for (const Site & site : set.sites)
        {
            const std::string where = fmt::format("module offset {:#x} ({}:{})", site.offset, set.file, site.line);
            if (const auto owner = claimed.find(site.offset); owner != claimed.end())
            {
                // Half a fix is worse than none: the earlier set keeps the site and this one stays out
                problem = fmt::format("{} is patched by set {} of {}", where, owner->second->name, owner->second->file);
                break;
            }
            if (site.offset + uint64_t{4} > page_offset || std::find(sites.begin(), sites.end(), site.offset) != sites.end())
            {
                problem = where + " is outside the module or listed twice";
                break;
            }
            if (const uint32_t word = ReadWord(image, site.offset); word != site.original)
            {
                problem = fmt::format("{} holds {:08x}, not {:08x}", where, word, site.original);
                break;
            }
            for (const Word & word : site.words)
            {
                set_page |= word.page_reg >= 0;
            }
            sites.push_back(site.offset);
            if (site.words.size() == 1)
            {
                // In place: only an adrp to the patch page needs a reach check
                const Word & word = site.words[0];
                const std::optional<uint32_t> value =
                    word.page_reg >= 0 ? EncodeAdrp(site.offset, page_offset, word.page_reg) : std::optional<uint32_t>(word.value);
                if (!value)
                {
                    problem = where + ": the patch page is out of adrp range";
                    break;
                }
                writes.emplace_back(site.offset, *value);
                continue;
            }

            // The words run in a cave, then branch back after the site
            const std::optional<uint32_t> branch = EncodeB(site.offset, set_cave);
            const std::optional<std::vector<uint32_t>> words = CaveWords(site, set_cave, page_offset);
            if (!branch || !words || set_cave + words->size() * 4 > cave_end)
            {
                problem = where + (!branch ? ": the caves are out of branch range" : ": a word cannot run from a cave, its target is out of range");
                break;
            }
            writes.emplace_back(site.offset, *branch);
            for (const uint32_t word : *words)
            {
                writes.emplace_back(set_cave, word);
                set_cave += 4;
            }
            ++in_caves;
        }
        if (!problem.empty())
        {
            if (report)
            {
                LOG_WARNING(Loader, "Patch set {} of {} left out: {}", set.name, set.file, problem);
            }
            continue;
        }

        for (const auto & [offset, word] : writes)
        {
            WriteWord(image, offset, word);
        }
        for (const uint32_t offset : sites)
        {
            claimed.emplace(offset, &set);
        }
        cave = set_cave;
        page_used |= set_page;
        if (report)
        {
            LOG_INFO(Loader, "Patch set {} of {}: {} sites patched ({} in caves)", set.name, set.file, sites.size(), in_caves);
        }
    }
    return page_used;
}

} // namespace FileSys
