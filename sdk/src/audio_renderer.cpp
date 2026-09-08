#include "audio_renderer.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace sd::native {
namespace {
std::string diagnostic(HRESULT hr,const char* operation) {
    char text[256]{};std::snprintf(text,sizeof(text),"%s failed: HRESULT 0x%08X",operation,unsigned(hr));return text;
}
void check(HRESULT hr,const char* operation) { if (FAILED(hr)) throw AudioFailure(hr,operation); }
constexpr GUID default_render={0xe6327cad,0xdcec,0x4949,{0xae,0x8a,0x99,0x1e,0x97,0x6a,0x79,0xd2}};
constexpr GUID pcm_subtype={1,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
constexpr GUID float_subtype={3,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
using Activate = HRESULT(WINAPI*)(LPCWSTR,REFIID,PROPVARIANT*,IActivateAudioInterfaceCompletionHandler*,IActivateAudioInterfaceAsyncOperation**);
Activate activation_api() {
    static Activate api=[] {
        // Activation can complete after a cancelled client has gone away. Its COM
        // ref keeps the callback object alive; pin code too until process exit.
        HMODULE self=nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&activation_api),&self)) throw AudioFailure(HRESULT_FROM_WIN32(GetLastError()),"Pin audio callback module");
        HMODULE module=LoadLibraryExW(L"Mmdevapi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) throw AudioFailure(HRESULT_FROM_WIN32(GetLastError()),"Load Mmdevapi.dll");
        const auto result=reinterpret_cast<Activate>(GetProcAddress(module,"ActivateAudioInterfaceAsync"));
        if (!result) { FreeLibrary(module); throw AudioFailure(E_NOINTERFACE,"ActivateAudioInterfaceAsync export"); }
        return result; // Optional audio platform remains resident with callbacks.
    }();
    return api;
}
}
AudioFailure::AudioFailure(HRESULT hr,const char* operation)
    : Failure(SD_IO_ERROR,diagnostic(hr,operation).c_str()),hresult(hr) {}
SAAudioRenderer::~SAAudioRenderer() { Stop(); }
ULONG SAAudioRenderer::Release() { const auto count=--refs_;if (!count) delete this;return count; }
HRESULT SAAudioRenderer::QueryInterface(REFIID iid,void** out) {
    if (!out) return E_POINTER;*out=nullptr;
    if (iid==__uuidof(IUnknown) || iid==__uuidof(IActivateAudioInterfaceCompletionHandler) || iid==__uuidof(IAgileObject)) {
        *out=static_cast<IActivateAudioInterfaceCompletionHandler*>(this);AddRef();return S_OK;
    }
    return E_NOINTERFACE;
}
HRESULT SAAudioRenderer::ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) {
    HRESULT result=E_FAIL;Microsoft::WRL::ComPtr<IUnknown> object;Microsoft::WRL::ComPtr<IAudioClient> client;
    const HRESULT call=operation->GetActivateResult(&result,&object);
    if (FAILED(call)) result=call;
    if (SUCCEEDED(result)) result=object.As(&client);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cancelled_) client_=std::move(client);
        activation_result_=result;activation_complete_=true;
    }
    changed_.notify_all();return S_OK;
}
void SAAudioRenderer::Initialize(const std::wstring& endpoint,uint32_t timeout) {
    if (cancelled_) throw Failure(SD_DISCONNECTED,"Audio activation cancelled");
    wchar_t default_id[40]{};StringFromGUID2(default_render,default_id,40);
    Microsoft::WRL::ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    check(activation_api()(endpoint.empty()?default_id:endpoint.c_str(),__uuidof(IAudioClient),nullptr,this,&operation),
          "SAAudioRenderer::PrivateAudioInterfaceInitializeAsync");
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!changed_.wait_for(lock,std::chrono::milliseconds(timeout),[this]{return activation_complete_||cancelled_;}))
            throw AudioFailure(HRESULT_FROM_WIN32(WAIT_TIMEOUT),"Audio activation timeout");
        if (cancelled_) throw Failure(SD_DISCONNECTED,"Audio activation cancelled");
        check(activation_result_,"SAAudioRenderer::ActivateCompleted");
    }
    PrivateFormatGetAndConvert();PrivateAudioClientInitialize(48000);
}
void SAAudioRenderer::PrivateFormatGetAndConvert() {
    WAVEFORMATEX* mix=nullptr;check(client_->GetMixFormat(&mix),"SAAudioRenderer::GetMixFormat");
    struct Free { WAVEFORMATEX* p;~Free(){CoTaskMemFree(p);} } free{mix};
    if (!mix || !mix->nChannels || mix->nChannels>8) throw Failure(SD_UNSUPPORTED,"Unsupported audio channel count");
    format_={};format_.Format=*mix;
    if (mix->wFormatTag==WAVE_FORMAT_EXTENSIBLE) {
        if (mix->cbSize<22) throw Failure(SD_UNSUPPORTED,"Truncated extensible audio format");
        format_=*reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix);
        if (format_.SubFormat!=pcm_subtype && format_.SubFormat!=float_subtype)
            throw Failure(SD_UNSUPPORTED,"Unsupported audio endpoint subtype");
        format_.SubFormat=pcm_subtype;format_.Samples.wValidBitsPerSample=16;
    } else if (mix->wFormatTag==WAVE_FORMAT_PCM || mix->wFormatTag==WAVE_FORMAT_IEEE_FLOAT) {
        format_.Format.wFormatTag=WAVE_FORMAT_PCM;format_.Format.cbSize=0;
    } else throw Failure(SD_UNSUPPORTED,"Unsupported audio endpoint format");
    format_.Format.wBitsPerSample=16;
    format_.Format.nBlockAlign=WORD(format_.Format.nChannels*2);
    format_.Format.nAvgBytesPerSec=format_.Format.nSamplesPerSec*format_.Format.nBlockAlign;
}
void SAAudioRenderer::PrivateAudioClientInitialize(uint32_t rate) {
    REFERENCE_TIME period=0;check(client_->GetDevicePeriod(nullptr,&period),"SAAudioRenderer::GetDevicePeriod");
    // Native RATEADJUST/minimum-duration profile. Shared mode requires periodicity 0.
    check(client_->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_RATEADJUST,period,0,&format_.Format,nullptr),
          "SAAudioRenderer::PrivateAudioClientInitialize");
    Microsoft::WRL::ComPtr<IAudioClockAdjustment> clock;
    check(client_->GetService(IID_PPV_ARGS(&clock)),"IAudioClockAdjustment GetService");
    check(clock->SetSampleRate(float(rate)),"IAudioClockAdjustment SetSampleRate");
    check(client_->GetBufferSize(&buffer_frames_),"SAAudioRenderer::GetBufferSize");
    check(client_->GetService(IID_PPV_ARGS(&render_)),"IAudioRenderClient GetService");
    if (!buffer_frames_) throw AudioFailure(E_UNEXPECTED,"Zero audio buffer");
}
void SAAudioRenderer::Start() {
    if (cancelled_) throw Failure(SD_DISCONNECTED,"Audio start cancelled");
    check(client_->Start(),"SAAudioRenderer::Start");started_=true;
}
std::vector<int16_t> SAAudioRenderer::ConvertStereo(const std::vector<uint8_t>& pcm,uint32_t channels) {
    if (pcm.size()%4 || !channels || channels>8) throw Failure(SD_INVALID_ARGUMENT,"Invalid PCM conversion input");
    const size_t frames=pcm.size()/4;std::vector<int16_t> result(frames*channels,0);
    for (size_t i=0;i<frames;++i) {
        uint16_t left,right;std::memcpy(&left,pcm.data()+i*4,2);std::memcpy(&right,pcm.data()+i*4+2,2);
        if (channels==1) { const uint16_t sum=uint16_t(left+right);std::memcpy(result.data()+i,&sum,2); }
        else { std::memcpy(result.data()+i*channels,&left,2);std::memcpy(result.data()+i*channels+1,&right,2); }
    }
    return result;
}
void SAAudioRenderer::Render(const std::vector<uint8_t>& pcm) {
    const auto samples=ConvertStereo(pcm,format_.Format.nChannels);
    const size_t frames=pcm.size()/4;size_t offset=0;
    while (offset<frames && !cancelled_) {
        UINT32 padding=0;check(client_->GetCurrentPadding(&padding),"SAAudioRenderer::GetCurrentPadding");
        if (padding>buffer_frames_) throw AudioFailure(E_UNEXPECTED,"Invalid audio padding");
        const UINT32 count=UINT32(std::min<size_t>(buffer_frames_-padding,frames-offset));
        if (!count) {
            std::unique_lock<std::mutex> lock(mutex_);changed_.wait_for(lock,std::chrono::milliseconds(1),[this]{return cancelled_.load();});continue;
        }
        BYTE* buffer=nullptr;check(render_->GetBuffer(count,&buffer),"SAAudioRenderer::GetBuffer");
        std::memcpy(buffer,samples.data()+offset*format_.Format.nChannels,size_t(count)*format_.Format.nBlockAlign);
        check(render_->ReleaseBuffer(count,0),"SAAudioRenderer::ReleaseBuffer");offset+=count;
    }
}
void SAAudioRenderer::Cancel() noexcept { cancelled_=true;changed_.notify_all(); }
void SAAudioRenderer::Stop() noexcept {
    Cancel();std::lock_guard<std::mutex> lock(mutex_);
    if (client_ && started_) client_->Stop();started_=false;
    render_.Reset();client_.Reset();buffer_frames_=0;
}
}
