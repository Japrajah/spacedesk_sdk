#include <initguid.h>
#include "sonar_notifications.h"
#include "frame_decoder.h"
#include <atomic>
#include <cwchar>

namespace sd::native {
namespace {
void pin_callback_module() {
    static const bool pinned=[] {
        HMODULE module=nullptr;
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&pin_callback_module),&module)!=FALSE;
    }();
    if(!pinned) throw Failure(SD_IO_ERROR,"Cannot pin Sonar notification callback module");
}
}
struct SonarEndpointWatch::Signal {
    HANDLE event=nullptr,wake=nullptr;
    std::atomic<bool> enabled{true};
    std::atomic<uint32_t> pending{0},active{0};
    explicit Signal(HANDLE source_wake) {
        event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!event) throw Failure(SD_IO_ERROR,"Cannot create Sonar notification event");
        if(!DuplicateHandle(GetCurrentProcess(),source_wake,GetCurrentProcess(),&wake,EVENT_MODIFY_STATE,FALSE,0)) {
            CloseHandle(event);event=nullptr;throw Failure(SD_IO_ERROR,"Cannot duplicate Sonar notification wake event");
        }
    }
    void Send(uint32_t flags) noexcept {
        if(!enabled.load()) return;
        active.fetch_add(1);
        if(enabled.load()) { pending.fetch_or(flags);SetEvent(event);SetEvent(wake); }
        active.fetch_sub(1);
    }
    void Disable() noexcept {
        enabled=false;
        // Callbacks never wait. Only the owner waits for the short atomic/SetEvent
        // section before closing duplicate handles; late callbacks see disabled.
        while(active.load()!=0) Sleep(1);
        pending=0;
        if(wake) { CloseHandle(wake);wake=nullptr; }
        if(event) { CloseHandle(event);event=nullptr; }
    }
    ~Signal() { Disable(); }
};
class SonarEndpointWatch::Callback final:public IMMNotificationClient {
    std::atomic<ULONG> references_{1};
    std::shared_ptr<Signal> signal_;
    const std::wstring input_,output_;
    HRESULT Changed(LPCWSTR id) noexcept {
        // Self-reference keeps an in-flight call alive while the owner unregisters.
        AddRef();
        if(id && (input_==id || output_==id)) signal_->Send(SonarDeviceRefresh|SonarDeviceInvalidate);
        Release();return S_OK;
    }
    ~Callback()=default;
public:
    Callback(std::shared_ptr<Signal> signal,std::wstring input,std::wstring output):signal_(std::move(signal)),input_(std::move(input)),output_(std::move(output)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out) return E_POINTER;*out=nullptr;
        if(iid==__uuidof(IUnknown) || iid==__uuidof(IMMNotificationClient)) { *out=static_cast<IMMNotificationClient*>(this);AddRef();return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count=--references_;if(!count) delete this;return count; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id,DWORD) override { return Changed(id); }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) override { return Changed(id); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override { return Changed(id); }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow,ERole,LPCWSTR) override { return S_OK; } // Explicit IDs never follow defaults.
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR id,const PROPERTYKEY key) override {
        if((IsEqualGUID(key.fmtid,PKEY_AudioEngine_DeviceFormat.fmtid) && key.pid==PKEY_AudioEngine_DeviceFormat.pid) ||
           (IsEqualGUID(key.fmtid,PKEY_AudioEngine_OEMFormat.fmtid) && key.pid==PKEY_AudioEngine_OEMFormat.pid)) return Changed(id);
        return S_OK; // Friendly-name/volume changes do not restart calibration.
    }
};
SonarEndpointWatch::SonarEndpointWatch(Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator,std::wstring input,std::wstring output,HANDLE wake)
    :enumerator_(std::move(enumerator)),signal_(std::make_shared<Signal>(wake)) {
    if(!enumerator_ || input.empty() || output.empty()) throw Failure(SD_INVALID_ARGUMENT,"Invalid Sonar endpoint watcher");
    pin_callback_module();callback_.Attach(new Callback(signal_,std::move(input),std::move(output)));
    const auto result=enumerator_->RegisterEndpointNotificationCallback(callback_.Get());
    if(FAILED(result)) throw Failure(SD_IO_ERROR,"Cannot register Sonar endpoint notifications");
    registered_=true;
}
SonarEndpointWatch::~SonarEndpointWatch() { Stop(); }
HANDLE SonarEndpointWatch::Event() const noexcept { return signal_->event; }
uint32_t SonarEndpointWatch::Take() noexcept {
    if(!signal_->enabled) return 0;
    // Reset before exchange: a callback concurrent with this operation either
    // contributes flags here or leaves flags/event for the next owner iteration.
    ResetEvent(signal_->event);return signal_->pending.exchange(0);
}
HRESULT SonarEndpointWatch::Stop() noexcept {
    signal_->Disable();HRESULT result=S_OK;
    if(registered_) {
        result=enumerator_->UnregisterEndpointNotificationCallback(callback_.Get());registered_=false;
        if(FAILED(result) && result!=HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
            // The registrar may still hold its borrowed pointer. Retain a disabled
            // callback rather than release memory it can call; Signal handles are
            // already closed and the DLL is pinned. No MMDevice object is retained.
            callback_.Detach();
        }
    }
    callback_.Reset();enumerator_.Reset();return result;
}
}
