#include "sonar_session.h"
#include "sonar_notifications.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <objbase.h>

namespace sd::native {
namespace {
struct CaseInsensitive {
    bool operator()(const std::wstring& a,const std::wstring& b) const { return CompareStringOrdinal(a.c_str(),int(a.size()),b.c_str(),int(b.size()),TRUE)==CSTR_LESS_THAN; }
};
std::mutex leases_mutex;
std::map<std::wstring,const void*,CaseInsensitive> leases;
bool Acquire(const SonarSessionConfig& config,const void* owner) {
    std::lock_guard<std::mutex> lock(leases_mutex);
    for(const auto* id:{&config.input,&config.output}) { const auto found=leases.find(*id);if(found!=leases.end() && found->second!=owner) return false; }
    const auto first=leases.emplace(config.input,owner);
    try { leases.emplace(config.output,owner); } catch(...) { if(first.second) leases.erase(first.first);throw; }
    return true;
}
void Release(const void* owner) {
    std::lock_guard<std::mutex> lock(leases_mutex);
    for(auto it=leases.begin();it!=leases.end();) if(it->second==owner) it=leases.erase(it);else ++it;
}
}
SonarSessionRuntime::SonarSessionRuntime(SonarSessionConfig config,SonarPlatformFactory factory):config_(std::move(config)) {
    state_.struct_size=sizeof(state_);state_.initialization_result=-1;
    wake_=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!wake_) throw Failure(SD_IO_ERROR,"Cannot create Sonar session wake event");
    try { auto ready=ready_.get_future();worker_=std::thread([this,factory=std::move(factory)]() mutable {Run(std::move(factory));});
        const auto result=ready.get();if(result<0) { worker_.join();throw Failure(result,LastError().c_str()); }
    } catch(...) { if(worker_.joinable()) worker_.join();CloseHandle(wake_);wake_=nullptr;throw; }
}
SonarSessionRuntime::~SonarSessionRuntime() {
    if(worker_.joinable()) {
        { std::lock_guard<std::mutex> lock(mutex_);terminating_=true;exit_requested_=true;
          std::lock_guard<std::mutex> guard(cancel_mutex_);if(cancellable_) cancellable_->CancelIo();SetEvent(wake_); }
        worker_.join();
    }
    if(wake_) CloseHandle(wake_);
}
void SonarSessionRuntime::Event(uint32_t type,int32_t value,sd_result result,const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    sd_sonar_event event{};event.struct_size=sizeof(event);event.type=type;event.value=value;event.result=result;event.sequence=++event_sequence_;
    std::snprintf(event.message,sizeof(event.message),"%s",message.c_str());
    if(events_.size()==128) { events_.pop_front();++state_.events_dropped; }
    events_.push_back(event);if(result<0) error_=message;events_ready_.notify_all();
}
void SonarSessionRuntime::Publish(const SonarPenController& core,SonarSessionPlatform& platform,bool running,sd_result result) {
    const auto& q=core.State();const auto low_frequency=platform.LowFrequencySaved();std::lock_guard<std::mutex> lock(mutex_);
    state_.running=running;state_.device_connected=platform.Connected();state_.calibrating=q.A&&running;state_.core_ready=q.r;
    state_.profile_enabled=q.S;state_.media_keys_requested=platform.MediaKeys();state_.raw_status=q.R;
    state_.initialization_result=q.Q;state_.record_error=q.m0;state_.last_result=result;state_.generation=q.w0;++state_.sequence;
    state_.pressure=q.m;state_.amplitude=q.a0;state_.frequency_hz=q.b0;state_.threshold=q.V;state_.minimum=q.w;state_.maximum=q.y;
    state_.quiet_initial=q.i0;state_.left_amplitude=q.g0;state_.quiet_between=q.f0;state_.right_amplitude=q.h0;
    state_.low_frequency_saved=low_frequency;
    pointer_state_={sizeof(pointer_state_),uint32_t(q.R==3 && q.D!=-1),q.D,q.E};
}
sd_result SonarSessionRuntime::Submit(Operation operation,std::string text,bool cancel,uint32_t code,uint32_t action) {
    auto command=std::make_shared<Command>();command->operation=operation;command->text=std::move(text);
    command->code=code;command->action=action;
    return Enqueue(command,cancel);
}
sd_result SonarSessionRuntime::Motion(SonarMotionEvent input,SonarMotionResult& output,int32_t& action_after) {
    auto command=std::make_shared<Command>();command->operation=Operation::Motion;command->motion=std::move(input);
    const auto result=Enqueue(command,false);
    if(result==SD_OK) { output=std::move(command->motion_result);action_after=command->motion.action; }
    return result;
}
sd_result SonarSessionRuntime::Enqueue(const std::shared_ptr<Command>& command,bool cancel) {
    auto result=command->result.get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(terminating_) return SD_DISCONNECTED;
        if(command->operation==Operation::Exit) terminating_=true;
        if(cancel) { std::lock_guard<std::mutex> guard(cancel_mutex_);if(cancellable_) cancellable_->CancelIo(); }
        commands_.push_back(command);SetEvent(wake_);
    }
    return result.get();
}
sd_result SonarSessionRuntime::ProfileReceive(std::string to,std::string sender,std::string text) {
    auto command=std::make_shared<Command>();command->operation=Operation::ProfileMessage;
    command->to=std::move(to);command->sender=std::move(sender);command->text=std::move(text);return Enqueue(command,false);
}
sd_result SonarSessionRuntime::ProfileRead(SonarPreferenceSnapshot& output) {
    auto command=std::make_shared<Command>();command->operation=Operation::ProfileRead;
    const auto result=Enqueue(command,false);if(result==SD_OK) output=std::move(command->profile);return result;
}
void SonarSessionRuntime::Run(SonarPlatformFactory factory) noexcept {
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);bool ready=false;
    std::unique_ptr<SonarSessionPlatform> platform;
    std::shared_ptr<Command> inflight;sd_result outer_failure=SD_OK;
    try {
        if(FAILED(com)) throw Failure(SD_IO_ERROR,"Cannot initialize Sonar MTA owner");
        platform=factory(config_,[this](uint32_t t,int32_t v,sd_result r,const std::string& m){Event(t,v,r,m);});
        if(!platform) throw Failure(SD_INTERNAL_ERROR,"Null Sonar platform");
        SonarPenController core(*platform);core.State().C=true;bool running=false,attempted=false,exit=false;sd_result last=SD_OK;
        platform->EnableNotifications(wake_);
        uint32_t pending_changes=0;uint64_t retry_changes=0;
        { std::lock_guard<std::mutex> lock(cancel_mutex_);cancellable_=platform.get(); }
        Publish(core,*platform,false,SD_OK);ready_.set_value(SD_OK);ready=true;
        const auto stop=[&]() {
            sd_result result=SD_OK;
            try { if(attempted) core.Stop(); }
            catch(const Failure& error) { Event(SD_SONAR_EVENT_ERROR,core.State().m0,error.status,error.what());result=error.status; }
            catch(const std::exception& error) { Event(SD_SONAR_EVENT_ERROR,core.State().m0,SD_INTERNAL_ERROR,error.what());result=SD_INTERNAL_ERROR; }
            platform->CleanupIo();
            if(auto error=platform->TakeError()) { Event(SD_SONAR_EVENT_ERROR,core.State().m0,error->result,error->message);result=error->result; }
            platform->ActiveScope(false);running=attempted=false;Release(this);return result;
        };
        const auto changes=[&]() {
            const auto incoming=platform->TakeDeviceChanges();
            if(incoming) { pending_changes|=incoming;retry_changes=0; }
            if(!pending_changes || GetTickCount64()<retry_changes) return false;
            const auto flags=pending_changes;pending_changes=0;
            // An interrupted capture may already have reported invalidation.
            // Preserve diagnostics while retaining the running/waiting session.
            if(auto error=platform->TakeError()) Event(SD_SONAR_EVENT_ERROR,core.State().m0,error->result,error->message);
            try {
                if(flags&SonarDeviceInvalidate) {
                    platform->InvalidateDevices();
                    if(running && core.State().H) {
                        core.HeadsetChanged(false);
                        core.Tick(); // Observe the native disconnected iteration before reconnecting.
                    }
                }
                const auto present=platform->RefreshPresence();
                if(running && core.State().H!=present) core.HeadsetChanged(present);
                last=SD_OK;
                if(auto error=platform->TakeError()) { last=error->result;Event(SD_SONAR_EVENT_ERROR,core.State().m0,last,error->message); }
                Event(SD_SONAR_EVENT_DEVICE,int32_t(present),last,{});
            } catch(const Failure& error) {
                last=error.status;Event(SD_SONAR_EVENT_ERROR,0,last,error.what());pending_changes|=SonarDeviceRefresh;retry_changes=GetTickCount64()+250;
            } catch(const std::exception& error) {
                last=SD_INTERNAL_ERROR;Event(SD_SONAR_EVENT_ERROR,0,last,error.what());pending_changes|=SonarDeviceRefresh;retry_changes=GetTickCount64()+250;
            }
            Publish(core,*platform,running,last);return true;
        };
        const auto invalidated=[&]() {
            platform->InvalidateDevices();
            if(running && core.State().H) { core.HeadsetChanged(false);core.Tick(); }
            if(auto cleanup=platform->TakeError()) Event(SD_SONAR_EVENT_ERROR,core.State().m0,cleanup->result,cleanup->message);
            // WASAPI can fail before MMDevice delivers the notification. Wait for
            // the inventory to settle instead of turning a transient removal into
            // permanent session failure or spinning on repeated activation errors.
            pending_changes|=SonarDeviceRefresh;retry_changes=GetTickCount64()+250;
        };
        while(!exit) {
            if(exit_requested_) { stop();Publish(core,*platform,false,last);break; }
            std::shared_ptr<Command> command;
            { std::lock_guard<std::mutex> lock(mutex_);if(!commands_.empty()) { command=commands_.front();commands_.pop_front(); } }
            if(command) {
                inflight=command;
                sd_result result=SD_OK;bool starting=false;
                try {
                    switch(command->operation) {
                    case Operation::Start:
                        if(running || attempted) throw Failure(SD_INVALID_ARGUMENT,"Stop the Sonar session before another Start");
                        if(!Acquire(config_,this)) throw Failure(SD_IO_ERROR,"Sonar audio endpoints are already owned by another SDK session");
                        starting=attempted=true;platform->ResetIo();platform->RefreshPresence();
                        { const auto initialized=core.Start();if(initialized) { result=SD_IO_ERROR;Event(SD_SONAR_EVENT_ERROR,initialized,result,"Sonar initialization rejected");stop(); }
                          else { running=true;platform->ActiveScope(true); } }
                        break;
                    case Operation::Stop:result=stop();break;
                    case Operation::Profile:
                        platform->SetProfileText(command->text);if(running) core.ApplyProfile(command->text);break;
                    case Operation::ProfileMessage:
                        result=core.ReceiveProfile(command->to,command->sender,command->text)?SD_OK:SD_IGNORED;break;
                    case Operation::ProfileRead:command->profile=platform->ProfileSnapshot();break;
                    case Operation::Refresh:
                        { const bool old=core.State().H,present=platform->RefreshPresence();
                          if(running && old!=present) core.HeadsetChanged(present);platform->ResetIo(); }
                        break;
                    case Operation::Key:result=running&&core.Key(command->code,command->action)?SD_OK:SD_IGNORED;break;
                    case Operation::Palm:core.State().k0=command->code!=0;break;
                    case Operation::Motion:
                        if(!running) command->motion_result={command->motion,false};
                        else {
                            struct Clock final:SonarMotionClock {
                                SonarPenController& core;SonarSessionPlatform& platform;HANDLE wake;
                                Clock(SonarPenController& c,SonarSessionPlatform& p,HANDLE w):core(c),platform(p),wake(w) {}
                                int64_t NowMillis() override { return SonarWallTimeMillis(); }
                                void WaitStep(int64_t deadline) override { platform.MotionProgress(core,deadline,wake); }
                            } clock(core,*platform,wake_);
                            command->motion_result=core.Motion(command->motion,clock);
                        }
                        break;
                    case Operation::Exit:result=stop();exit=true;break;
                    }
                } catch(const Failure& error) { result=error.status;Event(SD_SONAR_EVENT_ERROR,0,result,error.what());if(starting) stop(); }
                  catch(const std::exception& error) { result=SD_INTERNAL_ERROR;Event(SD_SONAR_EVENT_ERROR,0,result,error.what());if(starting) stop(); }
                if(command->operation==Operation::Refresh) platform->ResetIo();
                if(auto error=platform->TakeError()) {
                    result=error->result;Event(SD_SONAR_EVENT_ERROR,core.State().m0,result,error->message);
                    if(error->endpoint_invalidated && running) invalidated();else stop();
                }
                last=result;Publish(core,*platform,running,last);command->result.set_value(result);inflight.reset();continue;
            }
            if(changes()) continue;
            if(!running) {
                const auto now=GetTickCount64();const auto timeout=pending_changes?DWORD(retry_changes>now?std::min<uint64_t>(retry_changes-now,250):0):INFINITE;
                WaitForSingleObject(wake_,timeout);continue;
            }
            try {
                const auto delay=core.Tick();
                if(changes()) continue;
                if(auto error=platform->TakeError()) {
                    last=error->result;Event(SD_SONAR_EVENT_ERROR,core.State().m0,last,error->message);
                    if(error->endpoint_invalidated) invalidated();else stop();
                }
                Publish(core,*platform,running,last);
                if(running && delay>0) platform->WaitIo(uint32_t(delay),wake_);
                if(delay<0 && running) stop();
            } catch(const Failure& error) {
                last=error.status;Event(SD_SONAR_EVENT_ERROR,core.State().m0,last,error.what());
                const auto io=platform->TakeError();if(io && io->endpoint_invalidated) invalidated();else stop();
                Publish(core,*platform,running,last);
            }
              catch(const std::exception& error) { last=SD_INTERNAL_ERROR;Event(SD_SONAR_EVENT_ERROR,core.State().m0,last,error.what());stop();Publish(core,*platform,false,last); }
        }
    } catch(const Failure& error) {
        outer_failure=error.status;try { Event(SD_SONAR_EVENT_ERROR,0,error.status,error.what()); } catch(...) {}
        if(!ready) ready_.set_value(error.status);
    } catch(const std::bad_alloc&) {
        outer_failure=SD_OUT_OF_MEMORY;if(!ready) ready_.set_value(SD_OUT_OF_MEMORY);
    } catch(const std::exception& error) {
        outer_failure=SD_IO_ERROR;try { Event(SD_SONAR_EVENT_ERROR,0,SD_IO_ERROR,error.what()); } catch(...) {}
        if(!ready) ready_.set_value(SD_IO_ERROR);
    } catch(...) { outer_failure=SD_INTERNAL_ERROR;if(!ready) ready_.set_value(SD_INTERNAL_ERROR); }
    { std::lock_guard<std::mutex> lock(cancel_mutex_);cancellable_=nullptr; }
    if(platform) { platform->DisableNotifications();platform->CleanupIo(); }platform.reset();Release(this);
    if(inflight) inflight->result.set_value(outer_failure==SD_OK?SD_DISCONNECTED:outer_failure);
    { std::lock_guard<std::mutex> lock(mutex_);terminating_=true;state_.running=0;state_.media_keys_requested=0;
      if(outer_failure<0) state_.last_result=outer_failure;
      for(auto& command:commands_) command->result.set_value(SD_DISCONNECTED);commands_.clear();events_ready_.notify_all(); }
    if(SUCCEEDED(com)) CoUninitialize();
}
void SonarSessionRuntime::State(sd_sonar_state& state) { std::lock_guard<std::mutex> lock(mutex_);state=state_; }
void SonarSessionRuntime::PointerState(sd_sonar_pointer_state& state) { std::lock_guard<std::mutex> lock(mutex_);state=pointer_state_; }
sd_result SonarSessionRuntime::Poll(uint32_t timeout,sd_sonar_event& event) {
    std::unique_lock<std::mutex> lock(mutex_);
    if(events_.empty()) events_ready_.wait_for(lock,std::chrono::milliseconds(timeout),[&]{return !events_.empty()||terminating_;});
    event={};event.struct_size=sizeof(event);if(events_.empty()) return terminating_?SD_DISCONNECTED:SD_TIMEOUT;
    event=events_.front();events_.pop_front();return SD_OK;
}
std::string SonarSessionRuntime::LastError() { std::lock_guard<std::mutex> lock(mutex_);return error_; }
}
