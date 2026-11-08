// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif
#include "yuzu_video_core/renderer_vulkan/driver_pipeline_cache_io.h"

namespace IO = Vulkan::DriverPipelineCacheIO;
namespace fs = std::filesystem;
constexpr std::array<char, 8> Magic{'y', 'u', 'z', 'u', 'v', 'k', 'c', 'h'};
constexpr std::uint32_t Version = 28;
using Native = IO::Detail::FileOperations;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Scratch {
    fs::path root = fs::temp_directory_path() /
        ("nxemu-driver-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { Check(fs::create_directory(root), "create scratch directory"); }
    ~Scratch() { std::error_code ignored; fs::remove_all(root, ignored); }
};
std::vector<char> Bytes(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    Check(file.is_open(), "open cache for verification");
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}
void Write(const fs::path& path, std::span<const char> bytes) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    Check(static_cast<bool>(file), "write test data");
}
void NoTemporary(const fs::path& root) {
    for (const auto& entry : fs::directory_iterator(root))
        Check(entry.path().filename().string().find(".tmp.") == std::string::npos,
              "failed save left a temporary file");
}
void TestRoundTrip(const fs::path& root) {
    const auto filename = root / fs::path{u8"cache-\u00e9-\u65e5\u672c.bin"};
    const std::vector<char> first{'o', '\0', 'l', 'd'}, second{'n', 'e', 'w'};
    IO::SaveAtomically(filename, Magic, Version, first);
    const auto bytes = Bytes(filename);
    Check(bytes.size() == Magic.size() + sizeof(Version) + first.size(), "wrapper length changed");
    Check(std::equal(Magic.begin(), Magic.end(), bytes.begin()), "magic changed");
    std::uint32_t stored_version{};
    std::memcpy(&stored_version, bytes.data() + Magic.size(), sizeof(stored_version));
    Check(stored_version == Version, "version encoding changed");
    Check(std::equal(first.begin(), first.end(), bytes.end() - first.size()), "payload changed");
    Check(IO::Load(filename, Magic, Version).payload == first, "load round trip");
    IO::SaveAtomically(filename, Magic, Version, second);
    const auto replaced = IO::Load(filename, Magic, Version);
    Check(replaced.status == IO::LoadStatus::Loaded && replaced.payload == second, "replace cache");
    NoTemporary(root);
    IO::SaveAtomically(filename, Magic, Version, {});
    const auto empty = IO::Load(filename, Magic, Version);
    Check(empty.status == IO::LoadStatus::Loaded && empty.payload.empty(), "empty payload");
    fs::remove(filename);
}
void TestReads(const fs::path& root) {
    const auto filename = root / "read.bin";
    Check(IO::Load(filename, Magic, Version).status == IO::LoadStatus::Unavailable, "missing cache");
    const std::vector<char> payload{'d', 'a', 't', 'a'};
    IO::SaveAtomically(filename, Magic, Version, payload);
    const auto complete = Bytes(filename);
    constexpr size_t HeaderSize = Magic.size() + sizeof(Version);
    for (size_t length = 0; length < HeaderSize; ++length) {
        Write(filename, std::span(complete).first(length));
        Check(IO::Load(filename, Magic, Version).status == IO::LoadStatus::Truncated,
              "short wrapper was accepted");
        Check(Bytes(filename).size() == length, "load deleted or rewrote the invalid cache");
    }
    auto bad_magic = complete;
    bad_magic[0] ^= 1;
    Write(filename, bad_magic);
    Check(IO::Load(filename, Magic, Version).status == IO::LoadStatus::InvalidHeader, "bad magic");
    Write(filename, complete);
    Check(IO::Load(filename, Magic, Version + 1).status == IO::LoadStatus::InvalidHeader, "old version");
    // Model a file shortened after tellg: every header/payload short read is rejected.
    for (size_t length = 0; length < complete.size(); ++length) {
        std::istringstream stream{std::string(complete.begin(), complete.begin() + length)};
        const auto result = IO::Detail::Read(stream, static_cast<std::streamoff>(complete.size()),
                                             Magic, Version);
        Check(result.status == IO::LoadStatus::ReadFailure && result.payload.empty(),
              "racing truncation returned partial data");
    }
    std::istringstream failed;
    Check(IO::Detail::Read(failed, -1, Magic, Version).status == IO::LoadStatus::ReadFailure,
          "failed file size accepted");
    fs::remove(filename);
}

enum class Failure { Open, FirstWrite, SecondWrite, PayloadWrite, Commit, Close, Replace };
struct Faults : Native {
    Failure failure;
    mutable unsigned writes{};
    explicit Faults(Failure value) : failure{value} {}
    std::FILE* OpenExclusive(const fs::path& path) const {
        if (failure == Failure::Open) { errno = EACCES; return nullptr; }
        return Native::OpenExclusive(path);
    }
    void Write(std::FILE* file, std::span<const char> bytes) const {
        ++writes;
        const unsigned fail_at = failure == Failure::FirstWrite ? 1U :
                                 failure == Failure::SecondWrite ? 2U :
                                 failure == Failure::PayloadWrite ? 3U : 0U;
        if (writes == fail_at) {
            Native::Write(file, bytes.first(bytes.size() / 2));
            throw std::system_error(ENOSPC, std::generic_category(), "injected short write");
        }
        Native::Write(file, bytes);
    }
    void Commit(std::FILE* file) const {
        if (failure == Failure::Commit)
            throw std::system_error(EIO, std::generic_category(), "injected commit failure");
        Native::Commit(file);
    }
    void Close(std::FILE* file) const {
        Native::Close(file);
        if (failure == Failure::Close)
            throw std::system_error(EIO, std::generic_category(), "injected close failure");
    }
    void Replace(const fs::path& from, const fs::path& to) const {
        if (failure == Failure::Replace)
            throw fs::filesystem_error("injected replacement failure", from, to,
                                       std::make_error_code(std::errc::permission_denied));
        Native::Replace(from, to);
    }
};
void TestFailures(const fs::path& root) {
    const auto filename = root / "preserve.bin";
    const std::vector<char> previous{'o', 'l', 'd'}, next{'n', 'e', 'w'};
    IO::SaveAtomically(filename, Magic, Version, previous);
    const auto original = Bytes(filename);
    for (auto failure : {Failure::Open, Failure::FirstWrite, Failure::SecondWrite,
                          Failure::PayloadWrite, Failure::Commit, Failure::Close, Failure::Replace}) {
        bool failed{};
        try { IO::SaveAtomically(filename, Magic, Version, next, Faults{failure}); }
        catch (const std::system_error&) { failed = true; }
        Check(failed, "injected failure did not propagate");
        Check(Bytes(filename) == original, "failed save damaged previous cache");
        Check(IO::Load(filename, Magic, Version).payload == previous, "previous cache not loadable");
        NoTemporary(root);
    }
    const auto blocked = root / "blocked";
    fs::create_directory(blocked);
    Write(blocked / "keep", previous);
    bool failed{};
    try { IO::SaveAtomically(blocked, Magic, Version, next); }
    catch (const fs::filesystem_error&) { failed = true; }
    Check(failed && Bytes(blocked / "keep") == previous, "rename failure changed destination");
    NoTemporary(root);
    fs::remove_all(blocked);
    fs::remove(filename);
    for (auto failure : {Failure::FirstWrite, Failure::Commit, Failure::Replace}) {
        bool failed{};
        try { IO::SaveAtomically(filename, Magic, Version, next, Faults{failure}); }
        catch (const std::system_error&) { failed = true; }
        Check(failed && !fs::exists(filename), "failed first save published an incomplete cache");
        NoTemporary(root);
    }
}
struct Collision : Native {
    mutable fs::path retained;
    std::FILE* OpenExclusive(const fs::path& path) const {
        if (retained.empty()) {
            retained = path;
            const std::array<char, 4> other{'k', 'e', 'e', 'p'};
            ::Write(path, other);
            errno = EEXIST;
            return nullptr;
        }
        return Native::OpenExclusive(path);
    }
};
void TestCollision(const fs::path& root) {
    const auto filename = root / "collision.bin";
    Collision collision;
    const std::array<char, 3> payload{'n', 'e', 'w'};
    IO::SaveAtomically(filename, Magic, Version, payload, collision);
    Check(Bytes(collision.retained) == std::vector<char>({'k', 'e', 'e', 'p'}),
          "another writer's staging file was changed");
    Check(IO::Load(filename, Magic, Version).payload == std::vector<char>(payload.begin(), payload.end()),
          "collision retry failed");
    fs::remove(collision.retained);
    fs::remove(filename);
    NoTemporary(root);
}
#ifndef _WIN32
struct Interrupted : Native {
    unsigned stop_after;
    mutable unsigned writes{};
    explicit Interrupted(unsigned count) : stop_after{count} {}
    void Write(std::FILE* file, std::span<const char> bytes) const {
        Native::Write(file, bytes);
        if (++writes == stop_after) {
            std::fflush(file);
            _exit(73); // Abrupt process exit: no close, commit, rename or C++ cleanup.
        }
    }
    void Commit(std::FILE* file) const {
        Native::Commit(file);
        if (stop_after == 4) _exit(73);
    }
    void Close(std::FILE* file) const {
        Native::Close(file);
        if (stop_after == 5) _exit(73);
    }
};
void TestInterruptedProcess(const fs::path& root) {
    const auto filename = root / "interrupted.bin";
    const std::array<char, 3> previous{'o', 'l', 'd'}, next{'n', 'e', 'w'};
    for (unsigned phase = 1; phase <= 5; ++phase) {
        IO::SaveAtomically(filename, Magic, Version, previous);
        const auto original = Bytes(filename);
        const pid_t child = fork();
        Check(child >= 0, "fork failed");
        if (child == 0) {
            IO::SaveAtomically(filename, Magic, Version, next, Interrupted{phase});
            _exit(1);
        }
        int status{};
        Check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 73,
              "writer did not stop in the expected phase");
        Check(Bytes(filename) == original, "interrupted process damaged old cache");
        Check(IO::Load(filename, Magic, Version).payload == std::vector<char>(previous.begin(), previous.end()),
              "old cache not readable after writer exit");
        IO::SaveAtomically(filename, Magic, Version, next); // Stale staging files do not interfere.
        Check(IO::Load(filename, Magic, Version).payload == std::vector<char>(next.begin(), next.end()),
              "recovery after writer exit failed");
    }
    fs::remove(filename);
    for (const auto& entry : fs::directory_iterator(root)) fs::remove(entry.path());
}
#endif
void TestConcurrentWriters(const fs::path& root) {
    const auto filename = root / "concurrent.bin";
    IO::SaveAtomically(filename, Magic, Version, std::vector<char>(8192, 'a'));
    std::atomic<unsigned> finished{}, failures{};
    std::thread reader{[&] {
        unsigned reads{};
        while (finished.load() < 4 || reads < 100) {
            try {
                const auto cache = IO::Load(filename, Magic, Version);
                if (cache.status != IO::LoadStatus::Loaded || cache.payload.size() != 8192 ||
                    !std::all_of(cache.payload.begin(), cache.payload.end(), [&](char value) {
                        return value == cache.payload.front();
                    })) ++failures;
            } catch (...) { ++failures; }
            ++reads;
        }
    }};
    std::vector<std::thread> writers;
    for (unsigned writer = 0; writer < 4; ++writer) {
        writers.emplace_back([&, writer] {
            try {
                const std::vector<char> payload(8192, static_cast<char>('b' + writer));
                for (unsigned i = 0; i < 20; ++i) {
                    try { IO::SaveAtomically(filename, Magic, Version, payload); }
                    catch (const fs::filesystem_error& error) {
#ifdef _WIN32
                        // CRT readers may deny FILE_SHARE_DELETE. A failed replace
                        // must preserve the old cache until its reader closes.
                        if (error.code().category() == std::system_category() &&
                            (error.code().value() == ERROR_ACCESS_DENIED ||
                             error.code().value() == ERROR_SHARING_VIOLATION)) continue;
#endif
                        ++failures;
                    }
                }
            } catch (...) { ++failures; }
            ++finished;
        });
    }
    for (auto& writer : writers) writer.join();
    reader.join();
    Check(failures == 0, "concurrent writer/reader observed an incomplete cache");
    const std::vector<char> final_payload(8192, 'z');
    IO::SaveAtomically(filename, Magic, Version, final_payload);
    Check(IO::Load(filename, Magic, Version).payload == final_payload,
          "save failed after concurrent readers closed");
    NoTemporary(root);
    fs::remove(filename);
}
int main() {
    Scratch scratch;
    TestRoundTrip(scratch.root);
    TestReads(scratch.root);
    TestFailures(scratch.root);
    TestCollision(scratch.root);
#ifndef _WIN32
    TestInterruptedProcess(scratch.root);
#endif
    TestConcurrentWriters(scratch.root);
}
