#include <iostream>
#include <cassert>
namespace Core { class System {}; }
namespace Service::KernelHelpers { class ServiceContext {}; }
#include "core/hle/service/am/lifecycle_manager.cpp"
namespace Service {
Event::Event(KernelHelpers::ServiceContext&):m_event(nullptr){}
Event::~Event()=default;
void Event::Signal(){} void Event::Clear(){}
}
using namespace Service::AM;
int main() {
 Core::System s; Service::KernelHelpers::ServiceContext ctx; int cases=0;
 for(bool enabled:{false,true}) for(bool exit:{false,true}) for(bool pause:{false,true}) {
  LifecycleManager l(s,ctx,true);AppletMessage m{};
  l.SetActivityState(ActivityState::ForegroundVisible);l.UpdateRequestedFocusState();l.PopMessage(&m);l.GetAndClearFocusState();
  l.SetFocusStateChangedNotificationEnabled(enabled);
  if(pause) l.SetActivityState(ActivityState::ForegroundObscured);
#ifdef PATCHED
  l.SetShutdownFocusState(FocusState::NotInFocus);
#else
  l.SetFocusState(FocusState::NotInFocus);
#endif
  if(exit) l.RequestExit();
  bool updated=l.UpdateRequestedFocusState();l.PopMessage(&m);
  std::cout<<"enabled="<<enabled<<" exit="<<exit<<" pause="<<pause<<" first="<<int(m)<<" focus="<<int(l.GetAndClearFocusState())<<" overwritten="<<updated<<"\n";
#ifdef PATCHED
  assert(m==AppletMessage::FocusStateChanged);assert(!updated);
  if(exit) { l.PopMessage(&m);assert(m==AppletMessage::Exit); }
  assert(!l.HasPendingFocusStateChanged());
#endif
  cases++;
 }
 LifecycleManager fresh(s,ctx,true);assert(fresh.UpdateRequestedFocusState());assert(fresh.IsRunnable());
 std::cout<<"lifecycle cases="<<cases<<"; new instance foreground OK\n";
#ifdef PATCHED
 int suspended=0;
 for(auto activity:{ActivityState::ForegroundVisible,ActivityState::ForegroundObscured,ActivityState::BackgroundVisible,ActivityState::BackgroundObscured})
 for(bool always:{false,true}) {
  LifecycleManager l(s,ctx,true);l.SetActivityState(activity);l.SetOutOfFocusSuspendingEnabled(always);
  l.SetShutdownFocusState(FocusState::NotInFocus);
  bool before=l.IsRunnable();l.RequestExit();bool after=l.IsRunnable();
  std::cout<<"activity="<<int(activity)<<" always_suspend="<<always<<" runnable_during_focus_grace="<<before<<" runnable_after_exit="<<after<<"\n";
  if(!before)suspended++;
 }
 assert(suspended==0);std::cout<<"shutdown activity cases still suspended="<<suspended<<"/8\n";
#endif
}
