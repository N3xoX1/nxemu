// SPDX-License-Identifier: GPL-2.0-or-later
#include "bfres_layouts.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include "yuzu_video_core/shader_profile_compact.h"
#include "yuzu_video_core/shader_profile_harvest.h"

namespace BfresLayouts {
namespace {
// Bounds-checked little-endian reads of one file.
struct Blob {
    std::string_view d;
    template <class T>
    T At(u64 o) const {
        if (o > d.size() || d.size() - o < sizeof(T)) throw std::runtime_error("read outside the file");
        T v;
        std::memcpy(&v, d.data() + o, sizeof(T));
        return v;
    }
    u64 Q(u64 o) const { return At<u64>(o); }
    u32 U32(u64 o) const { return At<u32>(o); }
    u16 U16(u64 o) const { return At<u16>(o); }
    u8 U8(u64 o) const { return At<u8>(o); }
    std::string S(u64 o) const {
        const u16 n = U16(o);
        if (o + 2 + n > d.size()) throw std::runtime_error("string outside the file");
        return std::string(d.substr(o + 2, n));
    }
    // Names of a resource dictionary, in entry order (the root node first is skipped).
    std::vector<std::string> Keys(u64 o) const {
        std::vector<std::string> out;
        if (!o) return out;
        const u32 n = U32(o + 4);
        if (n > 65536) throw std::runtime_error("dictionary too large");
        for (u32 i = 0; i < n; ++i) out.push_back(S(Q(o + 8 + (u64{i} + 1) * 0x10 + 8)));
        return out;
    }
};
constexpr u32 NvnCodeMagic = 0x12345678;

// A program's key as the harvest names it: from its program header through its self branch.
std::string ProgramKeyOf(std::string_view code) {
    if (code.size() < 0x80) return {};
    u32 magic;
    std::memcpy(&magic, code.data(), 4);
    if (magic != NvnCodeMagic) return {};
    code.remove_prefix(0x30);
    const auto size = VideoCommon::ShaderProfile::Harvest::ProgramSize(code);
    return size ? VideoCommon::ShaderProfile::ProgramKey(code.substr(0, size)) : std::string{};
}

// BFRES attribute format (size << 8 | type) as the guest attribute word fields.
bool GuestAttribute(u32 format, u32 offset, u32 buffer, u32& word) {
    u32 type{}, size{};
    switch (format & 0xff) {
    case 1: type = 2; break; // unorm
    case 2: type = 1; break; // snorm
    case 3: type = 4; break; // uint
    case 4: type = 3; break; // sint
    case 5: type = 7; break; // float
    default: return false;
    }
    switch (format >> 8) {
    case 0x02: size = 0x1d; break; // 8
    case 0x09: size = 0x18; break; // 8_8
    case 0x0b: size = 0x0a; break; // 8_8_8_8
    case 0x0e: size = 0x30; break; // 10_10_10_2
    case 0x12: size = 0x0f; break; // 16_16
    case 0x15: size = 0x03; break; // 16_16_16_16
    case 0x17: size = 0x04; break; // 32_32
    case 0x18: size = 0x02; break; // 32_32_32
    case 0x19: size = 0x01; break; // 32_32_32_32
    default: return false;
    }
    if (buffer >= 32 || offset >= 0x4000) return false;
    word = 1 | buffer << 1 | offset << 6 | type << 20 | size << 23;
    return true;
}
} // namespace

std::vector<u32> Apply(std::vector<u32> words, const Layout& layout) {
    for (size_t w = 42; w < 74; ++w) words[w] = 0;
    for (const auto& [location, value] : layout.attributes) words[42 + location] = value;
    words[3] = layout.streams;
    for (const auto& [buffer, stride] : layout.strides) words[106 + buffer] = (words[106 + buffer] & 0xffff0000) | stride;
    return words;
}

void Collector::ReadArchive(std::string_view data, size_t file) {
    const Blob b{data};
    Archive archive;
    const auto name = b.S(b.Q(0x38));
    const auto names = b.Keys(b.Q(0x50));
    for (size_t i = 0; i < names.size(); ++i) {
        const u64 m = b.Q(0x48) + i * 0xc0;
        Model model;
        const auto options = [&](u64 array, u64 dictionary) {
            const auto keys = b.Keys(dictionary);
            for (size_t k = 0; k < keys.size(); ++k) {
                const u64 o = array + k * 0x28;
                model.options.push_back({keys[k], b.Keys(b.Q(o + 8)), b.U8(o + 0x19), b.U8(o + 0x1e), b.U8(o + 0x1f), b.U32(o + 0x20)});
            }
        };
        options(b.Q(m + 0x08), b.Q(m + 0x10)); // static options only: a shape's material sets them
        const auto attribute_names = b.Keys(b.Q(m + 0x30));
        for (size_t a = 0; a < attribute_names.size(); ++a)
            model.attributes[attribute_names[a]] = {b.U8(b.Q(m + 0x28) + a * 2), b.U8(b.Q(m + 0x28) + a * 2 + 1)};
        const u64 programs = b.Q(m + 0x60), keys = b.Q(m + 0x68), bnsh = b.Q(m + 0x80);
        const u32 count = b.U16(m + 0xac);
        model.static_words = b.U8(m + 0xb0);
        const size_t width = model.static_words + b.U8(m + 0xb1);
        for (u32 p = 0; p < count; ++p) {
            const u64 o = programs + u64{p} * 0x30;
            Program program;
            for (size_t w = 0; w < model.static_words; ++w) program.static_key.push_back(b.U32(keys + (p * width + w) * 4));
            program.attribute_mask = b.U32(o + 0x20);
            // The variation's programs, through the BNSH the model holds.
            const u64 variation = b.Q(o + 0x10) - bnsh;
            const u64 entry = b.Q(bnsh + variation + 0x10);
            for (const auto& [slot, out] : {std::pair{0x08, &program.vertex}, std::pair{0x28, &program.fragment}}) {
                const u64 record = b.Q(bnsh + entry + slot);
                if (!record) continue;
                const u64 code = b.Q(bnsh + record + 0x10), size = b.U32(bnsh + record + 0x18);
                if (bnsh + code > data.size() || data.size() - (bnsh + code) < size) throw std::runtime_error("program outside the archive");
                *out = ProgramKeyOf(data.substr(bnsh + code, size));
            }
            if (!program.vertex.empty()) model.programs.push_back(std::move(program));
        }
        archive.models.emplace(names[i], std::move(model));
    }
    const size_t index = archives.size();
    archives.push_back(std::move(archive));
    local.emplace(std::pair{file, name}, index);
    global.emplace(name, index);
}

void Collector::Add(std::string_view data, const std::string&) {
    if (data.size() < 0x100 || data.substr(0, 8) != "FRES    ") return;
    const size_t file = files++;
    ++stats["model files"];
    try {
        const Blob b{data};
        // Shader archives the file embeds (they may serve other files too).
        const u64 external = b.Q(0x98);
        const auto external_names = b.Keys(b.Q(0xa0));
        for (size_t i = 0; i < external_names.size(); ++i) {
            const u64 o = b.Q(external + i * 0x10), size = b.U32(external + i * 0x10 + 8);
            if (o > data.size() || data.size() - o < size || data.substr(o, 4) != "FSHA") continue;
            try {
                ReadArchive(data.substr(o, size), file);
            } catch (const std::exception&) {
                ++stats["unreadable shader archive"];
            }
        }
        const auto model_names = b.Keys(b.Q(0x30));
        for (size_t mi = 0; mi < model_names.size(); ++mi) {
            const u64 m = b.Q(0x28) + mi * 0x78;
            if (b.U32(m) != 0x4c444d46) { ++stats["unreadable model"]; continue; } // FMDL
            const u32 vertex_count = b.U16(m + 0x68), shape_count = b.U16(m + 0x6a), material_count = b.U16(m + 0x6c);
            std::vector<std::pair<std::map<std::string, std::array<u32, 3>>, std::vector<u32>>> buffers;
            for (u32 vi = 0; vi < vertex_count; ++vi) {
                const u64 v = b.Q(m + 0x28) + u64{vi} * 0x60;
                const u32 attribute_count = b.U8(v + 0x54), buffer_count = b.U8(v + 0x55);
                auto& [attributes, strides] = buffers.emplace_back();
                for (u32 ai = 0; ai < attribute_count; ++ai) {
                    const u64 a = b.Q(v + 0x10) + u64{ai} * 0x10;
                    attributes[b.S(b.Q(a))] = {b.U16(a + 8), b.U16(a + 0xc), b.U16(a + 0xe)};
                }
                for (u32 i = 0; i < buffer_count; ++i) strides.push_back(b.U32(b.Q(v + 0x40) + u64{i} * 0x10));
            }
            struct Material { std::string archive, model; std::map<std::string, std::string> assign, options; bool valid{}; };
            std::vector<Material> materials;
            for (u32 ti = 0; ti < material_count; ++ti) {
                const u64 t = b.Q(m + 0x40) + u64{ti} * 0xb8;
                const u64 sa = b.Q(t + 0x28);
                auto& material = materials.emplace_back();
                if (!sa) continue;
                material.valid = true;
                material.archive = b.S(b.Q(sa));
                material.model = b.S(b.Q(sa + 8));
                const auto assign_keys = b.Keys(b.Q(sa + 0x18)), option_keys = b.Keys(b.Q(sa + 0x38));
                const u32 assign_count = b.U8(sa + 0x44), option_count = b.U16(sa + 0x46);
                for (u32 i = 0; i < assign_count && i < assign_keys.size(); ++i) material.assign[assign_keys[i]] = b.S(b.Q(b.Q(sa + 0x10) + u64{i} * 8));
                for (u32 i = 0; i < option_count && i < option_keys.size(); ++i) material.options[option_keys[i]] = b.S(b.Q(b.Q(sa + 0x30) + u64{i} * 8));
            }
            for (u32 si = 0; si < shape_count; ++si) {
                const u64 s = b.Q(m + 0x30) + u64{si} * 0x70;
                const u32 material = b.U16(s + 0x5e), buffer = b.U16(s + 0x62);
                ++stats["shapes"];
                if (material >= materials.size() || !materials[material].valid || buffer >= buffers.size()) { ++stats["shape without material"]; continue; }
                const auto& mat = materials[material];
                shapes.push_back({file, mat.archive, mat.model, mat.assign, mat.options, buffers[buffer].first, buffers[buffer].second});
            }
        }
    } catch (const std::exception&) {
        ++stats["unreadable model file"];
    }
}

std::map<std::vector<std::string>, std::set<Layout>> Collector::Finish() {
    std::map<std::vector<std::string>, std::set<Layout>> out;
    for (const auto& shape : shapes) {
        auto found = local.find({shape.file, shape.archive});
        size_t index;
        if (found != local.end()) index = found->second;
        else if (const auto g = global.find(shape.archive); g != global.end()) index = g->second;
        else { ++stats["shape whose shader archive is missing"]; continue; }
        const auto m = archives[index].models.find(shape.model);
        if (m == archives[index].models.end()) { ++stats["shape whose shader model is missing"]; continue; }
        const auto& model = m->second;
        // The static key the material's options select.
        std::vector<u32> key(model.static_words);
        for (const auto& option : model.options) {
            if (option.word >= key.size()) continue;
            u32 choice = option.default_choice;
            if (const auto value = shape.options.find(option.name); value != shape.options.end())
                if (const auto at = std::find(option.choices.begin(), option.choices.end(), value->second); at != option.choices.end())
                    choice = static_cast<u32>(at - option.choices.begin());
            key[option.word] |= (choice << option.shift) & option.mask;
        }
        bool matched{};
        for (const auto& program : model.programs) {
            if (program.static_key != key) continue;
            matched = true;
            Layout layout;
            bool ok = true;
            for (const auto& [name, place] : model.attributes) {
                const auto [index_bit, location] = place;
                if (index_bit >= 32 || !((program.attribute_mask >> index_bit) & 1)) continue;
                const auto source_name = shape.assign.contains(name) ? shape.assign.at(name) : name;
                const auto source = shape.attributes.find(source_name);
                if (source == shape.attributes.end()) { ++stats["attribute without data"]; continue; }
                const auto [format, offset, buffer] = source->second;
                u32 word;
                if (location >= 32 || !GuestAttribute(format, offset, buffer, word)) { ok = false; break; }
                layout.attributes[location] = word;
                if (buffer < shape.strides.size()) layout.strides[buffer] = shape.strides[buffer];
                layout.streams |= 1U << buffer;
            }
            if (!ok) { ++stats["attribute format unknown"]; continue; }
            std::vector<std::string> keys{program.vertex};
            if (!program.fragment.empty()) keys.push_back(program.fragment);
            std::sort(keys.begin(), keys.end());
            out[std::move(keys)].insert(std::move(layout));
        }
        ++stats[matched ? "shapes matched" : "shape with no program for its options"];
    }
    return out;
}
} // namespace BfresLayouts
