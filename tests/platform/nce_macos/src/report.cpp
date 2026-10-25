#include "report.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mach/mach_time.h>

namespace ncev {

namespace {

std::vector<Check> g_checks;
std::string g_step;
std::string g_step_title;
bool g_any_blocker = false;

const char * OutcomeText(Outcome o) {
    switch (o) {
    case Outcome::Pass: return "PASS";
    case Outcome::Fail: return "FAIL";
    case Outcome::Info: return "INFO";
    }
    return "?";
}

const char * ClassText(Class c) {
    switch (c) {
    case Class::Blocker: return "bloquant";
    case Class::Change: return "changement";
    case Class::Info: return "info";
    }
    return "?";
}

} // namespace

std::string Fmt(const char * fmt, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return buffer;
}

uint64_t NowNs() {
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return mach_absolute_time() * tb.numer / tb.denom;
}

Stats Summarize(std::vector<uint64_t> s) {
    Stats r{};
    if (s.empty()) {
        return r;
    }
    std::sort(s.begin(), s.end());
    double sum = 0;
    for (uint64_t v : s) {
        sum += double(v);
    }
    r.median_ns = double(s[s.size() / 2]);
    r.p99_ns = double(s[std::min(s.size() - 1, s.size() * 99 / 100)]);
    r.max_ns = double(s.back());
    r.mean_ns = sum / double(s.size());
    return r;
}

std::string StatsText(const Stats & s) {
    return Fmt("médiane %.2f µs, p99 %.2f µs, max %.1f µs", s.median_ns / 1000.0, s.p99_ns / 1000.0,
               s.max_ns / 1000.0);
}

void BeginStep(const char * step, const char * title) {
    g_step = step;
    g_step_title = title;
    std::printf("\n==== Étape %s : %s ====\n", step, title);
    std::fflush(stdout);
}

void Record(const char * id, Outcome outcome, Class cls, const std::string & title, const std::string & detail,
            const std::string & change) {
    g_checks.push_back({g_step, id, outcome, cls, title, detail, change});
    std::printf("[%s] %-7s %s (%s)\n", OutcomeText(outcome), id, title.c_str(), ClassText(cls));
    if (!detail.empty()) {
        std::printf("          %s\n", detail.c_str());
    }
    if (outcome == Outcome::Fail && !change.empty()) {
        std::printf("          -> changement requis : %s\n", change.c_str());
    }
    std::fflush(stdout);
}

void EndStep() {
    bool blocker = false;
    std::vector<const Check *> changes;
    for (const Check & c : g_checks) {
        if (c.step != g_step || c.outcome != Outcome::Fail) {
            continue;
        }
        if (c.cls == Class::Blocker) {
            blocker = true;
        } else if (c.cls == Class::Change) {
            changes.push_back(&c);
        }
    }
    const char * verdict = blocker ? "NO-GO" : (changes.empty() ? "GO" : "GO SOUS CONDITIONS");
    std::printf("---- Verdict étape %s (%s) : %s\n", g_step.c_str(), g_step_title.c_str(), verdict);
    for (const Check * c : changes) {
        std::printf("     * [%s] %s\n", c->id.c_str(), c->change.c_str());
    }
    for (const Check & c : g_checks) {
        if (c.step == g_step && c.outcome == Outcome::Fail && c.cls == Class::Blocker) {
            std::printf("     ! bloquant [%s] %s\n", c.id.c_str(), c.title.c_str());
        }
    }
    g_any_blocker = g_any_blocker || blocker;
    std::fflush(stdout);
}

void WriteTsv() {
    const char * path = std::getenv("NCEV_TSV");
    if (path == nullptr) {
        return;
    }
    FILE * f = std::fopen(path, "a");
    if (f == nullptr) {
        return;
    }
    for (const Check & c : g_checks) {
        std::fprintf(f, "%s\t%s\t%s\t%s\t%s\t%s\t%s\n", c.step.c_str(), c.id.c_str(), OutcomeText(c.outcome),
                     ClassText(c.cls), c.title.c_str(), c.detail.c_str(), c.change.c_str());
    }
    std::fclose(f);
}

int ExitCode() {
    return g_any_blocker ? 2 : 0;
}

} // namespace ncev
