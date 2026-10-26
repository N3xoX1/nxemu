// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <string_view>

namespace VideoCommon::ShaderProfile
{
inline constexpr size_t MaxRevision = 32, MaxNote = 1024;
// Text shown to people (label, revision, note): well-formed UTF-8 without
// control characters, so a shared profile cannot break the layout around it.
inline bool ValidProfileText(std::string_view text, size_t limit) {
    if (text.size() > limit) return false;
    for (size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f) return false;
            ++i;
            continue;
        }
        const size_t length = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 ? 2 : 0;
        if (!length || c > 0xf4 || length > text.size() - i) return false;
        char32_t code = c & (0x7f >> length);
        for (size_t n = 1; n < length; ++n) {
            const auto next = static_cast<unsigned char>(text[i + n]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if ((length == 3 && code < 0x800) || (length == 4 && (code < 0x10000 || code > 0x10ffff)) ||
            (code >= 0xd800 && code <= 0xdfff) || (code >= 0x80 && code < 0xa0))
            return false;
        i += length;
    }
    return true;
}
} // namespace VideoCommon::ShaderProfile
