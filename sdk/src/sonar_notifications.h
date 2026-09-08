#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <memory>
#include <string>
#include <cstdint>

namespace sd::native {
enum : uint32_t { SonarDeviceRefresh=1,SonarDeviceInvalidate=2 };
// Actual IMMNotificationClient implementation owned by a Windows adapter.
// This is SDK platform code, not a claimed original APK class.
class SonarEndpointWatch {
    struct Signal;
    class Callback;
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<IMMNotificationClient> callback_;
    std::shared_ptr<Signal> signal_;
    bool registered_=false;
public:
    SonarEndpointWatch(Microsoft::WRL::ComPtr<IMMDeviceEnumerator>,std::wstring input,std::wstring output,HANDLE wake);
    ~SonarEndpointWatch();
    SonarEndpointWatch(const SonarEndpointWatch&)=delete;
    SonarEndpointWatch& operator=(const SonarEndpointWatch&)=delete;
    HANDLE Event() const noexcept;
    uint32_t Take() noexcept;
    HRESULT Stop() noexcept;
};
}
