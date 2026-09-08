#pragma once
#include "spacedesk_sdk.h"
#include "wire.h"
#include <windows.h>
#include <vector>
#include <stdexcept>
#include <memory>

namespace sd::native { class SADecLibJpeg; }
namespace sd {
struct Failure : std::runtime_error {
    sd_result status;
    uint32_t disconnect_reason;
    Failure(sd_result s, const char* message, uint32_t reason = 0)
        : std::runtime_error(message), status(s), disconnect_reason(reason) {}
};
struct DecodedFragment {
    uint32_t width = 0, height = 0;
    uint32_t compression = 0, chroma = 0, quality = 0;
    protocol::Rectangle update{};
    std::vector<uint8_t> pixels;
};
struct FrameSnapshot {
    uint32_t width = 0, height = 0, stride = 0;
    uint32_t compression = 0, chroma = 0, quality = 0;
    uint64_t sequence = 0;
    std::vector<uint8_t> pixels;
    void View(sd_frame& out) const noexcept;
};
// SDK CPU renderer boundary; native task scheduling is in frame_pipeline.h.
class FrameBuffer {
    std::vector<uint8_t> canvas_;
    uint32_t width_ = 0, height_ = 0;
    uint32_t compression_ = 0, chroma_ = 0, quality_ = 0;
    uint64_t sequence_ = 0;
public:
    void Apply(const DecodedFragment& fragment);
    std::shared_ptr<const FrameSnapshot> Present();
    void Clear();
};
uint32_t FragmentCount(const protocol::FramebufferHeader& header);
DecodedFragment FragmentDescription(const protocol::FramebufferHeader& header);
DecodedFragment DecodeFragment(const protocol::FramebufferHeader& header, const std::vector<uint8_t>& payload, native::SADecLibJpeg& jpeg);
}
