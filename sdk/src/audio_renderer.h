#pragma once
#include "frame_decoder.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <mutex>

namespace sd::native {
struct AudioFailure : Failure {
    HRESULT hresult;
    AudioFailure(HRESULT result,const char* operation);
};
// Confirmed native COM base/VMT: QueryInterface, AddRef, Release, ActivateCompleted.
class SAAudioRenderer final : public IActivateAudioInterfaceCompletionHandler {
    std::atomic<ULONG> refs_{1};
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_;
    std::condition_variable changed_;
    bool activation_complete_ = false, started_ = false;
    HRESULT activation_result_ = E_PENDING;
    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioRenderClient> render_;
    WAVEFORMATEXTENSIBLE format_{};
    UINT32 buffer_frames_ = 0;
    ~SAAudioRenderer(); // No destructor slot is added to the native four-slot COM VMT.
    void PrivateFormatGetAndConvert();
    void PrivateAudioClientInitialize(uint32_t server_rate);
public:
    SAAudioRenderer() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override;
    void Initialize(const std::wstring& endpoint,uint32_t timeout_ms);
    void Start();
    void Render(const std::vector<uint8_t>& stereo_pcm);
    void Stop() noexcept;
    void Cancel() noexcept;
    uint32_t Channels() const noexcept { return format_.Format.nChannels; }
    static std::vector<int16_t> ConvertStereo(const std::vector<uint8_t>& pcm,uint32_t channels);
};
}
