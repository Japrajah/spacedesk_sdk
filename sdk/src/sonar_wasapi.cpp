#include <initguid.h>
#include "sonar_wasapi.h"
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace sd::native {
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT hr,const char* operation) {
    if(FAILED(hr)) throw SonarWasapiFailure(hr,operation);
}
std::string message(HRESULT hr,const char* operation) {
    char text[192]{};std::snprintf(text,sizeof(text),"SonarPen %s: HRESULT 0x%08X",operation,unsigned(hr));return text;
}
HANDLE event_handle() {
    const auto result=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!result) throw Failure(SD_IO_ERROR,"Cannot create SonarPen audio event");return result;
}
WAVEFORMATEX format() { return WAVEFORMATEX{WAVE_FORMAT_PCM,2,44100,176400,4,16,0}; }
std::wstring id(IMMDevice* device) {
    LPWSTR value=nullptr;check(device->GetId(&value),"IMMDevice::GetId");
    std::wstring result=value?value:L"";CoTaskMemFree(value);return result;
}
ComPtr<IMMDeviceEnumerator> enumerator() {
    ComPtr<IMMDeviceEnumerator> result;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&result)),"MMDeviceEnumerator");
    return result;
}
class Factory final:public SonarAudioFactory {
    ComPtr<IMMDeviceEnumerator> devices_=enumerator();
public:
    ComPtr<IMMDeviceEnumerator> Notifications() override { return devices_; }
    ComPtr<IAudioClient> Client(const std::wstring& endpoint) override {
        ComPtr<IMMDevice> device;check(devices_->GetDevice(endpoint.c_str(),&device),"GetDevice");
        ComPtr<IAudioClient> result;check(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(result.GetAddressOf())),"Activate IAudioClient");
        return result;
    }
    ComPtr<IAudioEndpointVolume> Volume(const std::wstring& endpoint) override {
        ComPtr<IMMDevice> device;check(devices_->GetDevice(endpoint.c_str(),&device),"Get volume device");
        ComPtr<IAudioEndpointVolume> result;check(device->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(result.GetAddressOf())),"Activate endpoint volume");
        return result;
    }
};
constexpr DWORD stream_flags=AUDCLNT_STREAMFLAGS_EVENTCALLBACK|AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
}
SonarWasapiFailure::SonarWasapiFailure(HRESULT hr,const char* operation):Failure(SD_IO_ERROR,message(hr,operation).c_str()),hresult(hr) {}
std::vector<SonarEndpointInfo> SonarEndpoints() {
    const auto devices=enumerator();std::vector<SonarEndpointInfo> result;
    for(const auto flow:{eCapture,eRender}) {
        std::wstring default_id;ComPtr<IMMDevice> preferred;
        if(SUCCEEDED(devices->GetDefaultAudioEndpoint(flow,eConsole,&preferred))) default_id=id(preferred.Get());
        ComPtr<IMMDeviceCollection> collection;check(devices->EnumAudioEndpoints(flow,DEVICE_STATE_ACTIVE,&collection),"EnumAudioEndpoints");
        UINT count=0;check(collection->GetCount(&count),"Endpoint count");
        for(UINT i=0;i<count;++i) {
            ComPtr<IMMDevice> device;check(collection->Item(i,&device),"Endpoint item");
            SonarEndpointInfo info;info.flow=flow==eCapture?1u:2u;info.id=id(device.Get());info.is_default=info.id==default_id;
            ComPtr<IPropertyStore> properties;
            if(SUCCEEDED(device->OpenPropertyStore(STGM_READ,&properties))) {
                PROPVARIANT value{};
                if(SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName,&value)) && value.vt==VT_LPWSTR && value.pwszVal) info.name=value.pwszVal;
                PropVariantClear(&value);
                if(SUCCEEDED(properties->GetValue(PKEY_AudioEndpoint_FormFactor,&value)) && value.vt==VT_UI4) info.form_factor=value.ulVal;
                PropVariantClear(&value);
            }
            if(flow==eRender) {
                if(info.name.find(L"SP1")!=std::wstring::npos) info.source_type=22;
                else if(info.form_factor==Headset) info.source_type=3;
                else if(info.form_factor==Headphones) info.source_type=4;
            }
            result.push_back(std::move(info));
        }
    }
    return result;
}
std::unique_ptr<SonarAudioFactory> MakeSonarAudioFactory() { return std::make_unique<Factory>(); }
SonarWasapiAudio::SonarWasapiAudio(SonarAudioFactory& factory):factory_(factory) {
    cancel_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!cancel_) throw Failure(SD_IO_ERROR,"Cannot create SonarPen cancellation event");
    try { capture_event_=event_handle();render_event_=event_handle(); }
    catch(...) { if(capture_event_) CloseHandle(capture_event_);CloseHandle(cancel_);throw; }
}
SonarWasapiAudio::~SonarWasapiAudio() {
    CaptureRelease();TrackRelease();VolumeRestore();
    if(notification_event_) CloseHandle(notification_event_);
    CloseHandle(render_event_);CloseHandle(capture_event_);CloseHandle(cancel_);
}
void SonarWasapiAudio::Cancel() noexcept { SetEvent(cancel_); }
void SonarWasapiAudio::ResetCancellation() noexcept { ResetEvent(cancel_); }
void SonarWasapiAudio::NotificationEvent(HANDLE source) {
    HANDLE event=nullptr;
    if(source && !DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),&event,SYNCHRONIZE,FALSE,0))
        throw Failure(SD_IO_ERROR,"Cannot duplicate Sonar device-change event");
    if(notification_event_) CloseHandle(notification_event_);notification_event_=event;
}
void SonarWasapiAudio::CaptureCreate(const std::wstring& endpoint,const SonarRecordFormat& requested) {
    if(endpoint.empty() || requested.sample_rate!=44100 || requested.channels!=2 || requested.sample_bits!=16 ||
       requested.buffer_bytes<4 || requested.buffer_bytes>1024*1024) throw Failure(SD_INVALID_ARGUMENT,"Unsupported SonarPen capture format");
    CaptureRelease();auto client=factory_.Client(endpoint);if(!client) throw Failure(SD_IO_ERROR,"Null SonarPen capture client");const auto pcm=format();
    const REFERENCE_TIME duration=std::max<REFERENCE_TIME>(200000,(uint64_t(requested.buffer_bytes)*10000000)/176400);
    check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,stream_flags,duration,0,&pcm,nullptr),"Initialize capture PCM16/44100/stereo");
    check(client->SetEventHandle(capture_event_),"Set capture event");
    ComPtr<IAudioCaptureClient> capture;check(client->GetService(IID_PPV_ARGS(&capture)),"Get capture service");
    UINT32 frames=0;check(client->GetBufferSize(&frames),"Get capture buffer size");
    if(!frames || frames>44100*4) throw Failure(SD_UNSUPPORTED,"SonarPen capture buffer is outside the supported range");
    capture_client_=std::move(client);capture_=std::move(capture);capture_frames_=frames;ResetEvent(capture_event_);
}
void SonarWasapiAudio::CaptureStart() { if(!capture_client_) throw Failure(SD_IO_ERROR,"No SonarPen capture client");check(capture_client_->Start(),"Start capture");capture_running_=true; }
void SonarWasapiAudio::CaptureStop() {
    if(capture_client_) { check(capture_client_->Stop(),"Stop capture");capture_running_=false;check(capture_client_->Reset(),"Reset capture"); }
    incoming_.clear();
}
void SonarWasapiAudio::CaptureRelease() noexcept {
    if(capture_client_) { capture_client_->Stop();capture_client_->Reset(); }
    capture_.Reset();capture_client_.Reset();incoming_.clear();capture_running_=false;capture_frames_=0;
}
void SonarWasapiAudio::CaptureDrain() {
    if(!capture_) throw Failure(SD_IO_ERROR,"No SonarPen capture service");
    for(;;) {
        UINT32 available=0;check(capture_->GetNextPacketSize(&available),"Get next capture packet");if(!available) return;
        BYTE* data=nullptr;UINT32 frames=0;DWORD flags=0;
        const auto hr=capture_->GetBuffer(&data,&frames,&flags,nullptr,nullptr);
        if(hr==AUDCLNT_S_BUFFER_EMPTY) return;check(hr,"Get capture buffer");
        struct Release { IAudioCaptureClient* capture;UINT32 frames;bool armed=true;~Release(){if(armed) capture->ReleaseBuffer(frames);} } release{capture_.Get(),frames};
        if(!frames || frames>capture_frames_ || incoming_.size()+size_t(frames)*2>size_t(44100)*4*2)
            throw Failure(SD_IO_ERROR,"SonarPen capture queue overflow");
        if(flags&AUDCLNT_BUFFERFLAGS_SILENT) incoming_.insert(incoming_.end(),size_t(frames)*2,0);
        else {
            if(frames && !data) throw Failure(SD_IO_ERROR,"Null SonarPen capture data");
            const auto* samples=reinterpret_cast<const int16_t*>(data);
            incoming_.insert(incoming_.end(),samples,samples+size_t(frames)*2);
        }
        release.armed=false;check(capture_->ReleaseBuffer(frames),"Release capture buffer");
    }
}
uint32_t SonarWasapiAudio::Read(int16_t* output,uint32_t count,uint32_t timeout) {
    if(!output || !count || count%2 || count>65536 || !capture_running_) throw Failure(SD_INVALID_ARGUMENT,"Invalid SonarPen capture read");
    const auto interrupted=[&] { return WaitForSingleObject(cancel_,0)==WAIT_OBJECT_0 || (notification_event_ && WaitForSingleObject(notification_event_,0)==WAIT_OBJECT_0); };
    if(interrupted()) throw Failure(SD_DISCONNECTED,"SonarPen read interrupted");
    const auto deadline=GetTickCount64()+timeout;
    try { while(incoming_.size()<count) {
        if(interrupted()) throw Failure(SD_DISCONNECTED,"SonarPen read interrupted");
        PumpRender();CaptureDrain();if(incoming_.size()>=count) break;
        const auto now=GetTickCount64();if(now>=deadline) break;
        const HANDLE events[]{cancel_,capture_event_,render_event_,notification_event_};
        const DWORD wait=WaitForMultipleObjects(notification_event_?4:3,events,FALSE,DWORD(std::min<uint64_t>(deadline-now,100)));
        if(wait==WAIT_FAILED) throw Failure(SD_IO_ERROR,"SonarPen capture wait failed");
    } } catch(...) { incoming_.clear();throw; }
    if(interrupted()) throw Failure(SD_DISCONNECTED,"SonarPen read interrupted");
    const auto copied=uint32_t(std::min<size_t>(count,incoming_.size()));
    for(uint32_t i=0;i<copied;++i) { output[i]=incoming_.front();incoming_.pop_front(); }
    return copied;
}
bool SonarWasapiAudio::TryReadFull(int16_t* output,uint32_t count) {
    if(!output || !count || count%2 || count>65536 || !capture_running_) throw Failure(SD_INVALID_ARGUMENT,"Invalid cooperative Sonar read");
    if(WaitForSingleObject(cancel_,0)==WAIT_OBJECT_0 || (notification_event_ && WaitForSingleObject(notification_event_,0)==WAIT_OBJECT_0))
        throw Failure(SD_DISCONNECTED,"Sonar motion wait interrupted");
    try { PumpRender();CaptureDrain(); } catch(...) { incoming_.clear();throw; }
    if(incoming_.size()<count) return false;
    return Read(output,count,0)==count;
}
void SonarWasapiAudio::WaitCapture(uint32_t timeout,HANDLE wake) {
    PumpRender();
    HANDLE handles[5]{cancel_,capture_event_,render_event_};DWORD count=3;
    if(notification_event_) handles[count++]=notification_event_;
    if(wake) handles[count++]=wake;
    if(WaitForMultipleObjects(count,handles,FALSE,timeout)==WAIT_FAILED) throw Failure(SD_IO_ERROR,"Sonar motion audio wait failed");
}
void SonarWasapiAudio::TrackCreate(const std::wstring& endpoint,const SonarTrackFormat& requested) {
    if(endpoint.empty() || requested.sample_rate!=44100 || requested.channels!=2 || requested.sample_bits!=16 || requested.buffer_bytes!=176400 || requested.mode!=0)
        throw Failure(SD_INVALID_ARGUMENT,"Unsupported SonarPen static track format");
    TrackRelease();auto client=factory_.Client(endpoint);if(!client) throw Failure(SD_IO_ERROR,"Null SonarPen render client");const auto pcm=format();
    check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,stream_flags,1000000,0,&pcm,nullptr),"Initialize render PCM16/44100/stereo");
    check(client->SetEventHandle(render_event_),"Set render event");
    ComPtr<IAudioRenderClient> render;check(client->GetService(IID_PPV_ARGS(&render)),"Get render service");
    UINT32 frames=0;check(client->GetBufferSize(&frames),"Get render buffer size");
    if(!frames || frames>44100*4) throw Failure(SD_UNSUPPORTED,"SonarPen render buffer is outside the supported range");
    render_client_=std::move(client);render_=std::move(render);render_frames_=frames;track_state_=2;ResetEvent(render_event_);
}
int32_t SonarWasapiAudio::TrackWrite(const int16_t* samples,uint32_t count) {
    if(!render_client_ || render_running_ || !samples || !count || count>88200 || count%2) throw Failure(SD_INVALID_ARGUMENT,"Invalid SonarPen static track write");
    tone_.assign(samples,samples+count);position_=0;track_state_=1;return int32_t(count);
}
void SonarWasapiAudio::TrackLoop(int32_t start,int32_t end,int32_t count) {
    if(start<0 || end<=start || size_t(end)>tone_.size()/2 || count<-1) throw Failure(SD_INVALID_ARGUMENT,"Invalid SonarPen loop points");
    loop_start_=size_t(start);loop_end_=size_t(end);loops_=count;
}
void SonarWasapiAudio::TrackGain(float gain) {
    if(!std::isfinite(gain) || gain<0 || gain>1) throw Failure(SD_INVALID_ARGUMENT,"Invalid SonarPen track gain");gain_=gain;
}
void SonarWasapiAudio::Fill(int16_t* destination,uint32_t frames) {
    for(uint32_t i=0;i<frames;++i) {
        if(loops_ && position_==loop_end_) { position_=loop_start_;if(loops_>0) --loops_; }
        for(size_t channel=0;channel<2;++channel) destination[size_t(i)*2+channel]=position_<tone_.size()/2?int16_t(float(tone_[position_*2+channel])*gain_):0;
        if(position_<tone_.size()/2) ++position_;
    }
}
void SonarWasapiAudio::PumpRender() {
    if(!render_running_) return;
    UINT32 padding=0;check(render_client_->GetCurrentPadding(&padding),"Get render padding");
    if(padding>render_frames_) throw Failure(SD_IO_ERROR,"Invalid SonarPen render padding");
    const auto frames=render_frames_-padding;if(!frames) return;
    BYTE* data=nullptr;check(render_->GetBuffer(frames,&data),"Get render buffer");
    if(!data) { render_->ReleaseBuffer(frames,AUDCLNT_BUFFERFLAGS_SILENT);throw Failure(SD_IO_ERROR,"Null SonarPen render data"); }
    Fill(reinterpret_cast<int16_t*>(data),frames);check(render_->ReleaseBuffer(frames,0),"Release render buffer");
}
void SonarWasapiAudio::TrackPlay() {
    if(track_state_!=1) throw Failure(SD_IO_ERROR,"SonarPen track has no PCM data");
    render_running_=true;
    try { PumpRender();check(render_client_->Start(),"Start render"); }
    catch(...) { render_running_=false;throw; }
}
void SonarWasapiAudio::TrackStop() {
    if(render_client_) { check(render_client_->Stop(),"Stop render");render_running_=false;check(render_client_->Reset(),"Reset render"); }
    position_=0;
}
void SonarWasapiAudio::TrackRelease() noexcept {
    if(render_client_) { render_client_->Stop();render_client_->Reset(); }
    render_.Reset();render_client_.Reset();render_running_=false;render_frames_=0;track_state_=0;
    tone_.clear();position_=loop_start_=loop_end_=0;loops_=0;gain_=1;
}
void SonarWasapiAudio::Wait(uint32_t timeout,HANDLE wake) {
    const auto deadline=GetTickCount64()+timeout;
    for(;;) {
        if(WaitForSingleObject(cancel_,0)==WAIT_OBJECT_0 || (notification_event_ && WaitForSingleObject(notification_event_,0)==WAIT_OBJECT_0)) return;
        PumpRender();
        const auto now=GetTickCount64();if(now>=deadline) return;
        const HANDLE events[]{cancel_,render_event_,wake};
        const auto result=WaitForMultipleObjects(wake?3:2,events,FALSE,DWORD(std::min<uint64_t>(deadline-now,100)));
        if(result==WAIT_FAILED)
            throw Failure(SD_IO_ERROR,"SonarPen render wait failed");
        if(wake && result==WAIT_OBJECT_0+2) return;
    }
}
void SonarWasapiAudio::VolumeOpen(const std::wstring& endpoint) {
    if(endpoint.empty()) throw Failure(SD_INVALID_ARGUMENT,"Empty SonarPen volume endpoint");
    if(volume_endpoint_==endpoint && volume_) return;
    check(VolumeRestore(),"Restore previous endpoint volume");volume_=factory_.Volume(endpoint);if(!volume_) throw Failure(SD_IO_ERROR,"Null SonarPen endpoint volume");volume_endpoint_=endpoint;
}
int32_t SonarWasapiAudio::VolumeGet(const std::wstring& endpoint) {
    VolumeOpen(endpoint);float value=0;check(volume_->GetMasterVolumeLevelScalar(&value),"Get endpoint volume");
    return int32_t(std::floor(value*1000.0f+0.5f));
}
void SonarWasapiAudio::VolumeSet(const std::wstring& endpoint,int32_t value) {
    if(value<0 || value>1000) throw Failure(SD_INVALID_ARGUMENT,"SonarPen endpoint volume outside 0..1000");
    VolumeOpen(endpoint);float before=0;if(!volume_owned_) check(volume_->GetMasterVolumeLevelScalar(&before),"Snapshot endpoint volume");
    check(volume_->SetMasterVolumeLevelScalar(float(value)/1000.0f,nullptr),"Set endpoint volume");
    if(!volume_owned_) volume_before_=before;volume_owned_=true;
}
HRESULT SonarWasapiAudio::VolumeRestore() noexcept {
    const auto result=volume_ && volume_owned_?volume_->SetMasterVolumeLevelScalar(volume_before_,nullptr):S_OK;
    volume_owned_=false;volume_.Reset();volume_endpoint_.clear();
    return result;
}
}
