#pragma once
#include "frame_decoder.h"
#include <turbojpeg.h>

namespace sd::native {
// Native decoder helper recovered from SADecLibJpeg log strings. Handles are
// persistent per task slot and never used concurrently.
class SADecLibJpeg {
    tjhandle handle_ = nullptr;
    void Check(int result, const char* operation, uint32_t reason = 0);
public:
    SADecLibJpeg();
    ~SADecLibJpeg();
    SADecLibJpeg(const SADecLibJpeg&) = delete;
    SADecLibJpeg& operator=(const SADecLibJpeg&) = delete;
    DecodedFragment Decode(const protocol::FramebufferHeader&, const std::vector<uint8_t>&);
    std::vector<uint8_t> YuvRgbConvert(const uint8_t* const planes[3], const int strides[3], uint32_t width, uint32_t height);
};
}
