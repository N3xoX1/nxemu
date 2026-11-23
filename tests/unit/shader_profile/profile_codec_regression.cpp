// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile_codec.h"
#include <iostream>
#include <stdexcept>
#include <string>
using namespace VideoCommon::ShaderProfile;
namespace {
void Require(bool value, const char* what) {
    if (!value) throw std::runtime_error(std::string("profile codec regression: ") + what);
}
bool Rejected(const std::string& value, ContainerInfo* info = nullptr) {
    try { Unpack(value, info); } catch (const std::exception&) { return true; }
    return false;
}
void Put(std::string& bytes, size_t offset, uint64_t value, size_t size) {
    for (size_t i = 0; i < size; ++i) bytes[offset + i] = static_cast<char>(value >> (8 * i));
}
// The same file with these extension bytes after the header.
std::string WithExtensions(const std::string& packed, const std::string& extensions) {
    auto out = packed.substr(0, ProfileHeaderSize) + extensions + packed.substr(ProfileHeaderSize);
    Put(out, 44, extensions.size(), 4);
    return out;
}
std::string Extension(uint16_t tag, const std::string& bytes) {
    std::string out(4, '\0');
    Put(out, 0, tag, 2);
    Put(out, 2, bytes.size(), 2);
    return out + bytes;
}
} // namespace
int main() try {
    const std::string json = "{\"metadata\":\"" + std::string(4096, 'a') + "\"}";
    const auto packed = Pack(json);
    Require(packed.substr(0, 8) == ProfileMagic, "magic");
    Require(Unpack(packed) == json && packed.size() < json.size(), "round trip");
    Require(Unpack(json) == json, "raw payload for tools");
    Require(Pack(json) == packed, "deterministic encoding");
    ContainerInfo info;
    Unpack(packed, &info);
    Require(info.major == ProfileMajor && info.minor == ProfileMinor && info.platform == SwitchPlatform &&
                info.schema == ProfileSchema && info.features == 0 && info.extensions.empty(),
            "header fields");

    for (size_t length : {size_t{8}, ProfileHeaderSize - 1, ProfileHeaderSize, packed.size() - 1})
        Require(Rejected(packed.substr(0, length)), "truncated file accepted");
    // Major, payload size, frame size, platform, schema, features, extension size, the frame.
    for (size_t offset : {size_t{8}, size_t{16}, size_t{24}, size_t{32}, size_t{36}, size_t{40}, size_t{44},
                          ProfileHeaderSize + 4, packed.size() - 1}) {
        auto bad = packed;
        bad[offset] ^= 1;
        Require(Rejected(bad), "damaged header or frame accepted");
    }
    {
        // A newer minor version reads the same: it only adds what older readers may skip.
        auto minor = packed;
        Put(minor, 12, ProfileMinor + 1, 4);
        Require(Unpack(minor) == json, "newer minor version refused");
    }
    {
        // Data after the frame, even a second valid frame, is refused.
        auto extra = packed + packed.substr(ProfileHeaderSize);
        Put(extra, 24, extra.size() - ProfileHeaderSize, 8);
        Require(Rejected(extra), "trailing frame accepted");
    }
    {
        // Switch 2 files: refused where only Switch 1 runs, read when the caller asks for the platform.
        const auto switch2 = Pack(json, 3, Switch2Platform);
        Require(Rejected(switch2), "Switch 2 file accepted without a platform check");
        ContainerInfo s2;
        Require(Unpack(switch2, &s2) == json && s2.platform == Switch2Platform, "Switch 2 file not read with info");
        for (const uint32_t platform : {0U, 3U}) {
            auto other = packed;
            Put(other, 32, platform, 4);
            Require(Rejected(other, &s2), "unknown platform accepted");
        }
    }
    {
        // Extensions: optional ones are kept and skipped, required ones (bit 15) refuse the file.
        const auto optional = Extension(1, "abc") + Extension(7, "");
        ContainerInfo with;
        Require(Unpack(WithExtensions(packed, optional), &with) == json && with.extensions == optional,
                "optional extensions not skipped");
        Require(Rejected(WithExtensions(packed, Extension(0x8001, "x"))), "required extension accepted");
        Require(Rejected(WithExtensions(packed, Extension(7, "") + Extension(1, ""))), "unordered extensions accepted");
        Require(Rejected(WithExtensions(packed, Extension(0, ""))), "extension tag 0 accepted");
        Require(Rejected(WithExtensions(packed, Extension(1, "abc").substr(0, 5))), "truncated extension accepted");
        Require(Rejected(WithExtensions(packed, Extension(1, std::string(MaxProfileExtensionBytes, 'x')))),
                "oversized extensions accepted");
    }
    {
        // Required features and other schemas are refused whole.
        auto feature = packed;
        Put(feature, 40, 1, 4);
        Require(Rejected(feature), "unknown required feature accepted");
        auto schema = packed;
        Put(schema, 36, ProfileSchema + 1, 4);
        Require(Rejected(schema), "unknown schema accepted");
    }
    std::cout << "profile container round trip, header fields, platforms, extensions, bounds and checksum checks passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
