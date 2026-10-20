#pragma once
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
#include "yuzu_common/common_types.h"
#define LOG_DEBUG(...) ((void)0)
#define UNIMPLEMENTED_MSG(...) throw std::runtime_error("unsupported command")
namespace Tegra::Host1x {
struct Call {
    unsigned device, method, argument;
    bool operator==(const Call&) const = default;
};
class Host1x { public: std::vector<Call> calls; };
class Nvdec {
    Host1x& host;
public:
    explicit Nvdec(Host1x& host_) : host{host_} {}
    void ProcessMethod(u32 method, u32 argument) { host.calls.push_back({0, method, argument}); }
};
class Vic {
    Host1x& host;
public:
    enum class Method : u32 {};
    Vic(Host1x& host_, std::shared_ptr<Nvdec>) : host{host_} {}
    void ProcessMethod(Method method, u32 argument) { host.calls.push_back({1, u32(method), argument}); }
};
class Control {
    Host1x& host;
public:
    enum class Method : u32 {};
    explicit Control(Host1x& host_) : host{host_} {}
    void ProcessMethod(Method method, u32 argument) { host.calls.push_back({2, u32(method), argument}); }
};
class SyncptIncrManager {
public:
    explicit SyncptIncrManager(Host1x&) {}
    void Increment(u32) {}
    u32 IncrementWhenDone(u32, u32) { return 0; }
    void SignalDone(u32) {}
};
}
