// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <compare>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include "yuzu_common/common_types.h"

// What a game's model files (BFRES, Switch 1 layout, with their BFSHA shader archives) say about the
// vertex input of the pipelines it draws: each shape's material picks programs through the archive's
// option keys, its vertex buffer gives each attribute's format, offset, buffer and stride. Without
// dynamic vertex input, that layout is part of the pipeline, so it is the guess that matters most.
namespace BfresLayouts {
// Guest words of the vertex input (shader_profile_guest_state.h): attributes by location (words
// 42 + location), strides by buffer (low half of words 106 + buffer), enabled streams (word 3).
struct Layout {
    std::map<u32, u32> attributes, strides;
    u32 streams{};
    auto operator<=>(const Layout&) const = default;
};
std::vector<u32> Apply(std::vector<u32> words, const Layout& layout);

class Collector {
public:
    // A decoded game resource: read when it is a BFRES.
    void Add(std::string_view data, const std::string& origin);
    // Layouts per program set (program keys, sorted).
    std::map<std::vector<std::string>, std::set<Layout>> Finish();
    std::map<std::string, size_t> stats;

private:
    struct Program {
        std::vector<u32> static_key;
        u32 attribute_mask{};
        std::string vertex, fragment;
    };
    struct Option {
        std::string name;
        std::vector<std::string> choices;
        u32 default_choice{}, word{}, shift{}, mask{};
    };
    struct Model {
        std::vector<Option> options;
        size_t static_words{};
        std::map<std::string, std::pair<u32, u32>> attributes; // name -> index, location
        std::vector<Program> programs;
    };
    struct Archive {
        std::map<std::string, Model> models;
    };
    struct Shape {
        size_t file;
        std::string archive, model;
        std::map<std::string, std::string> assign, options;
        std::map<std::string, std::array<u32, 3>> attributes; // name -> format, offset, buffer
        std::vector<u32> strides;
    };
    void ReadArchive(std::string_view data, size_t file);
    std::vector<Archive> archives;
    std::map<std::pair<size_t, std::string>, size_t> local;
    std::map<std::string, size_t> global;
    std::vector<Shape> shapes;
    size_t files{};
};
} // namespace BfresLayouts
