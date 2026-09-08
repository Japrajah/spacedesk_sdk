#include "h264_decoder.h"
#include <mferror.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <ctime>

namespace sd::native {
using Microsoft::WRL::ComPtr;
// Opt-in native diagnostics: record control results, never encoded image data.
static bool mft_diagnostics() {
    static const bool enabled=[] { char value[2]{};return GetEnvironmentVariableA("SPACEDESK_SDK_MFT_TRACE",value,2)==1 && value[0]=='1'; }();
    return enabled;
}
static void mft_trace(const char* operation,HRESULT result,int64_t a=0,int64_t b=0,int64_t c=0,int64_t d=0) {
    if(mft_diagnostics()) std::fprintf(stderr,"MFT pid=%lu tid=%lu %s hr=%08lX a=%lld b=%lld c=%lld d=%lld\n",
        GetCurrentProcessId(),GetCurrentThreadId(),operation,static_cast<unsigned long>(result),a,b,c,d);
}
static void check(HRESULT result, const char* operation, uint32_t reason = SD_DISCONNECT_H264_DECODE_FAILED) {
    if (FAILED(result)) {
        char message[200]{};
        std::snprintf(message, sizeof(message), "%s (HRESULT 0x%08lX)", operation, static_cast<unsigned long>(result));
        throw Failure(SD_DECODE_ERROR, message, reason);
    }
}
void PSMftPlatform::Initialize() {
    if (started_) return;
    module_ = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module_) throw Failure(SD_DECODE_ERROR, "Media Foundation is unavailable", SD_DISCONNECT_H264_INIT_FAILED);
#define LOAD_MF(Member, Export) Member = reinterpret_cast<decltype(Member)>(GetProcAddress(module_, Export)); \
    if (!Member) { Reset(); throw Failure(SD_DECODE_ERROR, "Media Foundation export unavailable: " Export, SD_DISCONNECT_H264_INIT_FAILED); }
    LOAD_MF(Startup, "MFStartup"); LOAD_MF(Shutdown, "MFShutdown"); LOAD_MF(EnumEx, "MFTEnumEx");
    LOAD_MF(CreateMediaType, "MFCreateMediaType"); LOAD_MF(CreateSample, "MFCreateSample"); LOAD_MF(CreateMemoryBuffer, "MFCreateMemoryBuffer");
#undef LOAD_MF
    check(Startup(MF_VERSION, MFSTARTUP_FULL), "MFStartup", SD_DISCONNECT_H264_INIT_FAILED);
    started_ = true;
}
void PSMftPlatform::Reset() noexcept {
    if (started_ && Shutdown) Shutdown();
    started_ = false;
    if (module_) FreeLibrary(module_);
    module_ = nullptr;
}
void PSMftMediaType::Create(PSMftPlatform& platform, REFGUID subtype, uint32_t w, uint32_t h, bool independent) {
    check(platform.CreateMediaType(&type_), "MFCreateMediaType", SD_DISCONNECT_H264_INIT_FAILED);
    check(type_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "MF major type", 1);
    check(type_->SetGUID(MF_MT_SUBTYPE, subtype), "MF subtype", 1);
    check(type_->SetUINT64(MF_MT_FRAME_SIZE, (uint64_t(w) << 32) | h), "MF frame size", 1);
    check(type_->SetUINT64(MF_MT_FRAME_RATE, (uint64_t(60) << 32) | 1), "MF frame rate", 1);
    check(type_->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, (uint64_t(1) << 32) | 1), "MF pixel aspect", 1);
    check(type_->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_MixedInterlaceOrProgressive), "MF interlace", 1);
    check(type_->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, independent ? 1 : 0), "MF independent flag", 1);
}
void PSMftMediaSample::Initialize(PSMftPlatform& platform, uint32_t size, int64_t time, int64_t duration) {
    check(platform.CreateSample(&sample_), "MFCreateSample");
    ComPtr<IMFMediaBuffer> buffer;
    check(platform.CreateMemoryBuffer(size, &buffer), "MFCreateMemoryBuffer");
    check(sample_->AddBuffer(buffer.Get()), "MF sample AddBuffer");
    check(sample_->SetSampleTime(time), "MF sample timestamp");
    check(sample_->SetSampleDuration(duration), "MF sample duration");
}
void PSMftMediaSample::CopyFrom(const std::vector<uint8_t>& bytes) {
    ComPtr<IMFMediaBuffer> buffer; check(sample_->GetBufferByIndex(0, &buffer), "MF input buffer");
    BYTE* data = nullptr; DWORD maximum = 0;
    check(buffer->Lock(&data, &maximum, nullptr), "MF input Lock");
    if (bytes.size() > maximum) { buffer->Unlock(); throw Failure(SD_DECODE_ERROR, "MF input allocation is too small", 2); }
    std::memcpy(data, bytes.data(), bytes.size());
    const auto result = buffer->SetCurrentLength(static_cast<DWORD>(bytes.size()));
    buffer->Unlock(); check(result, "MF input length");
}
void PSMftTransform::Initialize(PSMftPlatform& platform) {
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activators = nullptr; UINT32 count = 0;
    // The native code uses 0x41: SYNCMFT | SORTANDFILTER, not asynchronous MFTs.
    check(platform.EnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        &input, nullptr, &activators, &count), "MFTEnumEx H264", 1);
    HRESULT result = MF_E_TOPO_CODEC_NOT_FOUND;
    for (UINT32 i = 0; i < count; ++i) {
        if (!transform_) {
            result = activators[i]->ActivateObject(IID_PPV_ARGS(transform_.ReleaseAndGetAddressOf()));
            if(mft_diagnostics()) {
                GUID id{};wchar_t text[40]{};
                const auto queried=activators[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute,&id);StringFromGUID2(id,text,40);
                std::fprintf(stderr,"MFT pid=%lu tid=%lu PSMftTransform::Activate index=%u count=%u hr=%08lX clsid=%ls guid_hr=%08lX\n",
                    GetCurrentProcessId(),GetCurrentThreadId(),i,count,static_cast<unsigned long>(result),text,static_cast<unsigned long>(queried));
            }
        }
        activators[i]->Release();
    }
    CoTaskMemFree(activators);
    if (!transform_) check(FAILED(result) ? result : E_NOINTERFACE, "Activate H264 decoder", 1);
}
PSDecoderMftH264::PSDecoderMftH264() : sample_time_(10000000LL * std::clock()) {}
void PSDecoderMftH264::Initialize(uint32_t w, uint32_t h) {
    if (transform_.Get() && width_ == w && height_ == h) return;
    const auto clock_origin = sample_time_;
    Reset(); sample_time_ = clock_origin;
    platform_.Initialize(); transform_.Initialize(platform_);
    width_ = w; height_ = h;
    ComPtr<IMFAttributes> attributes;
    const auto attributes_result=transform_.Get()->GetAttributes(&attributes);
    mft_trace("PSDecoderMftH264::GetAttributes",attributes_result,w,h,sample_time_);
    if (SUCCEEDED(attributes_result)) {
        // GUID identities verified against codecapi.h; the original log repeats
        // NumWorkerThreads for the FastDecodeMode setting, but its GUID differs.
        static const GUID low_latency = {0x9c27891a,0xed7a,0x40e1,{0x88,0xe8,0xb2,0x27,0x27,0xa0,0x24,0xee}};
        static const GUID workers = {0x9561c3e8,0xea9e,0x4435,{0x9b,0x1e,0xa9,0x3e,0x69,0x18,0x94,0xd8}};
        static const GUID fast_decode = {0x6b529f7d,0xd3b1,0x49c6,{0xa9,0x99,0x9e,0xc6,0x91,0x1b,0xed,0xbf}};
        static const GUID acceleration = {0xf7db8a2f,0x4f48,0x4ee8,{0xae,0x31,0x8b,0x6e,0xbe,0x55,0x8a,0xe2}};
        const auto set=[&](const char* name,const GUID& id,UINT32 value) {
            const auto result=attributes->SetUINT32(id,value);
            if(mft_diagnostics()) {
                UINT32 actual=UINT32_MAX;const auto queried=attributes->GetUINT32(id,&actual);
                mft_trace(name,result,value,actual,queried);
            }
        };
        set("CODECAPI_AVLowLatencyMode",low_latency,1);set("CODECAPI_AVDecNumWorkerThreads",workers,8);
        set("CODECAPI_AVDecVideoFastDecodeMode",fast_decode,32);set("CODECAPI_AVDecVideoAcceleration_H264",acceleration,1);
    }
    PSMftMediaType input;
    input.Create(platform_, MFVideoFormat_H264, w, h, false);
    check(transform_.Get()->SetInputType(0, input.Get(), 0), "H264 SetInputType", 1);
    MediaTypeOutputFind(SD_DISCONNECT_H264_INIT_FAILED);
    check(transform_.Get()->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "MF begin streaming", 1);
    check(transform_.Get()->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "MF start stream", 1);
}
void PSDecoderMftH264::MediaTypeOutputFind(uint32_t failure_reason) {
    ComPtr<IMFMediaType> chosen;
    for (DWORD index = 0; index < 128; ++index) {
        ComPtr<IMFMediaType> type;
        HRESULT result = transform_.Get()->GetOutputAvailableType(0, index, &type);
        if (result == MF_E_NO_MORE_TYPES) break;
        check(result, "MF output type enumeration", failure_reason);
        GUID subtype{}; check(type->GetGUID(MF_MT_SUBTYPE, &subtype), "MF output subtype", failure_reason);
        if (subtype == MFVideoFormat_I420 || subtype == MFVideoFormat_IYUV) { chosen = type; break; }
    }
    if (!chosen) {
        PSMftMediaType fallback;
        fallback.Create(platform_, MFVideoFormat_I420, width_, height_, true);
        chosen = fallback.Get();
    }
    UINT64 dimensions = 0;
    check(chosen->GetUINT64(MF_MT_FRAME_SIZE, &dimensions), "MF output dimensions", failure_reason);
    const uint32_t w = uint32_t(dimensions >> 32), h = uint32_t(dimensions);
    if (w < width_ || h < height_ || w > ((width_ + 15) & ~15u) || h > ((height_ + 15) & ~15u))
        throw Failure(SD_PROTOCOL_ERROR, "H264 bitstream dimensions differ from framebuffer metadata", 2);
    MFVideoArea aperture{}; UINT32 aperture_size = 0;
    if (SUCCEEDED(chosen->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&aperture), sizeof(aperture), &aperture_size))) {
        if (aperture_size != sizeof(aperture) || aperture.OffsetX.value || aperture.OffsetX.fract ||
            aperture.OffsetY.value || aperture.OffsetY.fract || uint32_t(aperture.Area.cx) != width_ || uint32_t(aperture.Area.cy) != height_)
            throw Failure(SD_PROTOCOL_ERROR, "H264 display aperture differs from framebuffer metadata", 2);
    }
    check(transform_.Get()->SetOutputType(0, chosen.Get(), 0), "H264 SetOutputType", failure_reason);
}
void PSDecoderMftH264::Reset() noexcept {
    mft_trace("PSDecoderMftH264::Reset",S_OK,width_,height_,sample_time_,int64_t(uintptr_t(transform_.Get())));
    if (transform_.Get()) {
        transform_.Get()->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        transform_.Get()->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    transform_.Reset(); platform_.Reset(); width_ = height_ = 0; sample_time_ = 0;
}
std::optional<DecodedFragment> PSDecoderMftH264::OutputGet(const protocol::FramebufferHeader& header) {
    for (int change = 0; change < 4; ++change) {
        MFT_OUTPUT_STREAM_INFO info{}; check(transform_.Get()->GetOutputStreamInfo(0, &info), "MF output stream info");
        PSMftMediaSample supplied;
        if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            const uint64_t size = std::max<uint64_t>(info.cbSize, uint64_t(width_) * height_ * 4);
            if (size > max_payload) throw Failure(SD_DECODE_ERROR, "MF output buffer exceeds memory budget", 2);
            supplied.Initialize(platform_, uint32_t(size), sample_time_, sample_duration_);
        }
        MFT_OUTPUT_DATA_BUFFER output{}; output.pSample = supplied.Get(); DWORD status = 0;
        const auto result = transform_.Get()->ProcessOutput(0, 1, &output, &status);
        mft_trace("PSDecoderMftH264::ProcessOutput",result,info.dwFlags,info.cbSize,output.dwStatus,status);
        if (output.pEvents) output.pEvents->Release();
        ComPtr<IMFSample> sample;
        if (output.pSample == supplied.Get()) sample = supplied.Get();
        else if (output.pSample) sample.Attach(output.pSample);
        if (result == MF_E_TRANSFORM_STREAM_CHANGE) { MediaTypeOutputFind(SD_DISCONNECT_H264_DECODE_FAILED); continue; }
        if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) return std::nullopt;
        check(result, "H264 ProcessOutput");
        if (!sample) throw Failure(SD_DECODE_ERROR, "MF returned an empty output sample", 2);
        if(mft_diagnostics()) {
            LONGLONG time=0,duration=0;UINT32 discontinuity=UINT32_MAX;
            const auto queried=sample->GetSampleTime(&time);sample->GetSampleDuration(&duration);
            sample->GetUINT32(MFSampleExtension_Discontinuity,&discontinuity);
            mft_trace("PSDecoderMftH264::OutputSample",queried,time,duration,discontinuity);
        }
        ComPtr<IMFMediaBuffer> buffer; check(sample->ConvertToContiguousBuffer(&buffer), "MF output buffer");
        BYTE* data = nullptr; DWORD length = 0;
        check(buffer->Lock(&data, nullptr, &length), "MF output Lock");
        struct Unlock { IMFMediaBuffer* p; ~Unlock() { p->Unlock(); } } unlock{buffer.Get()};
        const uint32_t stride = (width_ + 15) & ~15u, rows = (height_ + 15) & ~15u;
        const size_t y_size = size_t(stride) * rows, uv_size = y_size / 4;
        if (y_size + 2 * uv_size > length) throw Failure(SD_DECODE_ERROR, "Truncated I420 output planes", 2);
        auto frame = FragmentDescription(header);
        const uint8_t* planes[3] = {data, data + y_size, data + y_size + uv_size};
        const int strides[3] = {int(stride), int(stride / 2), int(stride / 2)};
        frame.pixels = yuv_converter_.YuvRgbConvert(planes, strides, width_, height_);
        return frame;
    }
    throw Failure(SD_DECODE_ERROR, "Repeated H264 output format changes", 2);
}
std::optional<DecodedFragment> PSDecoderMftH264::Decode(const protocol::FramebufferHeader& header, const std::vector<uint8_t>& bytes) {
    auto description = FragmentDescription(header);
    if (FragmentCount(header) != 1 || description.update.left || description.update.top ||
        uint32_t(description.update.right) != header.width || uint32_t(description.update.bottom) != header.height)
        throw Failure(SD_PROTOCOL_ERROR, "H264 requires a complete framebuffer access unit", 2);
    if (!header.quality || header.quality > 100) throw Failure(SD_PROTOCOL_ERROR, "Invalid H264 quality", 2);
    if (bytes.size() < 4 || bytes[0] || bytes[1] || (bytes[2] != 1 && (bytes[2] || bytes[3] != 1)))
        throw Failure(SD_DECODE_ERROR, "H264 input is not an Annex B access unit", 2);
    Initialize(header.width, header.height);
    sample_time_ += sample_duration_;
    PSMftMediaSample input; input.Initialize(platform_, uint32_t(bytes.size()), sample_time_, sample_duration_); input.CopyFrom(bytes);
    auto result = transform_.Get()->ProcessInput(0, input.Get(), 0);
    mft_trace("PSDecoderMftH264::ProcessInput",result,int64_t(bytes.size()),sample_time_,sample_duration_);
    std::optional<DecodedFragment> latest;
    if (result == MF_E_NOTACCEPTING) {
        for (int i = 0; i < 16; ++i) { auto frame = OutputGet(header); if (!frame) break; latest = std::move(frame); }
        result = transform_.Get()->ProcessInput(0, input.Get(), 0);
        mft_trace("PSDecoderMftH264::ProcessInputRetry",result,int64_t(bytes.size()),sample_time_,sample_duration_);
    }
    check(result, "H264 ProcessInput");
    for (int i = 0; i < 16; ++i) { auto frame = OutputGet(header); if (!frame) break; latest = std::move(frame); }
    return latest; // NEED_MORE_INPUT is not a corrupt packet or fatal decoder failure.
}
}
