// SPDX-FileCopyrightText: Copyright 2026 nxemu contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <span>
#include <unordered_set>

#include "yuzu_video_core/texture_cache/types.h"

namespace VideoCommon {

// Small invalidations avoid allocation. GC batches use hashed membership so
// scanning the cache does not multiply its size by the number of removed views.
class ImageViewInvalidation {
public:
    explicit ImageViewInvalidation(std::span<const ImageViewId> views_) : views{views_} {
        if (views.size() >= HashThreshold) {
            hashed.reserve(views.size());
            hashed.insert(views.begin(), views.end());
        }
    }

    [[nodiscard]] bool Contains(ImageViewId id) const {
        return views.size() < HashThreshold ? std::ranges::find(views, id) != views.end()
                                            : hashed.contains(id);
    }

private:
    static constexpr size_t HashThreshold = 16;
    std::span<const ImageViewId> views;
    std::unordered_set<ImageViewId> hashed;
};

} // namespace VideoCommon
