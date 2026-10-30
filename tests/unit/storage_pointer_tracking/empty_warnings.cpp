// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "yuzu_video_core/buffer_cache/empty_storage_buffer_warning.h"

int main() {
    using VideoCommon::EmptyStorageBufferWarningCounter;
    unsigned failures{};
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
    };
    EmptyStorageBufferWarningCounter counter;
    const std::array<u64, 4> descriptor{0x556600510ULL, 0, 0x510, 0};
    unsigned messages{};
    for (u64 i = 1; i <= 12600; ++i) {
        const u64 occurrence = counter.Observe(descriptor);
        if (occurrence) {
            ++messages;
            check(occurrence == i, "reported occurrence counts include omitted repetitions");
        }
    }
    check(messages == 14, "12600 identical failures produce 14 warning messages");
    for (size_t field = 0; field < descriptor.size(); ++field) {
        auto different = descriptor;
        ++different[field];
        check(counter.Observe(different) == 1, "each changed field is reported immediately");
        check(counter.Observe(descriptor) == 1, "return to previous descriptor is reported");
    }
    EmptyStorageBufferWarningCounter other;
    check(other.Observe(descriptor) == 1, "buffer caches have independent counters");
    check(other.Observe(descriptor) == 2, "second warning retains occurrence count");
    check(other.Observe(descriptor) == 0, "third identical warning is omitted");
    check(other.Observe(descriptor) == 4, "fourth warning reports continued failure");
    return failures ? 1 : 0;
}
