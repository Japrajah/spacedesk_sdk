#pragma once
#include "frame_decoder.h"
#include "jpeg_decoder.h"
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <optional>

namespace sd::native {
// Optional platform loading preserves MJPEG/raw operation on systems without MF.
class PSMftPlatform {
    HMODULE module_ = nullptr;
    bool started_ = false;
public:
    decltype(&MFStartup) Startup = nullptr;
    decltype(&MFShutdown) Shutdown = nullptr;
    decltype(&MFTEnumEx) EnumEx = nullptr;
    decltype(&MFCreateMediaType) CreateMediaType = nullptr;
    decltype(&MFCreateSample) CreateSample = nullptr;
    decltype(&MFCreateMemoryBuffer) CreateMemoryBuffer = nullptr;
    void Initialize();
    void Reset() noexcept;
    ~PSMftPlatform() { Reset(); }
};
class PSMftMediaType {
    Microsoft::WRL::ComPtr<IMFMediaType> type_;
public:
    virtual ~PSMftMediaType() = default;
    void Create(PSMftPlatform&, REFGUID subtype, uint32_t width, uint32_t height, bool independent);
    IMFMediaType* Get() const noexcept { return type_.Get(); }
};
class PSMftMediaSample {
    Microsoft::WRL::ComPtr<IMFSample> sample_;
public:
    virtual ~PSMftMediaSample() = default;
    void Initialize(PSMftPlatform&, uint32_t bytes, int64_t time, int64_t duration);
    void CopyFrom(const std::vector<uint8_t>& bytes);
    IMFSample* Get() const noexcept { return sample_.Get(); }
};
class PSMftTransform {
    Microsoft::WRL::ComPtr<IMFTransform> transform_;
public:
    void Initialize(PSMftPlatform&);
    IMFTransform* Get() const noexcept { return transform_.Get(); }
    void Reset() noexcept { transform_.Reset(); }
};
class PSDecoderMftH264 {
    PSMftPlatform platform_;
    PSMftTransform transform_;
    SADecLibJpeg yuv_converter_;
    uint32_t width_ = 0, height_ = 0;
    int64_t sample_time_ = 0;
    static constexpr int64_t sample_duration_ = 166666; // Native fixed 60 Hz clock.
    void Initialize(uint32_t width, uint32_t height);
    void MediaTypeOutputFind(uint32_t failure_reason);
    std::optional<DecodedFragment> OutputGet(const protocol::FramebufferHeader& header);
public:
    PSDecoderMftH264();
    ~PSDecoderMftH264() { Reset(); }
    std::optional<DecodedFragment> Decode(const protocol::FramebufferHeader&, const std::vector<uint8_t>&);
    void Reset() noexcept;
};
}
