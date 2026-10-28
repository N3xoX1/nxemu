// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_common/startup_wait.h"
#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
void Check(bool value) { if (!value) throw std::runtime_error("Guest startup readiness regression"); }
int main() try {
    std::stop_source stop;
    std::atomic<bool> ready{false}, executed{false};
    auto start = std::async(std::launch::async,[&] {
        if (Common::WaitForStartup(stop.get_token(),[&] { return ready.load(); })) executed=true;
    });
    Check(start.wait_for(40ms)==std::future_status::timeout && !executed);
    ready=true;
    Check(start.wait_for(1s)==std::future_status::ready); start.get(); Check(executed);
    Check(Common::WaitForStartup(stop.get_token(),[] { return true; }));
    // Cancellation before readiness must never release guest execution.
    ready=false; executed=false;
    auto cancel = std::async(std::launch::async,[&] {
        if (Common::WaitForStartup(stop.get_token(),[&] { return ready.load(); })) executed=true;
    });
    Check(cancel.wait_for(40ms)==std::future_status::timeout);
    stop.request_stop();
    Check(cancel.wait_for(1s)==std::future_status::ready); cancel.get(); Check(!executed);
    Check(!Common::WaitForStartup(stop.get_token(),[] { return true; }));
    std::cout<<"PASS: guest execution waits for readiness; cancellation never starts the guest\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
