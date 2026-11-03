#include <cstdio>
#include <cstdlib>

// The production lifecycle only stores references to these in its constructor.
namespace Core {
class System {};
}
namespace Service::KernelHelpers {
class ServiceContext {};
}

#include "core/hle/service/am/lifecycle_manager.cpp"

// Exercise message ordering without creating kernel event handles.
namespace Service {
Event::Event(KernelHelpers::ServiceContext&) : m_event(nullptr) {}
Event::~Event() = default;
void Event::Signal() {}
void Event::Clear() {}
} // namespace Service

namespace {
using namespace Service::AM;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
}

void TestShutdownMessageOrder(Core::System& system, Service::KernelHelpers::ServiceContext& context) {
    for (bool notifications_enabled : {false, true}) {
        for (bool exit_requested : {false, true}) {
            for (bool obscured : {false, true}) {
                LifecycleManager lifecycle(system, context, true);
                lifecycle.UpdateRequestedFocusState();
                AppletMessage message{};
                lifecycle.PopMessage(&message);
                lifecycle.GetAndClearFocusState();
                lifecycle.SetFocusStateChangedNotificationEnabled(notifications_enabled);
                if (obscured) {
                    lifecycle.SetActivityState(ActivityState::ForegroundObscured);
                }
                lifecycle.SetShutdownFocusState(FocusState::NotInFocus);
                if (exit_requested) {
                    lifecycle.RequestExit();
                }
                Check(!lifecycle.UpdateRequestedFocusState(), "shutdown focus is preserved");
                Check(lifecycle.PopMessage(&message), "shutdown message is available");
                Check(message == AppletMessage::FocusStateChanged, "focus precedes exit");
                Check(lifecycle.GetAndClearFocusState() == FocusState::NotInFocus,
                      "guest observes loss of focus");
                Check(!lifecycle.HasPendingFocusStateChanged(), "focus message was consumed");
                if (exit_requested) {
                    Check(lifecycle.PopMessage(&message) && message == AppletMessage::Exit,
                          "exit remains available after focus");
                }
            }
        }
    }
}

void TestShutdownRunnable(Core::System& system, Service::KernelHelpers::ServiceContext& context) {
    for (auto activity : {ActivityState::ForegroundVisible, ActivityState::ForegroundObscured,
                          ActivityState::BackgroundVisible, ActivityState::BackgroundObscured}) {
        for (bool always_suspend : {false, true}) {
            LifecycleManager lifecycle(system, context, true);
            lifecycle.SetActivityState(activity);
            lifecycle.SetOutOfFocusSuspendingEnabled(always_suspend);
            lifecycle.SetShutdownFocusState(FocusState::NotInFocus);
            Check(lifecycle.IsRunnable(), "application can process focus before exit is requested");
            lifecycle.RequestExit();
            Check(lifecycle.IsRunnable(), "application remains runnable after exit is requested");
        }
    }
}

void TestNormalFocus(Core::System& system, Service::KernelHelpers::ServiceContext& context) {
    LifecycleManager lifecycle(system, context, true);
    Check(lifecycle.UpdateRequestedFocusState(), "new session can enter foreground");
    Check(lifecycle.IsRunnable(), "foreground session is runnable");
    AppletMessage message{};
    lifecycle.PopMessage(&message);
    lifecycle.GetAndClearFocusState();
    lifecycle.SetFocusStateChangedNotificationEnabled(false);
    lifecycle.SetFocusState(FocusState::NotInFocus);
    Check(!lifecycle.PopMessage(&message), "normal notifications can still be disabled");
    Check(lifecycle.UpdateRequestedFocusState(), "normal foreground updates are not frozen");
    Check(lifecycle.GetAndClearFocusState() == FocusState::InFocus, "normal focus is restored");
}
} // namespace

int main() {
    Core::System system;
    Service::KernelHelpers::ServiceContext context;
    TestShutdownMessageOrder(system, context);
    TestShutdownRunnable(system, context);
    TestNormalFocus(system, context);
    std::puts("PASS: shutdown focus ordering, runnable state and normal focus notifications");
}
