#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <random>
#include <tuple>

using u32=uint32_t;using u64=uint64_t;using Handle=u32;using Result=int;
constexpr Result ResultSuccess=0,ResultInvalidCurrentMemory=1,ResultInvalidHandle=2,
                 ResultInvalidState=3,ResultTerminationRequested=4,Pending=99;
namespace Svc {constexpr u32 HandleWaitMask=0x40000000,InvalidHandle=0;}
struct KProcessAddress {u64 value{};u64 GetValue()const{return value;}};
enum class ThreadState {Initialized,Runnable,Waiting,Terminated};
enum class ThreadWaitReasonForDebugging {ConditionVar};
struct KernelCore {u32 lock_word{};bool writable=true;int stores{},warnings{},lookups{};};
struct KThread {
    ThreadState state=ThreadState::Runnable;
    int queued{},wakeups{},closed{};u32 tag=2;Result result=Pending;
    bool IsTerminationRequested()const{return false;}
    ThreadState GetState()const{return state;}
    void AddWaiter(KThread*){++queued;}
    void Close(){++closed;}
    void SetUserAddressKey(KProcessAddress,u32){}
    template<class T>void BeginWait(T*){state=ThreadState::Waiting;}
    void SetWaitReasonForDebugging(ThreadWaitReasonForDebugging){}
    Result GetWaitResult()const{return result;}
    u64 GetThreadId()const{return tag;}
    u32 GetAddressKeyValue()const{return tag;}
    KProcessAddress GetAddressKey()const{return {0x1000};}
    void EndWait(Result value){result=value;++wakeups;state=ThreadState::Runnable;}
};
KThread owner,waiter,next_waiter;KernelCore* active_kernel{};
struct Object {KThread* ptr;KThread* ReleasePointerUnsafe(){return ptr;}};
struct Table {
    template<class T>Object GetObjectWithoutPseudoHandle(Handle handle){
        ++active_kernel->lookups;
        if(handle==owner.tag)return {&owner};
        if(handle==waiter.tag)return {&waiter};
        return {nullptr};
    }
};
struct Process {Table GetHandleTable(){return {};}} process;
Process& GetCurrentProcess(KernelCore&){return process;}
KThread* GetCurrentThreadPointer(KernelCore&){return &waiter;}
struct KScopedSchedulerLock {explicit KScopedSchedulerLock(KernelCore&){} };
struct ThreadQueueImplForKConditionVariableWaitForAddress {
    explicit ThreadQueueImplForKConditionVariableWaitForAddress(KernelCore&){}
};
struct KScheduler {static bool IsSchedulerLockedByCurrentThread(KernelCore&){return true;}};
bool ReadFromUser(KernelCore& k,u32* value,KProcessAddress){*value=k.lock_word;return true;}
bool WriteToUser(KernelCore& k,KProcessAddress,const u32* value){
    if(!k.writable)return false;
    k.lock_word=*value;++k.stores;return true;
}
bool UpdateLockAtomic(KernelCore& k,u32* previous,KProcessAddress,u32 zero,u32 mask){
    *previous=k.lock_word;k.lock_word=k.lock_word?(k.lock_word|mask):zero;++k.stores;return true;
}
#define ASSERT(x) do{if(!(x))std::abort();}while(0)
#define R_UNLESS(c,r) do{if(!(c))return r;}while(0)
#define R_SUCCEED_IF(c) do{if(c)return ResultSuccess;}while(0)
#define R_RETURN(r) return r
#define LOG_WARNING(...) (++active_kernel->warnings)
struct KConditionVariable {
    KernelCore& m_kernel;
    static Result WaitForAddress(KernelCore&,Handle,KProcessAddress,u32);
    void SignalImpl(KThread*);
    void SignalBefore(KThread*);
};
#include "wait-address.inc"
#include "signal-reference.inc"
#include "signal-current.inc"

unsigned checks{};
void Check(bool value){++checks;if(!value){std::fprintf(stderr,"FAIL check %u\n",checks);std::exit(1);}}
void Reset(KernelCore& k,u32 tag,ThreadState state){
    k={};active_kernel=&k;owner={};owner.tag=1;owner.state=state;
    waiter={};waiter.tag=2;waiter.state=ThreadState::Waiting;
    next_waiter={};next_waiter.tag=3;next_waiter.state=ThreadState::Waiting;k.lock_word=tag;
}
auto Trace(const KernelCore& k){
    return std::tuple{k.lock_word,k.stores,k.warnings,k.lookups,owner.queued,owner.closed,
                      waiter.result,waiter.wakeups,waiter.state};
}
int main(){
    KernelCore k;std::mt19937 random(0x51139);
    for(unsigned i=0;i<100000;++i){
        const auto state=static_cast<ThreadState>(random()%3);
        const u32 tags[]={0,1,1|Svc::HandleWaitMask,55};
        const u32 tag=tags[random()%4];
        Reset(k,tag,state);KConditionVariable cv{k};cv.SignalBefore(&waiter);const auto before=Trace(k);
        Reset(k,tag,state);cv.SignalImpl(&waiter);Check(Trace(k)==before);
        if(tag==0)Check(k.lookups==0&&k.stores==1&&k.warnings==0);
    }
    for(u32 tag:{1u,1u|Svc::HandleWaitMask}){
        Reset(k,tag,ThreadState::Terminated);KConditionVariable cv{k};cv.SignalImpl(&waiter);
        Check(k.lock_word==2&&owner.queued==0&&owner.closed==1);
        Check(waiter.wakeups==1&&waiter.result==ResultSuccess&&k.warnings==1);
        // A second signal must wait on the new LIVE owner, never steal its lock.
        cv.SignalImpl(&next_waiter);
        Check(k.lock_word==(2|Svc::HandleWaitMask)&&waiter.queued==1);
        Check(next_waiter.wakeups==0&&k.warnings==1&&owner.queued==0);
    }
    Reset(k,1,ThreadState::Terminated);k.writable=false;KConditionVariable cv{k};cv.SignalImpl(&waiter);
    Check(waiter.result==ResultInvalidCurrentMemory&&waiter.wakeups==1&&owner.closed==1&&owner.queued==0);
    Reset(k,1|Svc::HandleWaitMask,ThreadState::Terminated);
    Check(KConditionVariable::WaitForAddress(k,1,{0x1000},2)==ResultSuccess);
    Check(k.lock_word==2&&owner.queued==0&&owner.closed==1);
    std::printf("PASS %u checks: normal-path traces equal; abandoned condvar handoff, new live owner, memory error and direct wait\n",checks);
}
