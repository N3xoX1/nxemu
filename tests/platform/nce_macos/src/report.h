// NCE macOS validation: result reporting.
//
// Each check has an outcome (PASS/FAIL/INFO) and a class:
//   Blocker - FAIL means no known exact workaround: the step is NO-GO.
//   Change  - FAIL means the existing NCE does not work as is on macOS, and the
//             listed change is required (its feasibility is checked by other tests).
//   Info    - measurement or observation, never changes the verdict.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ncev {

enum class Outcome { Pass, Fail, Info };
enum class Class { Blocker, Change, Info };

struct Check {
    std::string step;
    std::string id;
    Outcome outcome;
    Class cls;
    std::string title;
    std::string detail;
    std::string change; // required change when a Change check fails
};

void BeginStep(const char * step, const char * title);
void Record(const char * id, Outcome outcome, Class cls, const std::string & title, const std::string & detail,
            const std::string & change = {});

inline void Expect(const char * id, bool ok, Class cls, const std::string & title, const std::string & detail,
                   const std::string & change = {}) {
    Record(id, ok ? Outcome::Pass : Outcome::Fail, cls, title, detail, change);
}
inline void Note(const char * id, const std::string & title, const std::string & detail) {
    Record(id, Outcome::Info, Class::Info, title, detail);
}

// Prints the verdict of the current step: GO, GO SOUS CONDITIONS, or NO-GO.
void EndStep();
// Writes every check as TSV (one line per check) when NCEV_TSV is set.
void WriteTsv();
int ExitCode();

std::string Fmt(const char * fmt, ...) __attribute__((format(printf, 1, 2)));

uint64_t NowNs();

struct Stats {
    double median_ns;
    double p99_ns;
    double max_ns;
    double mean_ns;
};
Stats Summarize(std::vector<uint64_t> samples);
std::string StatsText(const Stats & s);

} // namespace ncev
