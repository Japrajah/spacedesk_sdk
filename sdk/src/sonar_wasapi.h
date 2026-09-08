#pragma once
#include "sonar_controller.h"
#include "frame_decoder.h"
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <deque>

namespace sd::native {
struct SonarWasapiFailure:Failure {
    HRESULT hresult;
    SonarWasapiFailure(HRESULT,const char* operation);
};
struct SonarEndpointInfo {
    std::wstring id,name;
    uint32_t flow=0,form_factor=UINT32_MAX,source_type=0;
    bool is_default=false;
};
std::vector<SonarEndpointInfo> SonarEndpoints(); // Metadata only; COM must be initialized.

// SDK platform adapter contracts, not recovered APK class names.
class SonarAudioFactory {
public:
    virtual ~SonarAudioFactory()=default;
    virtual Microsoft::WRL::ComPtr<IAudioClient> Client(const std::wstring& endpoint)=0;
    virtual Microsoft::WRL::ComPtr<IAudioEndpointVolume> Volume(const std::wstring& endpoint)=0;
    virtual Microsoft::WRL::ComPtr<IMMDeviceEnumerator> Notifications() { return {}; }
};
std::unique_ptr<SonarAudioFactory> MakeSonarAudioFactory();

// One MTA owner performs all audio calls. Cancel is the only cross-thread call;
// the owner must finish before this object/factory is destroyed.
class SonarWasapiAudio {
    SonarAudioFactory& factory_;
    HANDLE cancel_=nullptr,capture_event_=nullptr,render_event_=nullptr,notification_event_=nullptr;
    Microsoft::WRL::ComPtr<IAudioClient> capture_client_,render_client_;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_;
    Microsoft::WRL::ComPtr<IAudioRenderClient> render_;
    Microsoft::WRL::ComPtr<IAudioEndpointVolume> volume_;
    std::wstring volume_endpoint_;
    bool capture_running_=false,render_running_=false,volume_owned_=false;
    float volume_before_=0,gain_=1;
    uint32_t capture_frames_=0,render_frames_=0;
    std::deque<int16_t> incoming_;
    std::vector<int16_t> tone_;
    size_t position_=0,loop_start_=0,loop_end_=0;
    int32_t loops_=0,track_state_=0;
    void CaptureDrain();
    void VolumeOpen(const std::wstring&);
    void Fill(int16_t*,uint32_t frames);
public:
    explicit SonarWasapiAudio(SonarAudioFactory&);
    ~SonarWasapiAudio();
    SonarWasapiAudio(const SonarWasapiAudio&)=delete;
    SonarWasapiAudio& operator=(const SonarWasapiAudio&)=delete;
    void Cancel() noexcept;
    void ResetCancellation() noexcept;
    void NotificationEvent(HANDLE); // Owner installs/removes a duplicate of the watcher event.
    void CaptureCreate(const std::wstring&,const SonarRecordFormat&);
    void CaptureStart();
    void CaptureStop();
    void CaptureRelease() noexcept;
    bool HasCapture() const noexcept { return capture_client_!=nullptr; }
    bool Capturing() const noexcept { return capture_running_; }
    uint32_t Read(int16_t*,uint32_t count,uint32_t timeout_ms);
    bool TryReadFull(int16_t*,uint32_t count); // Keeps partial PCM queued for cooperative motion waits.
    void WaitCapture(uint32_t timeout_ms,HANDLE wake);
    void TrackCreate(const std::wstring&,const SonarTrackFormat&);
    int32_t TrackWrite(const int16_t*,uint32_t count);
    void TrackLoop(int32_t start,int32_t end,int32_t count);
    void TrackGain(float gain);
    void TrackPlay();
    void TrackStop();
    void TrackRelease() noexcept;
    int32_t TrackState() const noexcept { return track_state_; }
    bool TrackPlaying() const noexcept { return render_running_; }
    void PumpRender();
    void Wait(uint32_t timeout_ms,HANDLE wake=nullptr); // Service render events while the controller sleeps.
    int32_t VolumeGet(const std::wstring& endpoint); // 0..1000 normalized endpoint steps.
    void VolumeSet(const std::wstring& endpoint,int32_t value);
    HRESULT VolumeRestore() noexcept;
};
}
