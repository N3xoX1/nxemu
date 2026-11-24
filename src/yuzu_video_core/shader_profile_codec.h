// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
namespace VideoCommon::ShaderProfile {
// The NXShader container (docs/nxshader-format.md): a fixed 48-byte header, up
// to 4 KiB of extensions, then one checksummed zstd frame holding the payload.
inline constexpr std::string_view ProfileMagic = "NXSHADR1";
inline constexpr uint32_t ProfileMajor = 1, ProfileMinor = 0, ProfileSchema = 1;
inline constexpr size_t ProfileHeaderSize = 48, MaxProfileExtensionBytes = 4096;
// Platforms: 1 Switch, 2 Switch 2.
inline constexpr uint32_t SwitchPlatform = 1, Switch2Platform = 2;
struct ContainerInfo {
    uint32_t major{}, minor{}, platform{}, schema{}, features{};
    std::string extensions;
};
std::string Pack(std::string_view payload, int level = 3, uint32_t platform = SwitchPlatform);
// Also reads raw payload bytes (major 0) for tools.
// Without info, a file for another platform than the Switch is refused.
std::string Unpack(std::string_view bytes, ContainerInfo* info = nullptr);
// One record of an append-only local file. PackFrame returns an empty string when
// compression would not save space; the caller then stores the bytes as they are.
// UnpackFrame is false unless bytes are exactly one frame of at most max_size bytes.
std::string PackFrame(std::string_view raw, int level = 1);
bool UnpackFrame(std::string_view bytes, size_t max_size, std::string& out);

// Game resources compressed with zstd (TotK's .zs files), often with a dictionary
// the game ships (Pack/ZsDic.pack.zs). A frame names its dictionary by ID.
bool IsZstd(std::string_view bytes);
class ZstdDictionary {
public:
    // Throws unless the bytes are a zstd dictionary with an ID.
    explicit ZstdDictionary(std::string_view bytes);
    ~ZstdDictionary();
    ZstdDictionary(const ZstdDictionary&) = delete;
    ZstdDictionary& operator=(const ZstdDictionary&) = delete;
    [[nodiscard]] unsigned Id() const noexcept { return id; }

private:
    friend std::string DecodeZstd(std::string_view, size_t, const ZstdDictionary*);
    struct State;
    std::unique_ptr<State> state;
    unsigned id{};
};
// The dictionary ID a frame names (0 for none) and the size it decodes to; throws
// unless the bytes are exactly one frame of a known size of at most max_size bytes.
unsigned ZstdFrameDictionary(std::string_view frame);
size_t ZstdFrameSize(std::string_view frame, size_t max_size);
// Decodes exactly one frame of `size` bytes. The dictionary must be the one the frame names.
std::string DecodeZstd(std::string_view frame, size_t size, const ZstdDictionary* dictionary = nullptr);
} // namespace VideoCommon::ShaderProfile
