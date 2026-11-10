// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <string>
#include <string_view>
namespace VideoCommon::ShaderProfile {
// Bounded compression envelope; the runtime separately requires NXSHBIN1 or NXSHBIN2.
// Unpack also accepts raw bytes for offline diagnostic tools.
std::string Pack(std::string_view json, int level = 3);
std::string Unpack(std::string_view bytes);
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
