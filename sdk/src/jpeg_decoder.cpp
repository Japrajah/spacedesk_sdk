#include "jpeg_decoder.h"
#include <string>

namespace sd::native {
SADecLibJpeg::SADecLibJpeg() {
    handle_ = tj3Init(TJINIT_DECOMPRESS);
    if (!handle_) throw Failure(SD_OUT_OF_MEMORY, "Cannot initialize TurboJPEG decoder");
    // Header validation and an independent library-side pixel budget agree.
    if (tj3Set(handle_, TJPARAM_MAXPIXELS, int(max_pixels))) {
        tj3Destroy(handle_); handle_ = nullptr;
        throw Failure(SD_DECODE_ERROR, "Cannot configure TurboJPEG pixel budget");
    }
}
SADecLibJpeg::~SADecLibJpeg() { if (handle_) tj3Destroy(handle_); }
void SADecLibJpeg::Check(int result, const char* operation, uint32_t reason) {
    if (result) {
        const std::string text = std::string(operation) + ": " + tj3GetErrorStr(handle_);
        throw Failure(SD_DECODE_ERROR, text.c_str(), reason);
    }
}
DecodedFragment SADecLibJpeg::Decode(const protocol::FramebufferHeader& header, const std::vector<uint8_t>& bytes) {
    auto frame = FragmentDescription(header);
    if (bytes.size() < 4 || bytes[0] != 0xff || bytes[1] != 0xd8)
        throw Failure(SD_DECODE_ERROR, "MJPEG payload has no JPEG start marker");
    Check(tj3DecompressHeader(handle_, bytes.data(), bytes.size()), "SADecLibJpeg::Decode header");
    const uint32_t width = uint32_t(frame.update.right - frame.update.left), height = uint32_t(frame.update.bottom - frame.update.top);
    if (tj3Get(handle_, TJPARAM_JPEGWIDTH) != int(width) || tj3Get(handle_, TJPARAM_JPEGHEIGHT) != int(height))
        throw Failure(SD_PROTOCOL_ERROR, "JPEG dimensions do not match the update rectangle");
    frame.pixels.resize(size_t(width) * height * 4);
    Check(tj3Decompress8(handle_, bytes.data(), bytes.size(), frame.pixels.data(), int(width * 4), TJPF_BGRA), "SADecLibJpeg::Decode pixels");
    return frame;
}
std::vector<uint8_t> SADecLibJpeg::YuvRgbConvert(const uint8_t* const planes[3], const int strides[3], uint32_t width, uint32_t height) {
    Check(tj3Set(handle_, TJPARAM_SUBSAMP, TJSAMP_420), "SADecLibJpeg::YuvRgbConvert subsampling", 2);
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    Check(tj3DecodeYUVPlanes8(handle_, planes, strides, pixels.data(), int(width), int(width * 4), int(height), TJPF_BGRX),
        "SADecLibJpeg::YuvRgbConvert", 2);
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    return pixels;
}
}
