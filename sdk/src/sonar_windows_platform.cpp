#include "sonar_session.h"
#include "sonar_wasapi.h"
#include "sonar_notifications.h"
#include <algorithm>
#include <cstring>

namespace sd::native {
namespace {
class WindowsPlatform final:public SonarSessionPlatform {
    SonarSessionConfig config_;
    SonarEventSink sink_;
    SonarPreferences preferences_;
    std::unique_ptr<SonarAudioFactory> factory_;
    SonarWasapiAudio audio_;
    std::function<std::vector<SonarEndpointInfo>()> endpoints_;
    std::unique_ptr<SonarEndpointWatch> notifications_;
    bool connected_=false,active_scope_=false;
    bool media_=false,media_keys_=false,worker_=false,worker_alive_=false;
    std::array<int16_t,4096> motion_samples_{};
    std::optional<SonarSessionError> error_;
    template<class F> auto Io(F&& fn)->decltype(fn()) {
        try { return fn(); }
        catch(const SonarWasapiFailure& error) {
            if(!error_) error_=SonarSessionError{error.status,error.what(),error.hresult==AUDCLNT_E_DEVICE_INVALIDATED ||
                error.hresult==AUDCLNT_E_SERVICE_NOT_RUNNING || error.hresult==HRESULT_FROM_WIN32(ERROR_NOT_FOUND)};
            throw;
        }
        catch(const Failure& error) { if(error.status!=SD_DISCONNECTED && !error_) error_=SonarSessionError{error.status,error.what()};throw; }
        catch(const std::bad_alloc&) { if(!error_) error_=SonarSessionError{SD_OUT_OF_MEMORY,"Sonar audio allocation failed"};throw; }
        catch(const std::exception& error) { if(!error_) error_=SonarSessionError{SD_IO_ERROR,error.what()};throw; }
    }
public:
    WindowsPlatform(const SonarSessionConfig& config,SonarEventSink sink,std::unique_ptr<SonarAudioFactory> factory,
        std::function<std::vector<SonarEndpointInfo>()> endpoints):config_(config),sink_(std::move(sink)),
        preferences_(config.profiles,[this](sd_result result,const std::string& message){sink_(SD_SONAR_EVENT_PROFILE,0,result,message);}),
        factory_(std::move(factory)),audio_(*factory_),endpoints_(std::move(endpoints)) { RefreshPresence(); }
    ~WindowsPlatform() override { DisableNotifications(); }
    void EnableNotifications(HANDLE wake) override {
        auto registrar=factory_->Notifications();if(!registrar) return; // Explicit injected backends can remain manually driven.
        notifications_=std::make_unique<SonarEndpointWatch>(std::move(registrar),config_.input,config_.output,wake);
        audio_.NotificationEvent(notifications_->Event());
    }
    void DisableNotifications() noexcept override {
        audio_.NotificationEvent(nullptr);notifications_.reset();
    }
    uint32_t TakeDeviceChanges() noexcept override { return notifications_?notifications_->Take():0; }
    void InvalidateDevices() noexcept override {
        audio_.CaptureRelease();audio_.TrackRelease();
        if(FAILED(audio_.VolumeRestore()) && !error_) { try { error_=SonarSessionError{SD_IO_ERROR,"Failed to restore changed Sonar endpoint volume"}; } catch(...) {} }
        connected_=false;
    }
    void CancelIo() noexcept override { audio_.Cancel(); }
    void ResetIo() noexcept override { audio_.ResetCancellation(); }
    void WaitIo(uint32_t timeout,HANDLE wake) override { Io([&]{audio_.Wait(timeout,wake);}); }
    void CleanupIo() noexcept override {
        audio_.CaptureRelease();audio_.TrackRelease();const auto restored=audio_.VolumeRestore();active_scope_=false;
        if(FAILED(restored) && !error_) { try { error_=SonarSessionError{SD_IO_ERROR,"Failed to restore the Sonar endpoint volume"}; } catch(...) {} }
        worker_=worker_alive_=media_keys_=false;
    }
    void ActiveScope(bool active) noexcept override { active_scope_=active; }
    void MotionProgress(SonarPenController& core,int64_t deadline,HANDLE wake) override {
        if(!audio_.HasCapture()) { CreateRecorder(SonarRecordFormat(MinimumRecordBytes()));StartRecorder(); }
        if(Io([&]{return audio_.TryReadFull(motion_samples_.data(),uint32_t(motion_samples_.size()));}))
            core.SteadySamples(motion_samples_.data(),uint32_t(motion_samples_.size()));
        else {
            const auto remaining=deadline-SonarWallTimeMillis();
            if(remaining>0) Io([&]{audio_.WaitCapture(uint32_t(std::min<int64_t>(remaining,10)),wake);});
        }
    }
    void SetProfileText(std::string text) override { preferences_.SetProfile(text); }
    SonarPreferenceSnapshot ProfileSnapshot() const override { return preferences_.Snapshot(); }
    SonarProfileEnvironment* ProfileEnvironment() noexcept override { return &preferences_; }
    bool RefreshPresence() override {
        const auto endpoints=endpoints_();bool input=false,output=false;
        for(const auto& endpoint:endpoints) {
            if(endpoint.id==config_.input) { if(endpoint.flow!=1) throw Failure(SD_INVALID_ARGUMENT,"Sonar capture ID is not an input endpoint");input=true; }
            if(endpoint.id==config_.output) { if(endpoint.flow!=2) throw Failure(SD_INVALID_ARGUMENT,"Sonar render ID is not an output endpoint");output=true; }
        }
        const bool connected=input&&output;
        if(connected_ && !connected) InvalidateDevices();
        connected_=connected;return connected_;
    }
    bool Connected() const noexcept override { return connected_; }
    bool MediaKeys() const noexcept override { return media_keys_; }
    std::optional<SonarSessionError> TakeError() override { auto error=std::move(error_);error_.reset();return error; }
    bool HasAudioService() override { return true; }
    int32_t MaximumVolume() override { return 1000; }
    int32_t CurrentVolume() override { return connected_?Io([&]{return audio_.VolumeGet(config_.output);}):0; }
    void SetVolume(int32_t value) override { if(active_scope_ && connected_) Io([&]{audio_.VolumeSet(config_.output,value);}); }
    void AdjustVolumeUp() override { SetVolume(std::min(1000,CurrentVolume()+1)); }
    bool HasTrack() const override { return audio_.TrackState()!=0; }
    bool TrackPlaying() const override { return audio_.TrackPlaying(); }
    void CreateTrack(const SonarTrackFormat& format) override { Io([&]{audio_.TrackCreate(config_.output,format);}); }
    int32_t TrackState() override { return audio_.TrackState(); }
    int32_t WriteTrack(const int16_t* samples,uint32_t count) override { return Io([&]{return audio_.TrackWrite(samples,count);}); }
    int32_t SetTrackLoop(int32_t start,int32_t end,int32_t count) override { Io([&]{audio_.TrackLoop(start,end,count);});return 0; }
    float MaximumTrackGain() override { return 1; }
    int32_t SetTrackGain(float value) override { Io([&]{audio_.TrackGain(value);});return 0; }
    void PlayTrack() override { Io([&]{audio_.TrackPlay();}); }
    void StopTrack() override { Io([&]{audio_.TrackStop();}); }
    void ReleaseTrack() override { audio_.TrackRelease(); }
    bool HasRecorder() const override { return audio_.HasCapture(); }
    int32_t MinimumRecordBytes() const noexcept override { return 8192; }
    void CreateRecorder(const SonarRecordFormat& format) override { Io([&]{audio_.CaptureCreate(config_.input,format);}); }
    int32_t RecordingState() override { return audio_.Capturing()?3:1; }
    void StartRecorder() override { Io([&]{audio_.CaptureStart();}); }
    int32_t ReadBytes(uint8_t* bytes,uint32_t capacity) override {
        if(capacity%4) throw Failure(SD_INVALID_ARGUMENT,"Unaligned Sonar PCM read");
        std::vector<int16_t> values(capacity/2);
        const auto count=Io([&]{return audio_.Read(values.data(),uint32_t(values.size()),config_.timeout);});
        if(count) std::memcpy(bytes,values.data(),size_t(count)*2);return int32_t(count*2);
    }
    int32_t ReadSamples(int16_t* values,uint32_t capacity) override { return int32_t(Io([&]{return audio_.Read(values,capacity,config_.timeout);})); }
    void StopRecorder() override { Io([&]{audio_.CaptureStop();}); }
    void ReleaseRecorder() override { audio_.CaptureRelease(); }
    void ForgetRecorder() noexcept override { audio_.CaptureRelease(); }
    void SaveLowFrequency() override { preferences_.SaveLowFrequency(); }
    void SaveReadings(float,float,float,float,float) override {} // Numeric values remain available in the core snapshot.
    bool HonorOrHuawei() const override { return false; }
    void Status(int32_t value) override { sink_(SD_SONAR_EVENT_STATUS,value,SD_OK,{}); }
    void PostButton() override { sink_(SD_SONAR_EVENT_BUTTON,1,SD_OK,{}); }
    bool IsChromeOs() override { return false; }
    bool ModernAudioDevices() const noexcept override { return true; }
    bool LegacyProfileFiles() const noexcept override { return false; }
    std::vector<SonarAudioDevice> AudioDevices(uint32_t) override {
        if(!connected_) return {};
        return {{config_.kind==SD_SONAR_DEVICE_SP1?22u:3u,config_.kind==SD_SONAR_DEVICE_SP1?"SP1 (explicit SDK selection)":"Explicit analog SonarPen selection"}};
    }
    std::vector<std::optional<std::string>> UsbProducts() override { return {}; }
    bool LegacyHeadsetConnected() override { return connected_; }
    bool RecordingAllowed() override { return true; } // Windows enforces access during WASAPI activation; failures are I/O events.
    bool ProfileLoaded() override { return (preferences_.Flags()&SD_SONAR_PROFILE_LOADED)!=0; }
    std::string ReadProfile() override { return preferences_.Snapshot().profile; }
    bool LowFrequencySaved() override { return (preferences_.Flags()&SD_SONAR_PROFILE_LOW_FREQUENCY)!=0; }
    std::optional<std::string> ReadManualProfile() override { return {}; }
    bool CreateProfileWatcher() override { return false; }
    bool HasProfileWatcher() const override { return false; }
    void StartProfileWatcher() override {}
    void StopProfileWatcher() override {}
    void ForgetProfileWatcher() noexcept override {}
    void RegisterHeadset() override {} // Runtime owns one notification registration for the full handle lifetime.
    void UnregisterHeadset() override {}
    bool HasMediaSession() const override { return media_; }
    void CreateMediaSession(const char*) override { media_=true; }
    void MediaSessionFlags(uint32_t) override {}
    void MediaSessionCallback() override {}
    void MediaSessionPlayback(const SonarPlaybackState&) override {}
    void MediaSessionActive(bool value) override { media_keys_=value;sink_(SD_SONAR_EVENT_MEDIA_KEYS,int32_t(value),SD_OK,{}); }
    bool HasWorker() const override { return worker_; }
    bool WorkerAlive() override { return worker_alive_; }
    void CreateWorker(uint64_t) override { worker_=true;worker_alive_=false; }
    void StartWorker() override { worker_alive_=true; }
    void InterruptWorker() override { worker_alive_=false; }
    void ForgetWorker() noexcept override { worker_=false; }
};
}
std::unique_ptr<SonarSessionPlatform> MakeWindowsSonarPlatform(const SonarSessionConfig& config,SonarEventSink sink) {
    return std::make_unique<WindowsPlatform>(config,std::move(sink),MakeSonarAudioFactory(),SonarEndpoints);
}
std::unique_ptr<SonarSessionPlatform> MakeWindowsSonarPlatformWithAudio(const SonarSessionConfig& config,SonarEventSink sink,
    std::unique_ptr<SonarAudioFactory> factory,std::function<std::vector<SonarEndpointInfo>()> endpoints) {
    if(!factory || !endpoints) throw Failure(SD_INVALID_ARGUMENT,"Invalid Sonar audio adapter");
    return std::make_unique<WindowsPlatform>(config,std::move(sink),std::move(factory),std::move(endpoints));
}
}
