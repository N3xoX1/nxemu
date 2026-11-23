// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstdlib>
#include "yuzu_video_core/query_cache/pending_results.h"

using VideoCommon::PendingResults;

namespace {
int failures = 0;

void Check(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

constexpr VAddr A = 0x10000;

void CheckExtent() {
    PendingResults results;
    results.Add(A, 16, 1, false);
    Check(results.Any(A, 4), "first word");
    Check(results.Any(A + 8, 8), "timestamp");
    Check(results.Any(A + 15, 1), "last byte");
    Check(!results.Any(A + 16, 4), "after the result");
    Check(!results.Any(A - 4, 4), "before the result");
    Check(results.Any(A - 4, 8), "read ending inside the result");
    results.Completed(A, 1);
    Check(!results.Any(A, 16), "written result");

    results.Add(A, 4, 2, false);
    Check(results.Any(A, 4), "short result");
    Check(!results.Any(A + 4, 4), "after a short result");
    results.Completed(A, 2);

    results.Add(8, 16, 3, false);
    Check(results.Any(0, 16), "result near address zero");
}

void CheckReplacedReportStillWritesTimestamp() {
    PendingResults results;
    results.Add(A, 16, 1, false);
    results.Add(A, 4, 2, false);
    results.Replaced(A, 1);
    Check(results.Any(A + 8, 4), "timestamp of a rewritten ordinary report");
    results.Completed(A, 2);
    Check(results.Any(A + 8, 4), "timestamp after the newer report is written");
    results.Completed(A, 1);
    Check(!results.Any(A, 16), "both written");
}

void CheckReplacedHeldPayloadSkipsWrite() {
    PendingResults results;
    results.Add(A, 16, 1, true);
    results.Add(A, 4, 2, false);
    results.Replaced(A, 1);
    Check(!results.Any(A + 8, 4), "timestamp of a rewritten held payload");
    Check(results.Any(A, 4), "newer report");
    results.Completed(A, 1);
    Check(results.Any(A, 4), "skipped write leaves the newer report pending");
    results.Completed(A, 2);
    Check(!results.Any(A, 4), "newer report written");
}

void CheckSameAddressEntries() {
    PendingResults results;
    results.Add(A, 4, 1, false);
    results.Add(A, 4, 2, false);
    results.Completed(A, 2);
    Check(results.Any(A, 4), "other entry at the same address");
    results.Completed(A, 3);
    Check(results.Any(A, 4), "unknown location");
    results.Completed(A, 1);
    Check(!results.Any(A, 4), "all written");
}
} // namespace

int main() {
    CheckExtent();
    CheckReplacedReportStillWritesTimestamp();
    CheckReplacedHeldPayloadSkipsWrite();
    CheckSameAddressEntries();
    if (failures != 0) {
        return EXIT_FAILURE;
    }
    std::puts("PASS: pending query results");
    return EXIT_SUCCESS;
}
