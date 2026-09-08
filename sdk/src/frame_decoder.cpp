#include "frame_decoder.h"
#include "jpeg_decoder.h"
#include <algorithm>
#include <cstring>

namespace sd {
uint32_t FragmentCount(const protocol::FramebufferHeader& h) {
    if (h.four_fragments > 1 || h.fragment_count < 0 || h.fragment_count > 4)
        throw Failure(SD_PROTOCOL_ERROR, "Invalid framebuffer fragment count");
    return h.fragment_count ? uint32_t(h.fragment_count) : h.four_fragments ? 4u : 1u;
}
DecodedFragment FragmentDescription(const protocol::FramebufferHeader& h) {
    DecodedFragment result;
    result.width = h.width; result.height = h.height; result.update = h.update;
    result.compression = uint32_t(h.compression); result.chroma = uint32_t(h.chroma); result.quality = h.quality;
    auto& rect = result.update;
    if (!(rect.left | rect.top | rect.right | rect.bottom)) {
        rect.right = int32_t(h.width); rect.bottom = int32_t(h.height);
    }
    FragmentCount(h);
    if (!valid_size(h.width, h.height) || rect.left < 0 || rect.top < 0 ||
        rect.left >= rect.right || rect.top >= rect.bottom ||
        uint32_t(rect.right) > h.width || uint32_t(rect.bottom) > h.height || h.pitch <= 0)
        throw Failure(SD_PROTOCOL_ERROR, "Invalid framebuffer dimensions, pitch or rectangle");
    return result;
}
DecodedFragment DecodeFragment(const protocol::FramebufferHeader& h, const std::vector<uint8_t>& payload, native::SADecLibJpeg& jpeg) {
    auto result = FragmentDescription(h);
    const auto& rect = result.update;
    const uint32_t width = uint32_t(rect.right - rect.left), height = uint32_t(rect.bottom - rect.top);
    const size_t stride = size_t(width) * 4;
    if (h.compression == protocol::Compression::OFF) {
        // Native SATaskQueuedDecoder forwards raw BGRA to the renderer.
        if (h.pixel_format != 21) throw Failure(SD_UNSUPPORTED, "Unsupported raw framebuffer pixel format");
        size_t input_stride = stride;
        if (payload.size() != stride * height) {
            if (size_t(h.pitch) < stride || payload.size() != size_t(h.pitch) * height)
                throw Failure(SD_PROTOCOL_ERROR, "Raw framebuffer byte count does not match its rectangle");
            input_stride = size_t(h.pitch);
        }
        result.pixels.resize(stride * height);
        for (uint32_t row = 0; row < height; ++row)
            std::memcpy(result.pixels.data() + row * stride, payload.data() + row * input_stride, stride);
        for (size_t i = 3; i < result.pixels.size(); i += 4) result.pixels[i] = 255;
        return result;
    }
    if (h.compression != protocol::Compression::MJPEG)
        throw Failure(SD_UNSUPPORTED, "Framebuffer codec is not implemented");
    if (!h.quality || h.quality > 100) throw Failure(SD_PROTOCOL_ERROR, "Invalid MJPEG quality");
    return jpeg.Decode(h, payload);
}

void FrameBuffer::Apply(const DecodedFragment& f) {
    compression_ = f.compression; chroma_ = f.chroma; quality_ = f.quality;
    if (width_ != f.width || height_ != f.height) {
        width_ = f.width; height_ = f.height;
        canvas_.assign(size_t(width_) * height_ * 4, 0);
        for (size_t i = 3; i < canvas_.size(); i += 4) canvas_[i] = 255;
    }
    const uint32_t width = uint32_t(f.update.right - f.update.left);
    const uint32_t height = uint32_t(f.update.bottom - f.update.top);
    for (uint32_t row = 0; row < height; ++row)
        std::memcpy(canvas_.data() + (size_t(f.update.top + row) * width_ + f.update.left) * 4,
            f.pixels.data() + size_t(row) * width * 4, size_t(width) * 4);
}
std::shared_ptr<const FrameSnapshot> FrameBuffer::Present() {
    auto frame = std::make_shared<FrameSnapshot>();
    frame->width = width_; frame->height = height_; frame->stride = width_ * 4;
    frame->compression = compression_; frame->chroma = chroma_; frame->quality = quality_;
    frame->pixels = canvas_;
    frame->sequence = ++sequence_;
    return frame;
}
void FrameBuffer::Clear() {
    std::fill(canvas_.begin(), canvas_.end(), uint8_t(0));
    for (size_t i = 3; i < canvas_.size(); i += 4) canvas_[i] = 255;
}
void FrameSnapshot::View(sd_frame& out) const noexcept {
    out.width = width; out.height = height; out.stride = stride;
    out.pixel_format = SD_PIXEL_BGRA8; out.sequence = sequence; out.pixels = pixels.data();
}
}
