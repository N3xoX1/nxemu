// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Application-control reply flags and icon conversion are adapted from Eden.

#include "core/hle/service/ns/application_control_data.h"

#include <algorithm>
#include <memory>
#include <vector>
#include "yuzu_common/stb.h"

namespace Service::NS {
namespace {

constexpr int IconDimension = 174;
constexpr int MaxDecodedDimension = 1024;

struct JpegOutput {
    std::vector<uint8_t> bytes;
    bool overflow{};
};

void WriteJpeg(void* context, void* bytes, int count) {
    auto& output = *static_cast<JpegOutput*>(context);
    if (count < 0 || static_cast<size_t>(count) > ApplicationControlData::MaxIconSize - output.bytes.size()) {
        output.overflow = true;
        return;
    }
    if (!output.overflow && count != 0) {
        const auto* begin = static_cast<const uint8_t*>(bytes);
        output.bytes.insert(output.bytes.end(), begin, begin + count);
    }
}

bool ResizeIcon(std::span<const uint8_t> input, std::vector<uint8_t>& output) {
    // Restrict the decode before allocating pixels. Icons are small JPEG images.
    constexpr uint8_t end_marker[]{0xff, 0xd9};
    if (input.size() < 4 || input[0] != 0xff || input[1] != 0xd8 ||
        std::search(input.begin() + 2, input.end(), std::begin(end_marker), std::end(end_marker)) == input.end()) {
        return false;
    }
    int width{}, height{}, channels{};
    const auto size = static_cast<int>(input.size());
    if (!stbi_info_from_memory(input.data(), size, &width, &height, &channels) ||
        width <= 0 || height <= 0 || width > MaxDecodedDimension || height > MaxDecodedDimension) {
        return false;
    }
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels{
        stbi_load_from_memory(input.data(), size, &width, &height, &channels, STBI_rgb),
        stbi_image_free};
    if (!pixels) {
        return false;
    }
    if (width == IconDimension && height == IconDimension) {
        output.assign(input.begin(), input.end());
        return true;
    }
    std::vector<uint8_t> resized(IconDimension * IconDimension * STBI_rgb);
    // RGB has no alpha channel. The final argument is a flags bitmask, not a filter enum.
    if (!stbir_resize_uint8_srgb(pixels.get(), width, height, 0, resized.data(),
                                IconDimension, IconDimension, 0, STBI_rgb,
                                STBIR_ALPHA_CHANNEL_NONE, 0)) {
        return false;
    }
    JpegOutput encoded;
    if (!stbi_write_jpg_to_func(WriteJpeg, &encoded, IconDimension, IconDimension,
                                STBI_rgb, resized.data(), 90) || encoded.overflow || encoded.bytes.empty()) {
        return false;
    }
    output = std::move(encoded.bytes);
    return true;
}

} // namespace

LoaderResultStatus WriteApplicationControlData(std::span<const uint8_t> data,
    std::span<uint8_t> output, bool modern, bool resize_icon, ControlDataResponse& response) {
    response = {};
    if (data.size() < ApplicationControlData::NacpSize || data.size() > ApplicationControlData::MaxSize) {
        return LoaderResultStatus::ErrorNoControl;
    }
    auto icon = data.subspan(ApplicationControlData::NacpSize);
    // The modern success flags follow Eden's existing compatibility contract. Do not
    // return them when the icon is absent or its requested conversion failed.
    if (modern && icon.empty()) {
        return LoaderResultStatus::ErrorNoIcon;
    }
    std::vector<uint8_t> resized;
    if (resize_icon) {
        if (!ResizeIcon(icon, resized)) {
            return LoaderResultStatus::ErrorNoIcon;
        }
        icon = resized;
    }
    const auto size = ApplicationControlData::NacpSize + icon.size();
    if (output.size() < size) {
        return LoaderResultStatus::ErrorBufferTooSmall;
    }
    // Validate everything before touching the guest's output. Never truncate JPEG data.
    std::memcpy(output.data(), data.data(), ApplicationControlData::NacpSize);
    if (!icon.empty()) {
        std::memcpy(output.data() + ApplicationControlData::NacpSize, icon.data(), icon.size());
    }
    std::fill(output.begin() + size, output.end(), 0);
    response.actual_size = static_cast<uint32_t>(size);
    if (modern) {
        response.flags_a = 0x10001;
        response.flags_b = resize_icon ? 0x10001 : 0;
    }
    return LoaderResultStatus::Success;
}

} // namespace Service::NS
