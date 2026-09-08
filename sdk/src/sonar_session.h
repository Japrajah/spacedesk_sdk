#pragma once
#include "sonar_controller.h"
#include "sonar_preferences.h"
#include "frame_decoder.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <atomic>

namespace sd::native {
struct SonarSessionConfig { std::wstring input,output;uint32_t kind=0,timeout=250;SonarPreferenceConfig profiles; };
struct SonarSessionError { sd_result result=SD_OK;std::string message;bool endpoint_invalidated=false; };
using SonarEventSink=std::function<void(uint32_t,int32_t,sd_result,const std::string&)>;
// SDK host adapter. This is not an added superclass in the recovered APK.
class SonarSessionPlatform:public SonarPlatform {
public:
    virtual void CancelIo() noexcept=0;
    virtual void ResetIo() noexcept=0;
    virtual void WaitIo(uint32_t,HANDLE)=0;
    virtual void CleanupIo() noexcept=0;
    virtual void ActiveScope(bool) noexcept=0;
    virtual void SetProfileText(std::string)=0;
    virtual SonarPreferenceSnapshot ProfileSnapshot() const=0;
    virtual bool RefreshPresence()=0;
    virtual bool Connected() const noexcept=0;
    virtual bool MediaKeys() const noexcept=0;
    virtual std::optional<SonarSessionError> TakeError()=0;
    virtual void EnableNotifications(HANDLE wake)=0;
    virtual void DisableNotifications() noexcept=0;
    virtual uint32_t TakeDeviceChanges() noexcept=0;
    virtual void InvalidateDevices() noexcept=0;
    virtual void MotionProgress(SonarPenController&,int64_t deadline,HANDLE wake)=0;
};
using SonarPlatformFactory=std::function<std::unique_ptr<SonarSessionPlatform>(const SonarSessionConfig&,SonarEventSink)>;
std::unique_ptr<SonarSessionPlatform> MakeWindowsSonarPlatform(const SonarSessionConfig&,SonarEventSink);
class SonarAudioFactory;
struct SonarEndpointInfo;
std::unique_ptr<SonarSessionPlatform> MakeWindowsSonarPlatformWithAudio(const SonarSessionConfig&,SonarEventSink,
    std::unique_ptr<SonarAudioFactory>,std::function<std::vector<SonarEndpointInfo>()>);
class SonarSessionRuntime {
    enum class Operation { Start,Stop,Profile,ProfileMessage,ProfileRead,Refresh,Key,Motion,Palm,Exit };
    struct Command { Operation operation;std::string text,to,sender;uint32_t code=0,action=0;SonarMotionEvent motion;SonarMotionResult motion_result;SonarPreferenceSnapshot profile;std::promise<sd_result> result; };
    SonarSessionConfig config_;
    HANDLE wake_=nullptr;
    std::thread worker_;
    std::mutex mutex_,cancel_mutex_;
    std::condition_variable events_ready_;
    std::deque<std::shared_ptr<Command>> commands_;
    std::deque<sd_sonar_event> events_;
    sd_sonar_state state_{};
    sd_sonar_pointer_state pointer_state_{sizeof(sd_sonar_pointer_state),0,-1,-1};
    std::string error_;
    SonarSessionPlatform* cancellable_=nullptr; // Protected until owner-thread destruction.
    bool terminating_=false;
    std::atomic<bool> exit_requested_{false};
    uint64_t event_sequence_=0;
    std::promise<sd_result> ready_;
    void Run(SonarPlatformFactory) noexcept;
    sd_result Submit(Operation,std::string={},bool cancel=false,uint32_t code=0,uint32_t action=0);
    sd_result Enqueue(const std::shared_ptr<Command>&,bool cancel);
    void Event(uint32_t,int32_t,sd_result,const std::string&);
    void Publish(const SonarPenController&,SonarSessionPlatform&,bool,sd_result);
public:
    explicit SonarSessionRuntime(SonarSessionConfig,SonarPlatformFactory=MakeWindowsSonarPlatform);
    ~SonarSessionRuntime();
    SonarSessionRuntime(const SonarSessionRuntime&)=delete;
    SonarSessionRuntime& operator=(const SonarSessionRuntime&)=delete;
    sd_result Start() { return Submit(Operation::Start); }
    sd_result Stop() { return Submit(Operation::Stop,{},true); }
    sd_result Profile(std::string text) { return Submit(Operation::Profile,std::move(text)); }
    sd_result ProfileReceive(std::string to,std::string sender,std::string text);
    sd_result ProfileRead(SonarPreferenceSnapshot&);
    sd_result Refresh() { return Submit(Operation::Refresh,{},true); }
    sd_result Key(uint32_t code,uint32_t action) { return Submit(Operation::Key,{},false,code,action); }
    sd_result Motion(SonarMotionEvent,SonarMotionResult&,int32_t& input_action_after);
    sd_result Palm(bool enabled) { return Submit(Operation::Palm,{},false,uint32_t(enabled)); }
    void State(sd_sonar_state&);
    void PointerState(sd_sonar_pointer_state&);
    sd_result Poll(uint32_t,sd_sonar_event&);
    std::string LastError();
};
}
